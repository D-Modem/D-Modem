/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vcvm_startup.cpp - V.cvm's start-up after V.8, one DCE's: phase 2
 *                       (INFO0, L1 to L3, INFO1) and phase 3 (MP), as
 *                       vcvm_startup.h orders them
 *
 */

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include "bits.h"
#include "vcvm_startup.h"

namespace dsp {

/* the modes INFO0's octet 1 says this DCE will use (allowed) */
static uint8_t info0_modes(int a)
{
	return (uint8_t)(0xC0 | (a & VCVM_M_WIDE ? 0x20 : 0) |
			 (a & VCVM_M_WIDE400 ? 0x10 : 0) |
			 (a & VCVM_M_8DPSK ? 0x08 : 0) |
			 (a & VCVM_M_SYM22 ? 0x04 : 0) |
			 (a & VCVM_M_PARAM ? 0x01 : 0));
}

/* and back: what the peer will use */
static int info0_allowed(const VcvmInfo0 &i)
{
	uint8_t m = i.modes;
	return VCVM_M_ROBUST | VCVM_M_NARROW | (i.rings ? VCVM_M_RINGS : 0) |
	       (m & 0x20 ? VCVM_M_WIDE : 0) | (m & 0x10 ? VCVM_M_WIDE400 : 0) |
	       (m & 0x08 ? VCVM_M_8DPSK : 0) | (m & 0x04 ? VCVM_M_SYM22 : 0) |
	       (m & 0x01 ? VCVM_M_PARAM : 0);
}

/* the start-up of c: the modes allowed, INFO0, and (the calling DCE)
   INFO0 and the probing queued after the delay; 0, or -1 if the rate,
   the combs or the message mode won't go */
int VcvmStartup::init(const Config &c)
{
	c_ = c;
	log_.clear();
	state_ = RUNNING;
	why_ = "";
	t_ = 0;
	txq_.clear();
	txpos_ = 0;
	hist_.clear();
	hbase_ = 0;
	probe_end_tx_ = info1_end_tx_ = -1;
	got_mp_ = got_ack_ = sent_ack_ = mp_on_ = false;
	rtt_ms_ = -1;
	done_at_ = -1;
	last_rx_end_ = 0;
	mps_ = 0;
	rxl_ = txl_ = VcvmLoading();
	if (c.fs != 8000 && c.fs != 16000)
		return -1;
	allowed_ = vcvm_allowed(c.own, c.peer, c.modes);
	/* the message mode: P20 where both indicated the parametric mode */
	msgp_ = c.own.param && c.peer.param;
	info0_ = VcvmInfo0();
	info0_.modes = info0_modes(allowed_);
	info0_.combs = vcvm_probe_combs(c.fs, allowed_);
	info0_.rings = allowed_ & VCVM_M_RINGS;
	info0_.level = c.level;
	if (!info0_.combs)
		return -1;
	if (mtx_.init(c.fs, msgp_, c.calling) ||
	    mrx_.init(c.fs, msgp_, !c.calling))
		return -1;
	note("%s, %d Hz, messages in %s, modes %#x", c.calling ? "calling" :
	     "answering", c.fs, msgp_ ? "P20" : "V.21", allowed_);
	step_ = WAIT_INFO0;
	if (c.calling) {
		silence(c.delay_ms);
		uint8_t o[4];
		send_msg(o, info0_.encode(o));
		silence(100);
		send_probing();
		deadline_ = probe_end_tx_ + 6L * c.fs;
	} else
		deadline_ = 6L * c.fs;
	return 0;
}

/* a line to the log, the time in seconds before it */
void VcvmStartup::note(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	char t[32];
	snprintf(t, sizeof(t), "%7.3f ", (double)t_ / c_.fs);
	log_ += t;
	log_ += b;
	log_ += '\n';
}

/* FAILED (once, while running), why the reason */
void VcvmStartup::fail(const char *why)
{
	if (state_ != RUNNING)
		return;
	why_ = why;
	state_ = FAILED;
	note("failed: %s", why);
}

/* the sample after what is queued to send */
long VcvmStartup::queued() const
{
	return t_ + (long)(txq_.size() - txpos_);
}

/* ms of silence to the transmitter's queue */
void VcvmStartup::silence(int ms)
{
	txq_.insert(txq_.end(), (size_t)ms * c_.fs / 1000, 0.0f);
}

/* silence until sample at (what is queued counted) */
void VcvmStartup::silence_until(long at)
{
	long q = queued();
	if (at > q)
		txq_.insert(txq_.end(), (size_t)(at - q), 0.0f);
}

/* a message of n octets to the queue, in the message mode */
void VcvmStartup::send_msg(const uint8_t *m, int n)
{
	mtx_.send(m, n, txq_);
}

/* this DCE's L1 to L3 (the parts INFO0 said) to the queue, its end
   noted */
void VcvmStartup::send_probing()
{
	VcvmProbe p;
	p.fs = c_.fs;
	p.combs = info0_.combs;
	p.l2 = info0_.l2();
	p.l3 = info0_.l3();
	p.tap = c_.calling ? SCR_GPC : SCR_GPA;
	vcvm_probe_send(p, txq_);
	probe_end_tx_ = queued();
}

/* an MP (this DCE's loading, the round trip, the acknowledgement once
   the peer's came) and 100 ms of silence to the queue */
void VcvmStartup::send_mp()
{
	VcvmMp mp;
	mp.ack = got_mp_;
	mp.rtt = rtt_ms_ < 0 ? 255 : std::min(rtt_ms_ / 10, 254);
	mp.l = rxl_;
	uint8_t o[48];
	send_msg(o, mp.encode(o));
	silence(100);
	sent_ack_ = mp.ack && got_ack_;
	mps_++;
}

/* the received samples from `from` to now to a receiver */
template <class R> void VcvmStartup::replay(R &r, long from)
{
	from = std::max(from, hbase_);
	long e = hbase_ + (long)hist_.size();
	if (from < e)
		r.push(&hist_[(size_t)(from - hbase_)], (int)(e - from));
}

/* a message received, by the step: the peer's INFO0 (the round trip,
   its probing looked for), its INFO1 (the loading to send, ours after
   it), or an MP */
void VcvmStartup::on_msg(const VcvmMsg &m)
{
	last_rx_end_ = m.end;
	if (!m.ok) {
		note("a transmission without a good frame (%.3f to %.3f s)",
		     (double)m.start / c_.fs, (double)m.end / c_.fs);
		/* a lost INFO1 is MP's layout, which comes again until it is
		   acknowledged: on to the MP exchange, its timing kept for
		   the round trip */
		if (step_ != WAIT_INFO1)
			return;
	}
	switch (step_) {
	case WAIT_INFO0: {
		if (peer0_.decode(m.octets.data(), (int)m.octets.size())) {
			note("not an INFO0");
			return;
		}
		if (c_.calling && probe_end_tx_ >= 0)
			rtt_ms_ = (int)std::max(0L, (m.start - probe_end_tx_ -
						     c_.fs / 20) * 1000 / c_.fs);
		note("INFO0: modes %#x, combs %#x (%.3f to %.3f s)", peer0_.modes,
		     peer0_.combs, (double)m.start / c_.fs, (double)m.end / c_.fs);
		VcvmProbe p;
		p.fs = c_.fs;
		p.combs = peer0_.combs & vcvm_probe_combs(c_.fs, VCVM_M_ALL |
							   VCVM_M_WIDE400);
		p.l2 = peer0_.l2();
		p.l3 = peer0_.l3();
		p.tap = c_.calling ? SCR_GPA : SCR_GPC;
		prx_.init(p);
		prx_.start(m.end);
		replay(prx_, m.end);
		step_ = PROBE;
		/* the probing, 5 s at most, then (the calling DCE) the
		   answering DCE's INFO1 */
		deadline_ = t_ + (c_.calling ? 10L : 7L) * c_.fs;
		break;
	}
	case WAIT_INFO1: {
		VcvmMp mp;
		if (m.ok && mp.decode(m.octets.data(), (int)m.octets.size())) {
			note("not an INFO1");
			return;
		}
		if (m.ok) {
			txl_ = mp.l;
			note("INFO1: the peer asks for %s", txl_.str().c_str());
		} else
			note("INFO1 lost: the peer's loading from its MP");
		if (c_.calling) {
			silence_until(m.end + c_.fs / 20);
			VcvmMp i1;
			i1.rtt = rtt_ms_ < 0 ? 255 : std::min(rtt_ms_ / 10, 254);
			i1.l = rxl_;
			uint8_t o[48];
			send_msg(o, i1.encode(o));
			silence(100);
		} else
			rtt_ms_ = (int)std::max(0L, (m.start - info1_end_tx_ -
						     c_.fs / 20 - c_.fs / 100) *
						1000 / c_.fs);
		step_ = MP;
		mp_on_ = true;
		/* MP, 2.5 s at most each way (W200's), a few times */
		deadline_ = t_ + 20L * c_.fs;
		break;
	}
	case MP: {
		VcvmMp mp;
		if (mp.decode(m.octets.data(), (int)m.octets.size())) {
			note("not an MP");
			return;
		}
		txl_ = mp.l;
		got_mp_ = true;
		got_ack_ |= mp.ack;
		note("MP%s: %s", mp.ack ? " (ack)" : "", txl_.str().c_str());
		break;
	}
	case PROBE:
		break;
	}
}

/* what a block of samples leads to: the messages taken, the probing's
   outcome (the loading asked for, the answering DCE's INFO0, probing
   and INFO1), the MPs until both are acknowledged, the deadlines */
void VcvmStartup::step()
{
	VcvmMsg m;
	while (state_ == RUNNING && mrx_.get(&m))
		on_msg(m);
	if (state_ != RUNNING)
		return;
	if (step_ == PROBE) {
		if (prx_.failed()) {
			fail("the peer's probing not received");
			return;
		}
		if (prx_.done()) {
			const VcvmProbeMeas &pm = prx_.meas();
			rxl_ = vcvm_choose(pm, allowed_ & info0_allowed(peer0_));
			note("probing received (L3 %s, P20 %d/%d, P10 %d/%d in "
			     "error, c %.3f): asking for %s", pm.l3 ? "in" : "missing",
			     pm.e20, pm.n20, pm.e10, pm.n10, pm.cbar,
			     rxl_.str().c_str());
			if (c_.calling) {
				mrx_.restart(prx_.end());
				replay(mrx_, prx_.end());
			} else {
				silence_until(prx_.end() + c_.fs / 20);
				uint8_t o[48];
				send_msg(o, info0_.encode(o));
				silence(100);
				send_probing();
				silence(100);
				VcvmMp i1;
				i1.l = rxl_;
				send_msg(o, i1.encode(o));
				info1_end_tx_ = queued();
				silence(100);
				mrx_.restart(t_);
				deadline_ = info1_end_tx_ + 6L * c_.fs;
			}
			step_ = WAIT_INFO1;
		}
	}
	/* the next MP when ours is out, and the peer's that is coming in
	   (it may be the acknowledgement) is in */
	if (mp_on_ && txpos_ >= txq_.size() && !mrx_.busy()) {
		/* done once an MP of ours with the acknowledgement has gone
		   out after the peer's acknowledgement came: the peer has
		   one more if it lost the last */
		if (got_ack_ && sent_ack_) {
			state_ = DONE;
			done_at_ = t_;
			note("done: receiving %s, transmitting %s, round trip %d ms",
			     rxl_.str().c_str(), txl_.str().c_str(), rtt_ms_);
			return;
		}
		if (mps_ >= 10) {
			fail("no acknowledgement of MP");
			return;
		}
		send_mp();
	}
	if (t_ > deadline_)
		fail(step_ == WAIT_INFO0 ? "no INFO0" : step_ == PROBE ?
		     "the probing" : step_ == WAIT_INFO1 ? "no INFO1" :
		     "the MP exchange");
}

/* n samples from the line in (to the probing's receiver or the
   messages'), n out from the queue, 10 ms at a time */
void VcvmStartup::process(const float *in, float *out, int n)
{
	const int B = c_.fs / 100;
	while (n > 0) {
		int k = std::min(n, B);
		/* in */
		hist_.insert(hist_.end(), in, in + k);
		if ((long)hist_.size() > 3L * c_.fs) {
			long cut = (long)hist_.size() - 2L * c_.fs;
			hist_.erase(hist_.begin(), hist_.begin() + cut);
			hbase_ += cut;
		}
		if (state_ == RUNNING) {
			if (step_ == PROBE)
				prx_.push(in, k);
			else
				mrx_.push(in, k);
		}
		/* out */
		for (int i = 0; i < k; i++)
			out[i] = txpos_ < txq_.size() ? txq_[txpos_++] : 0.0f;
		if (txpos_ >= txq_.size() && txpos_ > 0) {
			txq_.clear();
			txpos_ = 0;
		}
		t_ += k;
		if (state_ == RUNNING)
			step();
		in += k;
		out += k;
		n -= k;
	}
}

} // namespace dsp
