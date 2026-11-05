/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hst_link.cpp - USRobotics HST's own link layer (hst_link.h): data
 *                   frames numbered modulo 256, cumulative RRs, REJ and
 *                   T1 recovery (go back N)
 *
 *    Control frames wait in a ring of their own and go out first; an
 *    RR is built when it goes, so that one carries the latest N(R).
 *    Data frames are filled with the DTE's characters only when one may
 *    go out, and stay in their ring until the remote acknowledges them.
 *    The Link's bit timer is T1 while frames are outstanding, else the
 *    idle RR's.
 *
 */

#include <cstring>

#include "dm_types.h"
#include "link.h"

namespace dlink {

/* the frames' types (the first octet) */
static const u8 T_REJ = 0x03, T_DATA = 0x04, T_RR = 0x05, T_ASK = 0x06,
	T_GRANT = 0x07, T_DATA_ACK = 0x08;
/* the octet after N(R) in an RR, a REJ and a data frame with an
   acknowledgement (a Courier's is always 8: perhaps its window) */
static const u8 ACK_TAIL = 0x08;
/* an ask's code: turn the channels around (others are ASL's rates,
   0x30 + the rate's index) */
static const u8 ASK_TURN = 0x04;

/* the back channel's rate */
static const s32 BACK_RATE = 300;

/* a call over link: nothing heard, sent or held */
void HstLink::init(Link *link)
{
	link_ = link;
	for (s16 i = 0; i < INFO_FRAMES; i++) {
		info_[i].frame = Frame();
		info_[i].frame.buf = info_[i].buf;
	}
	for (s16 i = 0; i < CTRL_FRAMES; i++) {
		ctrl_frame_[i] = Frame();
		ctrl_frame_[i].buf = ctrl_buf_[i];
	}
	state_ = IDLE;
	vs_ = va_ = 1;
	vr_ = 0;
	ack_due_ = reject_ = asks_logged_ = false;
	fwd_ = !link->caller();
	asked_ = turn_wait_ = false;
	granted_ = heard_ = false;
	quiet_ = 0;
	ask_k_ = 0;
	grant_k_ = -1;
	grant_frame_ = nullptr;
	fwd_frames_ = 0;
	tries_ = 0;
	sent_ = next_ = free_ = 0;
	ctrl_tx_ = ctrl_free_ = 0;
	backlog_.clear();
}

/* HST's pump has connected: our frames to Hdlc, our first RR (of
   nothing), and T1 for the remote's first frame */
void HstLink::start()
{
	link_->hdlc().set_framer(this, true);
	link_->push_pending = false;
	link_->logf("hst: the link, %s.",
		    link_->caller() ? "calling end" : "called end");
	state_ = WAIT;
	ack_due_ = true;
	tries_ = 0;
	link_->set_timer(t1_bits(), Link::Timer::Hst);
}

/* we hang up: a Courier sends no frame for it (its carrier stops), so
   neither do we; Released after a tenth of a second */
void HstLink::disconnect()
{
	state_ = DISC;
	link_->set_timer(rate() / 10, Link::Timer::Hst);
}

/* the received characters the DTE did not take, offered again: once it
   has them all, the frames that follow are taken again */
void HstLink::push_rest()
{
	if (state_ == DATA && !backlog_.empty())
		backlog_.push(link_);
}


/*
 *    timers and state
 *
 */

/* our side's rate (bit/s), for the timers: the back channel's, or the
   forward channel's that XID agreed (the pump's rate, which changes
   only once it has trained after a turn, else) */
s32 HstLink::rate() const
{
	if (!fwd_)
		return BACK_RATE;
	s32 r = link_->hst_rate();
	if (!r)
		r = link_->tx_rate();
	return r > 0 ? r : BACK_RATE;
}

/* T1 in bits: 4 s (two of the remote's longest frames on the back
   channel, and its delay), and our longest frame's time */
s32 HstLink::t1_bits() const
{
	return 4 * rate() + (4 + HST_INFO + 4) * 8;
}

/* the idle RR's interval (a Courier's is about 1.5 s) */
s32 HstLink::idle_bits() const
{
	return rate() * 3 / 2;
}

/* the timer for what is outstanding: T1 while data frames are, else
   the idle RR's */
void HstLink::timer_restart()
{
	link_->set_timer(sent_ != next_ ? t1_bits() : idle_bits(),
			 Link::Timer::Hst);
}

/* the timer has run out: waiting, our RR again (released after N2);
   in the data phase, with frames outstanding, them again from the
   oldest (released after N2 without an answer), else the idle RR;
   after our hangup, Released */
void HstLink::timeout()
{
	switch (state_) {
	case IDLE:
		return;
	case DISC:
		release("by us");
		return;
	case WAIT:
		if (++tries_ >= N2) {
			release("nothing from the remote");
			return;
		}
		ack_due_ = true;
		link_->set_timer(t1_bits(), Link::Timer::Hst);
		return;
	case DATA:
		break;
	}
	if (turn_wait_) {
		turn();
		return;
	}
	/* nothing heard since our grant turned the channels: the remote
	   did not get it, and still sends the back channel; ours the
	   forward channel again (the remote asks again) */
	if (granted_ && !heard_ && ++quiet_ >= 3) {
		link_->logf("hst: nothing heard since our grant: the "
			    "channels back.");
		turn();
		return;
	}
	/* (our ask again, as a Courier's) */
	if (asked_ && !fwd_)
		ctrl(T_ASK, ask_k_, ASK_TURN, 2);
	if (sent_ == next_) {
		ack_due_ = true;
		timer_restart();
		return;
	}
	if (++tries_ > N2) {
		release("no answer");
		return;
	}
	link_->stats().timeouts++;
	go_back();
	ack_due_ = true;
	link_->set_timer(t1_bits(), Link::Timer::Hst);
}

/* the remote's first frame: data can flow */
void HstLink::up()
{
	state_ = DATA;
	tries_ = 0;
	link_->logf("hst: connected.");
	timer_restart();
	link_->status(Status::Up);
}

/* the forward channel's side, asked to turn the channels around:
   time to grant it, the DTE having nothing more for us or our time
   with the channel up (no more new frames then) */
bool HstLink::granting() const
{
	return fwd_ && grant_k_ >= 0 &&
		(link_->tx_buffered() <= 0 || fwd_frames_ >= 32);
}

/* and the grant can go: our frames all out (none waiting to go, nor
   a control frame) */
bool HstLink::grant_ready() const
{
	return granting() && !grant_frame_ && next_ == free_ &&
		ctrl_tx_ == ctrl_free_;
}

/* the channels turn around: the pump told, our side of them the other,
   an RR first on the new one */
void HstLink::turn()
{
	granted_ = fwd_ && grant_frame_;
	heard_ = false;
	quiet_ = 0;
	fwd_ = !fwd_;
	asked_ = turn_wait_ = false;
	grant_k_ = -1;
	grant_frame_ = nullptr;
	fwd_frames_ = 0;
	ack_due_ = true;
	reject_ = false;
	link_->logf("hst: the channels turn around: we send the %s channel.",
		    fwd_ ? "forward" : "back");
	link_->hst_turn();
	tries_ = 0;
	timer_restart();
}

/* the link is down: Released */
void HstLink::release(const char *why)
{
	state_ = IDLE;
	link_->set_timer(0, Link::Timer::None);
	link_->logf("hst: released (%s).", why);
	link_->status(Status::Released);
}


/*
 *    frames to send
 *
 */

/* a control frame queued: type, then n (1 or 2) of a and b; dropped
   when the ring is full */
void HstLink::ctrl(u8 type, u8 a, u8 b, s16 n)
{
	s16 nf = static_cast<s16>((ctrl_free_ + 1) % CTRL_FRAMES);
	if (nf == ctrl_tx_) {
		link_->logf("err: hst: control queue full: frame dropped.");
		return;
	}
	Frame &f = ctrl_frame_[ctrl_free_];
	f.buf[0] = type;
	f.buf[1] = a;
	f.buf[2] = b;
	f.size = static_cast<s16>(1 + n);
	ctrl_free_ = nf;
}

/* a data frame can go: one waiting to go (again, after a go back), or
   fewer than HST_WIN outstanding and one filled now with the DTE's
   characters (N(S) V(S), which advances) */
bool HstLink::tx_info()
{
	if (next_ != free_)
		return true;
	if (diff(vs_, va_) >= HST_WIN)
		return false;
	Frame &f = info_[free_].frame;
	s16 n = link_->read_chars(f.buf + 2, HST_INFO);
	if (n < 0) {
		link_->logf("err: hst: DTE read error (%d).", n);
		link_->status(Status::Error);
		return false;
	}
	if (!n)
		return false;
	f.buf[0] = T_DATA;
	f.buf[1] = vs_;
	f.size = static_cast<s16>(2 + n);
	vs_ = static_cast<u8>(vs_ + 1);
	free_ = static_cast<s16>((free_ + 1) % INFO_FRAMES);
	return true;
}

/* the framer's next frame: a control frame waiting, else an RR due
   (with V(R) now), else in the data phase a data frame if one can go,
   T1 started for it; NULL for flags */
Frame *HstLink::next_tx_frame()
{
	if (ack_due_ && state_ != IDLE && state_ != DISC) {
		ack_due_ = false;
		ctrl(T_RR, vr_, ACK_TAIL, 2);
	}
	if (state_ == DATA && !fwd_ && !asked_ &&
	    (next_ != free_ || link_->tx_buffered() > 0)) {
		asked_ = true;
		ask_k_ = static_cast<u8>(ask_k_ + 1);
		ctrl(T_ASK, ask_k_, ASK_TURN, 2);
	}
	if (state_ == DATA && grant_ready()) {
		grant_frame_ = &ctrl_frame_[ctrl_free_];
		ctrl(T_GRANT, static_cast<u8>(grant_k_), 0, 1);
	}
	if (ctrl_tx_ != ctrl_free_) {
		Frame *f = &ctrl_frame_[ctrl_tx_];
		ctrl_tx_ = static_cast<s16>((ctrl_tx_ + 1) % CTRL_FRAMES);
		return f;
	}
	if (state_ != DATA || turn_wait_ || (granting() && next_ == free_) ||
	    !tx_info())
		return nullptr;
	if (fwd_)
		fwd_frames_++;
	else if (asked_)
		/* (our ask again before each data frame while it is
		   not granted, as a Courier asks again: the first may
		   have come before the remote's receiver was up) */
		ctrl(T_ASK, ask_k_, ASK_TURN, 2);
	const bool first = sent_ == next_;
	Frame *f = &info_[next_].frame;
	next_ = static_cast<s16>((next_ + 1) % INFO_FRAMES);
	if (first) {
		tries_ = 0;
		link_->set_timer(t1_bits(), Link::Timer::Hst);
	}
	return f;
}

/* a frame is out: our grant, and the channels turn once the flags
   after it are out too (50 ms of them: the remote's receiver decides
   with a delay); data frames are freed by their acknowledgement */
void HstLink::tx_complete(Frame *f)
{
	if (f == grant_frame_ && !turn_wait_) {
		turn_wait_ = true;
		link_->set_timer(rate() / 20 + 8,
				 Link::Timer::Hst);
	}
}


/*
 *    received frames
 *
 */

/* the remote has our frames up to nr: those freed (and those waiting
   to go again with them), T1 again for the rest; an nr past what we
   sent is taken for noise */
void HstLink::ack(u8 nr)
{
	const s16 n = diff(static_cast<u8>(nr + 1), va_);
	if (!n)
		return;
	if (n > diff(vs_, va_)) {
		link_->logf("err: hst: N(R) %d past V(S) %d (V(A) %d): ignored.",
			    nr, vs_, va_);
		link_->stats().rx_errors++;
		return;
	}
	const s16 waiting = static_cast<s16>((next_ - sent_ + INFO_FRAMES) %
					     INFO_FRAMES);
	sent_ = static_cast<s16>((sent_ + n) % INFO_FRAMES);
	if (waiting < n)
		next_ = sent_;
	va_ = static_cast<u8>(nr + 1);
	tries_ = 0;
	timer_restart();
}

/* go back: the outstanding frames go again from the oldest */
void HstLink::go_back()
{
	link_->stats().tx_again += static_cast<u32>((next_ - sent_ +
						     INFO_FRAMES) % INFO_FRAMES);
	next_ = sent_;
}

/* a data frame's information d (n octets), N(S) ns: the next in order
   to the DTE (dropped, unacknowledged, while it has not taken the last
   one's) and an RR; an old one an RR again; one ahead a REJ, once
   until the next in order comes */
void HstLink::rx_info(const u8 *d, s16 n, u8 ns)
{
	if (ns == static_cast<u8>(vr_ + 1)) {
		if (!backlog_.empty())
			return;
		if (n)
			backlog_.deliver(link_, d, n);
		vr_ = ns;
		reject_ = false;
		ack_due_ = true;
		return;
	}
	if (diff(vr_, ns) < 128) {
		ack_due_ = true;
		return;
	}
	if (!reject_) {
		reject_ = true;
		link_->stats().naks++;
		ctrl(T_REJ, vr_, ACK_TAIL, 2);
	}
}

/* a good frame from Hdlc: the first from the remote puts the link up;
   then by its type (too short a frame dropped): data (short too), an
   RR, a REJ (our frames from its N(R) + 1 again), the remote's ask to
   turn the channels (granted when our frames are out) or for another
   rate (not granted), its grant of ours (the channels turn) */
void HstLink::rx_complete(Frame *f)
{
	if (state_ == IDLE || state_ == DISC || f->count < 2)
		return;
	heard_ = true;
	if (state_ == WAIT)
		up();
	const u8 *b = f->buf;
	const s16 n = f->count;
	switch (b[0]) {
	case T_DATA:
		if (n - 2 > HST_RX_INFO)
			break;
		rx_info(b + 2, static_cast<s16>(n - 2), b[1]);
		return;
	case T_DATA_ACK:
		if (n < 4 || n - 4 > HST_RX_INFO)
			break;
		ack(b[2]);
		rx_info(b + 4, static_cast<s16>(n - 4), b[1]);
		return;
	case T_RR:
		ack(b[1]);
		return;
	case T_REJ:
		ack(b[1]);
		go_back();
		link_->stats().naks++;
		return;
	case T_ASK:
		if (n > 2 && b[2] == ASK_TURN) {
			if (fwd_ && !grant_frame_)
				grant_k_ = b[1];
			return;
		}
		if (!asks_logged_) {
			asks_logged_ = true;
			link_->logf("hst: the remote asks for another rate "
				    "(0x%02x): not granted.", n > 2 ? b[2] : 0);
		}
		return;
	case T_GRANT:
		if (!fwd_ && asked_ && b[1] == ask_k_)
			turn();
		return;
	default:
		if ((b[0] & 0x08) && (b[0] & 0x07) == n - 1) {
			/* short data: N(S) the nearest to V(R) + 1 with
			   those low bits */
			const u8 next = static_cast<u8>(vr_ + 1);
			const s16 d = static_cast<s16>(((b[0] >> 4) - (next & 15)) &
						       15);
			rx_info(b + 1, static_cast<s16>(n - 1),
				static_cast<u8>(next + (d < 8 ? d : d - 16)));
			return;
		}
		break;
	}
	link_->logf("err: hst: frame of %d octets (type 0x%02x) dropped.", n,
		    b[0]);
}

} // namespace dlink
