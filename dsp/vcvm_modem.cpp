/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    vcvm_modem.cpp - one V.cvm DCE after V.8: the start-up, phase 4,
 *                     the renegotiations and the retrains (vcvm_modem.h)
 *
 */

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "dm_types.h"
#include "bits.h"
#include "vcvm_modem.h"

namespace dsp {

/* the DCE of c, the octets it sends from src, those it decodes to
   sink, its start-up begun; 0, or -1 if the configuration won't go */
int VcvmModem::init(const Config &c, Source src, Sink sink)
{
	c_ = c;
	src_ = std::move(src);
	sink_ = std::move(sink);
	log_.clear();
	why_ = "";
	t_ = 0;
	txq_.clear();
	txpos_ = 0;
	hist_.clear();
	hbase_ = 0;
	renegs_ = retrains_ = 0;
	want_reneg_ = want_retrain_ = false;
	msgp_ = c.su.own.param && c.su.peer.param;
	if (mtx_.init(c.su.fs, msgp_, c.su.calling) ||
	    mrx_.init(c.su.fs, msgp_, !c.su.calling))
		return -1;
	start_startup(false);
	return state_ == FAILED ? -1 : 0;
}

/* a line to the log, the time in seconds before it */
void VcvmModem::note(const char *fmt, ...)
{
	char b[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	char t[32];
	snprintf(t, sizeof(t), "%7.3f ", static_cast<double>(t_) / c_.su.fs);
	log_ += t;
	log_ += b;
	log_ += '\n';
}

/* FAILED (once), why the reason, the transmitter stopped */
void VcvmModem::fail(const char *why)
{
	if (state_ == FAILED)
		return;
	why_ = why;
	state_ = FAILED;
	tx_data_ = false;
	note("failed: %s", why);
}

/* ms of silence to the transmitter's queue */
void VcvmModem::silence(int ms)
{
	txq_.insert(txq_.end(), static_cast<size_t>(ms) * c_.su.fs / 1000,
		    0.0f);
}

/* the line's samples kept since sample from, pushed again to receiver r */
template <class R> void VcvmModem::replay(R &r, s32 from)
{
	from = std::max(from, hbase_);
	s32 e = hbase_ + static_cast<s32>(hist_.size());
	if (from < e)
		r.push(&hist_[static_cast<size_t>(from - hbase_)],
		       static_cast<int>(e - from));
}

/* see vcvm_modem.h */
bool VcvmModem::connected() const
{
	return state_ == DATA && tx_data_ && rx_on_ && drx_.confirmed();
}

/* a new start-up from phase 2 (after a retrain: the calling DCE's
   INFO0 3 s late); FAILED if its configuration won't go */
void VcvmModem::start_startup(bool after_retrain)
{
	VcvmStartup::Config k = c_.su;
	if (after_retrain && k.calling)
		k.delay_ms = 3000;
	su_.reset(new VcvmStartup);
	su_at_ = t_;
	if (su_->init(k)) {
		fail("the start-up's configuration");
		return;
	}
	state_ = STARTUP;
	tx_data_ = rx_on_ = mrx_on_ = false;
	note(after_retrain ? "retrain: phase 2" : "phase 2");
}

/* the transmitter into phase 4 with the loading the peer asked for
   (none: it stays silent) */
void VcvmModem::start_tx()
{
	tx_data_ = false;
	if (txl_.kind == VcvmLoading::NONE)
		return;
	if (dtx_.init(txl_, c_.su.fs, c_.su.calling ? SCR_GPC : SCR_GPA, src_)) {
		fail("the transmitter's loading");
		return;
	}
	dtx_.start(txq_);
	tx_data_ = true;
}

/* the receivers from sample from: phase 4's with this DCE's loading,
   the messages' for a further MP, the kept samples replayed */
void VcvmModem::start_rx(s32 from)
{
	rx_on_ = false;
	rx_from_ = from;
	if (rxl_.kind != VcvmLoading::NONE) {
		if (drx_.init(rxl_, c_.su.fs, c_.su.calling ? SCR_GPA : SCR_GPC,
			      sink_)) {
			fail("the receiver's loading");
			return;
		}
		drx_.start(from);
		replay(drx_, from);
		rx_on_ = true;
	}
	/* until the peer's data decodes, a further MP of the peer's */
	mrx_.restart(from);
	replay(mrx_, from);
	mrx_on_ = true;
}

/* phase 4 on the start-up's end: its loadings and round trip, both
   directions started, the watch and the parametric mode's retrain
   timer set */
void VcvmModem::enter_data()
{
	rxl_ = su_->rx_loading();
	txl_ = su_->tx_loading();
	rtt_ms_ = su_->rtt_ms();
	/* the start-up's samples counted from its own start */
	last_rx_end_ = su_at_ + su_->last_rx_end();
	state_ = DATA;
	note("phase 4: transmitting %s, receiving %s", txl_.str().c_str(),
	     rxl_.str().c_str());
	start_tx();
	start_rx(last_rx_end_);
	p4_mps_ = 0;
	data_at_ = t_;
	next_check_ = t_ + 3L * c_.su.fs;
	words_ = failed_ = 0;
	cbar_low_ = false;
	next_retrain_ = c_.retrain_timers && rxl_.kind != VcvmLoading::COMB ?
		t_ + 30L * c_.su.fs : -1;
}

/* an MP (this DCE's loading, the round trip, ack and retrain) and
   100 ms of silence to the queue */
void VcvmModem::send_mp()
{
	VcvmMp mp;
	mp.ack = got_mp_;
	mp.retrain = retrain_ || peer_retrain_;
	mp.rtt = rtt_ms_ < 0 ? 255 : std::min(rtt_ms_ / 10, 254);
	mp.l = rxl_;
	u8 o[48];
	mtx_.send(o, mp.encode(o), txq_);
	silence(100);
	sent_ack_ = mp.ack && got_ack_;
	mps_++;
}

/* the loading phase 4's measurements allow: each loaded tone's order no
   higher than its phase error now allows, no rings where it grew, the
   code rate a step down where codewords failed; *retrain where fewer
   than four tones are left (or in the parametric mode: nothing in
   phase 4 measures the combs) */
VcvmLoading VcvmModem::rechoose(bool *retrain)
{
	VcvmLoading l = rxl_;
	*retrain = false;
	if (l.kind != VcvmLoading::COMB) {
		*retrain = true;
		return l;
	}
	const VcvmDataStats &st = drx_.stats();
	int allowed = vcvm_allowed(c_.su.own, c_.su.peer, c_.su.modes);
	for (int k = 0; k < l.tones; k++) {
		int pb = l.load.phase[k];
		if (!pb)
			continue;
		double s = st.sigma[k];
		int rule = s <= 9 && (allowed & VCVM_M_8DPSK) ? 3 : s <= 20 ? 2 : 0;
		int npb = std::min(pb, rule);
		if (npb != pb || s > (pb == 3 ? 7 : 15))
			l.load.ring[k] = 0;
		l.load.phase[k] = static_cast<u8>(npb);
		if (!npb)
			l.load.ring[k] = 0;
	}
	if (st.words && st.failed * 100 > st.words && l.rate > 0)
		l.rate--;
	if (l.loaded() < 4)
		*retrain = true;
	return l;
}

/* a renegotiation (or a retrain): asked for here (the new loading
   rechosen) or on the peer's control sequence (remote); the control
   sequence sent, the messages looked for, 25 s to finish */
void VcvmModem::begin_reneg(bool remote, bool retrain)
{
	want_reneg_ = want_retrain_ = false;
	state_ = RENEG;
	retrain_ = retrain;
	peer_retrain_ = false;
	got_mp_ = got_ack_ = sent_ack_ = false;
	mps_ = 0;
	if (!remote && !retrain) {
		bool rt;
		VcvmLoading l = rechoose(&rt);
		if (rt)
			retrain_ = true;
		else
			rxl_ = l;
	}
	note("%s %s%s", remote ? "the peer's control sequence:" : "asking for",
	     retrain_ ? "a retrain" : "a renegotiation",
	     retrain_ || remote ? "" : (", receiving " + rxl_.str()).c_str());
	/* the control sequence after the symbol going out, then MP */
	if (tx_data_)
		dtx_.control(txq_);
	tx_data_ = false;
	s32 from = remote ? drx_.position() : t_;
	rx_on_ = false;
	mrx_.restart(from);
	replay(mrx_, from);
	mrx_on_ = true;
	deadline_ = t_ + 25L * c_.su.fs;
}

/* the MPs acknowledged both ways: phase 4 again with the new loadings,
   or after a retrain the start-up again */
void VcvmModem::finish_reneg()
{
	if (retrain_ || peer_retrain_) {
		retrains_++;
		start_startup(true);
		return;
	}
	renegs_++;
	state_ = DATA;
	note("renegotiated: transmitting %s, receiving %s", txl_.str().c_str(),
	     rxl_.str().c_str());
	start_tx();
	start_rx(last_rx_end_);
	p4_mps_ = 0;
	data_at_ = t_;
	next_check_ = t_ + 3L * c_.su.fs;
	words_ = failed_ = 0;
}

/* a message received: in phase 4 the peer's last MP, a repeated one
   (answered), or a renegotiation's; in a renegotiation the peer's MP
   taken */
void VcvmModem::on_msg(const VcvmMsg &m)
{
	VcvmMp mp;
	if (!m.ok ||
	    mp.decode(m.octets.data(), static_cast<int>(m.octets.size())))
		return;
	last_rx_end_ = m.end;
	if (state_ == DATA) {
		if (mp.ack && !mp.retrain && p4_mps_++ == 0) {
			/* the peer's one more MP after our acknowledgement
			   (9.4), which ends after we have begun phase 4: its
			   phase 4 starts at that MP's end */
			note("the peer's last MP in phase 4");
			if (rx_on_) {
				drx_.start(m.end);
				replay(drx_, m.end);
				/* phase 4's watch from there */
				next_check_ = t_ + 3L * c_.su.fs;
				words_ = failed_ = 0;
			}
			return;
		}
		if (mp.ack && !mp.retrain) {
			/* MPs go on: the peer lost our last MP; another, and
			   phase 4 again */
			note("an MP of the peer's in phase 4: ours again");
			p4_mps_ = 0;
			txq_.resize(std::min(txq_.size(), txpos_));
			tx_data_ = false;
			got_mp_ = got_ack_ = true;
			send_mp();
			start_tx();
			if (rx_on_) {
				drx_.start(m.end);
				replay(drx_, m.end);
			}
			return;
		}
		/* a renegotiation or a retrain the control sequence of
		   which we did not see */
		begin_reneg(true, false);
		mrx_.restart(m.end);
		replay(mrx_, m.end);
	}
	if (state_ != RENEG)
		return;
	txl_ = mp.l;
	got_mp_ = true;
	got_ack_ |= mp.ack;
	peer_retrain_ |= mp.retrain;
	note("MP%s%s: the peer asks for %s", mp.ack ? " (ack)" : "",
	     mp.retrain ? " (retrain)" : "", mp.l.str().c_str());
}

/* phase 4's watch (9.5): a retrain on the signal lost, the parametric
   mode's c-bar and timers; a renegotiation, every 2 s, on codewords
   failing or a tone's phase error over its order's threshold */
void VcvmModem::check_data()
{
	const s32 fs = c_.su.fs;
	if (!rx_on_ || !drx_.confirmed()) {
		/* no data mode: the periodic retrain alone */
		if (next_retrain_ >= 0 && t_ >= next_retrain_ &&
		    rxl_.kind == VcvmLoading::NONE)
			begin_reneg(false, true);
		return;
	}
	if (drx_.lost()) {
		note("no codeword for 2 s");
		begin_reneg(false, true);
		return;
	}
	const VcvmDataStats &st = drx_.stats();
	if (next_retrain_ >= 0 && t_ - confirmed_at_ > 2L * fs) {
		/* 9.5: none while c-bar is under 0.85, at once when it
		   rises to it, else every 60 s; with 0.02 of hysteresis
		   either way (c-bar wanders by that much over a codec
		   mode that sits at 0.85), and not in phase 4's first
		   30 s */
		if (st.cbar < 0.83f)
			cbar_low_ = true;
		else if (cbar_low_ && st.cbar >= 0.87f &&
			 t_ - data_at_ > 30L * fs) {
			note("c-bar %.3f: a better codec mode", st.cbar);
			begin_reneg(false, true);
			return;
		}
		if (t_ >= next_retrain_) {
			next_retrain_ = t_ + 60L * fs;
			if (!cbar_low_) {
				begin_reneg(false, true);
				return;
			}
		}
	}
	if (!c_.auto_reneg || t_ < next_check_)
		return;
	next_check_ = t_ + 2L * fs;
	bool bad = st.failed - failed_ >= 2;
	words_ = st.words;
	failed_ = st.failed;
	for (int k = 0; k < rxl_.tones && rxl_.kind == VcvmLoading::COMB; k++) {
		int pb = rxl_.load.phase[k];
		if (pb && st.sigma[k] > (pb == 2 ? 24 : 12))
			bad = true;
	}
	if (bad)
		begin_reneg(false, false);
}

/* what a block of samples leads to: the messages taken, the
   renegotiations begun, the MPs sent until both are acknowledged */
void VcvmModem::step()
{
	VcvmMsg m;
	switch (state_) {
	case STARTUP:
	case FAILED:
		return;
	case DATA:
		while (mrx_on_ && state_ == DATA && mrx_.get(&m))
			on_msg(m);
		if (state_ != DATA)
			break;
		if (rx_on_ && mrx_on_ && drx_.confirmed()) {
			mrx_on_ = false;
			confirmed_at_ = t_;
			note("the peer's data decoding");
		}
		if (rx_on_ && drx_.control()) {
			begin_reneg(true, false);
			break;
		}
		if (want_retrain_ || want_reneg_) {
			begin_reneg(false, want_retrain_);
			break;
		}
		check_data();
		break;
	case RENEG:
		break;
	}
	if (state_ != RENEG)
		return;
	while (mrx_.get(&m))
		on_msg(m);
	if (txpos_ >= txq_.size() && !mrx_.busy()) {
		if (got_ack_ && sent_ack_) {
			finish_reneg();
			return;
		}
		if (mps_ >= 10) {
			fail("no acknowledgement of MP");
			return;
		}
		send_mp();
	}
	if (t_ > deadline_)
		fail("the renegotiation");
}

/* n samples from the line in, n out, 10 ms at a time: the start-up's,
   then phase 4's and the renegotiations' */
void VcvmModem::process(const float *in, float *out, int n)
{
	const s32 fs = c_.su.fs;
	const int B = fs / 100;
	while (n > 0) {
		int k = std::min(n, B);
		hist_.insert(hist_.end(), in, in + k);
		if (static_cast<s32>(hist_.size()) > 4L * fs) {
			s32 cut = static_cast<s32>(hist_.size()) -
				      3L * fs;
			hist_.erase(hist_.begin(), hist_.begin() + cut);
			hbase_ += cut;
		}
		if (state_ == STARTUP) {
			su_->process(in, out, k);
			t_ += k;
			if (su_->state() == VcvmStartup::DONE)
				enter_data();
			else if (su_->state() == VcvmStartup::FAILED)
				fail(su_->why());
		} else {
			if (state_ != FAILED) {
				if (rx_on_ && state_ == DATA)
					drx_.push(in, k);
				if (mrx_on_)
					mrx_.push(in, k);
			}
			/* a symbol ahead of what goes out */
			while (tx_data_ && queued() < t_ + k)
				dtx_.symbol(txq_);
			for (int i = 0; i < k; i++)
				out[i] = txpos_ < txq_.size() ? txq_[txpos_++] : 0.0f;
			if (txpos_ >= txq_.size()) {
				txq_.clear();
				txpos_ = 0;
			}
			t_ += k;
			step();
		}
		in += k;
		out += k;
		n -= k;
	}
}

} // namespace dsp
