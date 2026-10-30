/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    lapb.cpp - X.75 SLP (LAPB, basic mode) and V.120 (LAPD's frames,
 *               extended mode): link set-up with SABM or SABME, the
 *               data phase with REJ and T1 recovery, release with DISC
 *
 *    Control frames (S and U) wait in a ring of their own and go out
 *    first.  I frames are filled with the DTE's characters only when one
 *    may go out, and stay in their ring until the remote acknowledges
 *    them.  T1 is the Link's bit timer, which counts transmitted bits.
 *
 */

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "dm_types.h"
#include "link.h"

namespace dlink {

/* the addresses (X.75 2.4.2, isdn4linux): B (0x01) the calling end's
   commands and the called end's responses, A (0x03) the others */
static const u8 ADDR_A = 0x03, ADDR_B = 0x01;

/* V.120's logical link (V.120 2.3.2): its address octets, C/R 0 and 1
   (Q.921 3.3.3: the user side's commands and the network side's
   responses C/R 0) */
static const u8 V120_LLI_0[2] = { 0x08, 0x01 }, V120_LLI_1[2] = { 0x0a, 0x01 };
/* V.120's terminal adaption header (V.120 2.4.1): E 1 (no extension),
   BR 0, C2 C1 0, B and F 1 */
static const u8 V120_HEADER = 0x83;

/* control octets (X.75 2.3.4, Q.921 3.4): S frames' function (N(R) and
   P/F masked off), U frames with P/F masked off; P/F in a U frame (and
   in basic mode's I and S frames), and in extended mode's second
   octet */
static const u8 S_RR = 0x01, S_RNR = 0x05, S_REJ = 0x09;
static const u8 U_SABM = 0x2f, U_SABME = 0x6f, U_DM = 0x0f, U_DISC = 0x43,
	U_UA = 0x63, U_FRMR = 0x87;
static const u8 PF = 0x10, PF_EXT = 0x01;

/* T1 when the data pump's rate is not known: a 64 kbit/s B-channel */
static const s32 T1_NO_RATE = 64000;

/* a call with v's frames: disconnected, the addresses of our end, the
   rings empty */
void Lapb::init(Link *link, Variant v)
{
	link_ = link;
	variant_ = v;
	const bool caller = link->caller();
	if (v == Variant::V120) {
		tag_ = "v120";
		modulus_mask_ = 127;
		alen_ = 2;
		info_at_ = 2 + 2 + 1;
		sabm_ = U_SABME;
		memcpy(cmd_addr_, caller ? V120_LLI_0 : V120_LLI_1, 2);
		memcpy(rsp_addr_, caller ? V120_LLI_1 : V120_LLI_0, 2);
	} else {
		tag_ = "x75";
		modulus_mask_ = 7;
		alen_ = 1;
		info_at_ = 2;
		sabm_ = U_SABM;
		cmd_addr_[0] = caller ? ADDR_B : ADDR_A;
		rsp_addr_[0] = caller ? ADDR_A : ADDR_B;
	}
	for (s16 i = 0; i < INFO_FRAMES; i++) {
		info_[i].frame = Frame();
		info_[i].frame.buf = info_[i].buf;
	}
	for (s16 i = 0; i < CTRL_FRAMES; i++) {
		ctrl_frame_[i] = Frame();
		ctrl_frame_[i].buf = ctrl_buf_[i];
	}
	ctrl_tx_ = ctrl_free_ = 0;
	reset();
	state_ = IDLE;
	heard_ = false;
	tries_ = 0;
	rx_count_ = rx_head_ = 0;
}

/* the link is up at the bit level: our frames to Hdlc; the calling end
   sets the link up at once, the called one waits for its SABM (and
   sends its own if none comes in 4 T1s) */
void Lapb::start()
{
	link_->hdlc().set_framer(this, true);
	link_->push_pending = false;
	if (link_->caller()) {
		establish();
		return;
	}
	tries_ = 0;
	link_->set_timer(4 * t1_bits(), Link::Timer::T1);
}

/* we hang up: DISC, T1 for its answer */
void Lapb::disconnect()
{
	state_ = DISC;
	ctrl(cmd_addr_, U_DISC | PF);
	timer_start();
}

/* the received characters the DTE did not take, offered again: once it
   has them all our receiver is no longer busy, which an RR says */
void Lapb::push_rest()
{
	if (state_ != DATA || !rx_count_)
		return;
	s16 taken = link_->write_chars(rx_buf_ + rx_head_, rx_count_);
	if (taken > 0) {
		rx_head_ += taken;
		rx_count_ -= taken;
	}
	if (rx_count_)
		return;
	busy_ = false;
	super(cmd_addr_, S_RR, false);
	link_->push_pending = false;
}


/*
 *    state changes and T1
 *
 */

/* the sequence reset: V(S), V(A), V(R) 0, neither side busy, the I
   frames held dropped */
void Lapb::reset()
{
	vs_ = va_ = vr_ = 0;
	reject_ = busy_ = peer_busy_ = false;
	sent_ = next_ = free_ = 0;
}

/* the data phase begins (again) */
void Lapb::connect()
{
	reset();
	state_ = DATA;
	timer_stop();
	link_->logf("%s: connected.", tag_);
	link_->status(Status::Up);
}

/* the link set up by us: SABM, T1 for the UA */
void Lapb::establish()
{
	reset();
	state_ = ESTAB;
	ctrl(cmd_addr_, static_cast<u8>(sabm_ | PF));
	timer_start();
}

/* the link is down: disconnected, Released */
void Lapb::release(const char *why)
{
	reset();
	state_ = IDLE;
	timer_stop();
	link_->logf("%s: released (%s).", tag_, why);
	link_->status(Status::Released);
}

/* T1 in bits: 1 s at the pump's rate, and the time of our longest frame
   and two of the remote's longest ahead of the answer */
s32 Lapb::t1_bits() const
{
	s32 rate = link_->tx_rate();
	return (rate ? rate : T1_NO_RATE) +
		(LAPB_TX_INFO + 2 * LAPB_RX_INFO + 24) * 8;
}

/* T1 starts again, the tries from 0 */
void Lapb::timer_start()
{
	tries_ = 0;
	link_->set_timer(t1_bits(), Link::Timer::T1);
}

/* T1 stops */
void Lapb::timer_stop()
{
	tries_ = 0;
	link_->set_timer(0, Link::Timer::None);
}

/* T1 has run out: the called end that has heard nothing sets the link
   up itself; below N2 tries the frame awaiting an answer again (SABM,
   DISC, or a poll in the data phase), T1 again; at N2 the link is
   down (in the data phase after our DISC) */
void Lapb::t1_timeout()
{
	if (state_ == IDLE) {
		tries_ = 0;
		if (!heard_) {
			link_->logf("%s: no SABM from the caller: ours.", tag_);
			establish();
		}
		return;
	}
	if (tries_ < N2) {
		tries_++;
		if (state_ == ESTAB)
			ctrl(cmd_addr_, static_cast<u8>(sabm_ | PF));
		else if (state_ == DISC)
			ctrl(cmd_addr_, U_DISC | PF);
		else
			super(cmd_addr_, busy_ ? S_RNR : S_RR, true);
		link_->set_timer(t1_bits(), Link::Timer::T1);
		return;
	}
	switch (state_) {
	case ESTAB:
		release("no answer to SABM");
		break;
	case DISC:
		release("no answer to DISC");
		break;
	case DATA:
		link_->logf("%s: no answer to %d polls.", tag_, N2);
		disconnect();
		tries_ = N2 - 1;
		break;
	case IDLE:
		break;
	}
}


/*
 *    frames to send
 *
 */

/* a U frame queued: address addr, control octet c; dropped when the
   ring is full */
void Lapb::ctrl(const u8 *addr, u8 c)
{
	s16 nf = static_cast<s16>((ctrl_free_ + 1) % CTRL_FRAMES);
	if (nf == ctrl_tx_) {
		link_->logf("err: %s: control queue full: frame dropped.", tag_);
		return;
	}
	Frame &f = ctrl_frame_[ctrl_free_];
	memcpy(f.buf, addr, alen_);
	f.buf[alen_] = c;
	f.size = static_cast<s16>(alen_ + 1);
	ctrl_free_ = nf;
}

/* an S frame queued: function fn, N(R) = V(R) now, P/F pf (in extended
   mode a second control octet) */
void Lapb::super(const u8 *addr, u8 fn, bool pf)
{
	if (modulus_mask_ == 7) {
		ctrl(addr, static_cast<u8>(vr_ << 5 | (pf ? PF : 0) | fn));
		return;
	}
	ctrl(addr, fn);
	Frame &f = ctrl_frame_[(ctrl_free_ + CTRL_FRAMES - 1) % CTRL_FRAMES];
	if (f.size != alen_ + 1 || f.buf[alen_] != fn)
		return;         /* (dropped: the ring was full) */
	f.buf[alen_ + 1] = static_cast<u8>(vr_ << 1 | (pf ? PF_EXT : 0));
	f.size = static_cast<s16>(alen_ + 2);
}

/* an I frame can go: the remote not busy, fewer than k outstanding, and
   one waiting to go (again, after a go back) or filled now with the
   DTE's characters */
bool Lapb::tx_info()
{
	if (peer_busy_ || seq_diff(vs_, va_) >= LAPB_WIN)
		return false;
	if (next_ != free_)
		return true;
	Frame &f = info_[free_].frame;
	s16 n = link_->read_chars(f.buf + info_at_, LAPB_TX_INFO);
	if (n < 0) {
		link_->logf("err: %s: DTE read error (%d).", tag_, n);
		link_->status(Status::Error);
		return false;
	}
	if (!n)
		return false;
	f.size = static_cast<s16>(info_at_ + n);
	free_ = static_cast<s16>((free_ + 1) % INFO_FRAMES);
	return true;
}

/* the framer's next frame: a control frame waiting, else in the data
   phase an I frame if one can go (N(S) = V(S), which advances; N(R) =
   V(R)), T1 started if it is not running; NULL for flags */
Frame *Lapb::next_tx_frame()
{
	if (ctrl_tx_ != ctrl_free_) {
		Frame *f = &ctrl_frame_[ctrl_tx_];
		ctrl_tx_ = static_cast<s16>((ctrl_tx_ + 1) % CTRL_FRAMES);
		return f;
	}
	if (state_ != DATA || !tx_info())
		return nullptr;
	Frame *f = &info_[next_].frame;
	memcpy(f->buf, cmd_addr_, alen_);
	if (modulus_mask_ == 7) {
		f->buf[alen_] = static_cast<u8>(vr_ << 5 | vs_ << 1);
	} else {
		f->buf[alen_] = static_cast<u8>(vs_ << 1);
		f->buf[alen_ + 1] = static_cast<u8>(vr_ << 1);
	}
	if (variant_ == Variant::V120)
		f->buf[info_at_ - 1] = V120_HEADER;
	vs_ = seq(vs_ + 1);
	next_ = static_cast<s16>((next_ + 1) % INFO_FRAMES);
	if (!link_->timer())
		timer_start();
	return f;
}

/* a frame is out: nothing to do (I frames are freed by their
   acknowledgement) */
void Lapb::tx_complete(Frame *f)
{
	(void)f;
}


/*
 *    received frames
 *
 */

/* an N(R) we can be sent: from V(A) to the last N(S) we sent; else
   the frame is taken for noise (logged, counted as an error) */
bool Lapb::nr_ok(u8 nr)
{
	/* (the I frames sent and not acknowledged, our ring's count) */
	s16 sent = static_cast<s16>((next_ - sent_ + INFO_FRAMES) % INFO_FRAMES);
	if (seq_diff(nr, va_) <= std::max(seq_diff(vs_, va_), sent))
		return true;
	link_->logf("err: %s: N(R) %d past V(S) %d (V(A) %d): ignored.", tag_, nr,
		    vs_, va_);
	link_->stats().rx_errors++;
	return false;
}

/* our I frames up to N(R) - 1 acknowledged and freed (those waiting to
   go again after a go back too); out of timer recovery T1 stops when
   some were, and runs on while others are outstanding */
void Lapb::ack(u8 nr)
{
	const s16 n = seq_diff(nr, va_);
	const s16 out = seq_diff(vs_, va_);
	sent_ = static_cast<s16>((sent_ + n) % INFO_FRAMES);
	va_ = nr;
	if (n > out) {
		vs_ = va_;
		next_ = sent_;
	}
	if (n && !tries_ && state_ == DATA) {
		timer_stop();
		if (vs_ != va_)
			timer_start();
	}
}

/* go back: the outstanding I frames go again from the oldest */
void Lapb::go_back()
{
	s16 n = seq_diff(vs_, va_);
	vs_ = va_;
	next_ = sent_;
	link_->stats().tx_again += n;
}

/* an I frame in the data phase: its N(R) first; dropped while we are
   busy or out of sequence (one REJ until the frame comes); else its
   information to the DTE (what it does not take kept, our receiver
   busy meanwhile), V(R) on, and an RR unless an I frame of ours
   acknowledges it */
void Lapb::rx_info(Frame *f, bool pf)
{
	u8 fn, ns, nr;
	bool p;
	if (!parse(f, fn, ns, nr, p))
		return;
	(void)fn;
	(void)p;
	if (!nr_ok(nr))
		return;
	ack(nr);
	if (busy_) {
		if (pf)
			super(rsp_addr_, S_RNR, true);
		return;
	}
	if (ns != vr_) {
		if (!reject_) {
			reject_ = true;
			super(rsp_addr_, S_REJ, pf);
		} else if (pf)
			super(rsp_addr_, S_RR, true);
		return;
	}
	reject_ = false;
	/* the information: after the control field, and in V.120 the
	   terminal adaption header (its octets up to the one with E 1) */
	s16 at = static_cast<s16>(alen_ + (modulus_mask_ == 7 ? 1 : 2));
	if (variant_ == Variant::V120) {
		while (at < f->count && !(f->buf[at] & 0x80))
			at++;
		at++;
	}
	const s16 n = static_cast<s16>(at < f->count ? f->count - at : 0);
	const u8 *info = f->buf + at;
	s16 taken = n ? link_->write_chars(info, n) : 0;
	if (taken < 0)
		taken = 0;
	if (taken < n) {
		rx_head_ = 0;
		rx_count_ = static_cast<s16>(n - taken);
		memcpy(rx_buf_, info + taken, rx_count_);
		busy_ = true;
		link_->push_pending = true;
	}
	vr_ = seq(vr_ + 1);
	if (busy_)
		super(rsp_addr_, S_RNR, pf);
	else if (pf || peer_busy_ || !tx_info())
		super(rsp_addr_, S_RR, pf);
}

/* an S frame in the data phase: RR and REJ clear the remote's busy
   condition, RNR sets it, all acknowledge; a REJ (out of timer
   recovery) or a final response in timer recovery sends the
   outstanding frames again; a poll is answered with our receiver's
   state */
void Lapb::rx_super(Frame *f, bool cmd)
{
	u8 fn, ns, nr;
	bool pf;
	if (!parse(f, fn, ns, nr, pf))
		return;
	const u8 c = f->buf[alen_];
	if (!nr_ok(nr))
		return;
	switch (fn) {
	case S_RR:
		peer_busy_ = false;
		break;
	case S_RNR:
		peer_busy_ = true;
		break;
	case S_REJ:
		peer_busy_ = false;
		break;
	default:
		link_->logf("err: %s: unknown S frame 0x%02x.", tag_, c);
		return;
	}
	ack(nr);
	if (!cmd && pf && tries_) {
		/* the end of timer recovery */
		go_back();
		timer_stop();
		if (peer_busy_)
			timer_start();
	} else if (fn == S_REJ && !tries_) {
		go_back();
		timer_stop();
	} else if (fn == S_RNR && !link_->timer()) {
		/* (the polls go on while the remote is busy) */
		timer_start();
	}
	if (cmd && pf)
		super(rsp_addr_, busy_ ? S_RNR : S_RR, true);
}

/* a U frame: our mode's SABM (X.75's) or SABME (V.120's) sets the link
   up (again) and is always taken; DISC releases it (a DM while
   disconnected); UA ends our set-up or release; DM ends the link; FRMR
   sets it up again; the other mode's set-up is refused with a DM */
void Lapb::rx_unnum(Frame *f, bool cmd)
{
	const u8 c = f->buf[alen_];
	const u8 pf = c & PF;
	const u8 t = static_cast<u8>(c & ~PF);
	if (cmd) {
		switch (t) {
		case U_SABM:
		case U_SABME:
			if (t != sabm_) {
				ctrl(rsp_addr_, static_cast<u8>(U_DM | pf));
				break;
			}
			ctrl(rsp_addr_, static_cast<u8>(U_UA | pf));
			connect();
			break;
		case U_DISC:
			if (state_ == IDLE) {
				ctrl(rsp_addr_, static_cast<u8>(U_DM | pf));
				break;
			}
			ctrl(rsp_addr_, static_cast<u8>(U_UA | pf));
			release("by the remote");
			break;
		default:
			link_->logf("%s: U command 0x%02x ignored.", tag_, c);
			break;
		}
		return;
	}
	switch (t) {
	case U_UA:
		if (state_ == ESTAB)
			connect();
		else if (state_ == DISC)
			release("by us");
		break;
	case U_DM:
		if (state_ == ESTAB && !pf)
			break;  /* (a DM asking for a SABM: ours is out) */
		if (state_ != IDLE)
			release("DM");
		break;
	case U_FRMR:
		link_->logf("err: %s: FRMR: set up again.", tag_);
		if (state_ == DATA)
			establish();
		break;
	default:
		link_->logf("%s: U response 0x%02x ignored.", tag_, c);
		break;
	}
}

/* f's control field: an S frame's function fn, an I frame's N(S) ns,
   and N(R) and P/F (basic mode's one octet, extended mode's two);
   false (logged) for a frame too short for it */
bool Lapb::parse(const Frame *f, u8 &fn, u8 &ns, u8 &nr, bool &pf) const
{
	const u8 c = f->buf[alen_];
	if (modulus_mask_ == 7) {
		fn = c & 0x0f;
		ns = c >> 1 & 7;
		nr = c >> 5;
		pf = c & PF;
		return true;
	}
	if (f->count < alen_ + 2) {
		link_->logf("err: %s: frame of %d octets: no N(R).", tag_,
			    f->count);
		return false;
	}
	const u8 c2 = f->buf[alen_ + 1];
	fn = c;
	ns = c >> 1;
	nr = c2 >> 1;
	pf = c2 & PF_EXT;
	return true;
}

/* a good frame from Hdlc: one at neither of our addresses, too short,
   or with too long an information field, dropped; I and S frames
   counted only in the data phase (a poll while disconnected gets a
   DM), U frames in every state.  The remote's commands come at our
   response address */
void Lapb::rx_complete(Frame *f)
{
	const bool at_rsp = f->count > alen_ &&
		!memcmp(f->buf, rsp_addr_, alen_);
	const bool at_cmd = f->count > alen_ &&
		!memcmp(f->buf, cmd_addr_, alen_);
	if (!at_rsp && !at_cmd) {
		link_->logf("err: %s: frame of %d octets at 0x%02x dropped.",
			    tag_, f->count, f->count ? f->buf[0] : 0);
		return;
	}
	heard_ = true;
	const bool cmd = at_rsp;
	const u8 c = f->buf[alen_];
	if ((c & 3) == 3) {
		rx_unnum(f, cmd);
		return;
	}
	const s16 head = static_cast<s16>(alen_ + (modulus_mask_ == 7 ? 1 : 2) +
					  (variant_ == Variant::V120 ? 1 : 0));
	if (f->count - head > LAPB_RX_INFO) {
		link_->logf("err: %s: I frame of %d octets dropped.", tag_,
			    f->count - head);
		return;
	}
	u8 fn, ns, nr;
	bool pf;
	if (!parse(f, fn, ns, nr, pf))
		return;
	if (state_ != DATA) {
		if (state_ == IDLE && cmd && pf)
			ctrl(rsp_addr_, U_DM | PF);
		return;
	}
	if (!(c & 1)) {
		if (cmd)
			rx_info(f, pf);
		return;
	}
	if (f->count != alen_ + (modulus_mask_ == 7 ? 1 : 2)) {
		link_->logf("err: %s: S frame of %d octets.", tag_, f->count);
		return;
	}
	rx_super(f, cmd);
}

} // namespace dlink
