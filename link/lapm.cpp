/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    lapm.cpp - V.42 LAPM, the error correcting procedure (clause 8):
 *               XID negotiation, establishment, the data phase with REJ
 *               and timer recovery, release
 *
 *    Clause numbers are those of V.42 (03/2002).  Control frames (S, U,
 *    XID) wait in a ring of their own and go out first, oldest first.
 *    I frames are filled with the DTE's characters only when one may go
 *    out, and stay in the other ring until the remote acknowledges them.
 *    T401 is the Link's bit timer, which counts transmitted bits.
 *
 */

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "link.h"

namespace dlink {

/* the address octet (8.2.1, Table 6; DLCI 0, one octet): the
   originator's commands and the answerer's responses carry ADDR_ORIG,
   the answerer's commands and the originator's responses ADDR_ANS */
static const uint8_t ADDR_ORIG = 0x03, ADDR_ANS = 0x01;

/* the first control octet (8.2.2, Tables 7 and 8, modulo 128): S frames
   as a whole (N(R) and P/F are in the second octet), U frames with
   their P/F bit (PF) masked off */
static const uint8_t S_RR = 0x01, S_RNR = 0x05, S_REJ = 0x09, S_SREJ = 0x0d;
static const uint8_t U_SABME = 0x6f, U_DM = 0x0f, U_UI = 0x03, U_DISC = 0x43,
	U_UA = 0x63, U_FRMR = 0x87, U_XID = 0xaf, U_TEST = 0xe3;
static const uint8_t PF = 0x10;

/* the XID information field (12.2): the general purpose format, the
   parameter negotiation and private parameter groups and the user data
   subfield (which has no GL), and their parameters (Tables 11a, b, c) */
static const uint8_t FI_GENERAL = 0x82;
static const uint8_t GI_PARAM = 0x80, GI_PRIVATE = 0xf0, GI_USER = 0xff;
static const uint8_t PI_HDLC_OPT = 3, PI_N401_TX = 5, PI_N401_RX = 6,
	PI_K_TX = 7, PI_K_RX = 8;
static const uint8_t PI_V42BIS_SET = 0, PI_V42BIS_P0 = 1, PI_V42BIS_N2 = 2,
	PI_V42BIS_N7 = 3;
static const uint8_t PI_V44_SET = 0x40, PI_V44_C0 = 0x41, PI_V44_P0 = 0x42,
	PI_V44_P1T = 0x43, PI_V44_P1R = 0x44, PI_V44_P2T = 0x45,
	PI_V44_P2R = 0x46, PI_V44_P3T = 0x47, PI_V44_P3R = 0x48;

/* the HDLC optional functions we send (PL 3): the ISO bit positions 2,
   4, 8, 9, 12 and 16 only (Note 1 of Table 11a), so neither SREJ, TEST
   nor the 32-bit FCS is asked for */
static const uint32_t HDLC_OPT = 0x8a8900;

/* T401's bits when the data pump's rate is not known */
static const long T401_NO_RATE = 57600;

enum {
	SEQ_MASK = 0x7f,                /* sequence numbers modulo 128 */
	XID_DELAY = 384,                /* the originator's flags before its
					   first XID (48 octets) */
	NO_LIMIT = 1 << 30,             /* a V.44 parameter the remote does
					   not limit */
	V42BIS_DFLT_N2 = 512,           /* V.42bis's defaults (5.1) */
	V42BIS_DFLT_N7 = 6,
	XID_MAX = 77,                   /* our longest XID frame (V.42bis
					   and V.44 both offered) */
};

static_assert(XID_MAX <= 4 + LAPM_MAX_INFO, "an XID fits a frame slot");


/*
 *    helpers
 *
 */

/* b - a, modulo 128 */
static int seq_diff(uint8_t b, uint8_t a)
{
	return (b - a) & SEQ_MASK;
}

/* T401 (9.2.1): 2 s at the data pump's rate, in bits */
static long t401_bits(const Link *link)
{
	long rate = link->tx_rate();
	return rate ? 2 * rate : T401_NO_RATE;
}

/* P0 (V.42bis Annex A) counts its directions from the negotiation's
   initiator, Config::comp from us (bit 0: we decode): the two bits
   exchanged */
static int swap_dirs(int dirs)
{
	return (dirs & 1) << 1 | (dirs >> 1 & 1);
}

/* the value for k or N401 when ours and the remote's differ (9.2.3,
   9.2.4: between the initiator's and the default dflt): the default
   when they lie on its two sides, else the one nearer to it */
static unsigned settle(unsigned ours, unsigned remote, unsigned dflt)
{
	if ((ours < dflt) != (remote < dflt))
		return dflt;
	return ours < dflt ? std::max(ours, remote) : std::min(ours, remote);
}

/* a parameter: PI, PL, and v in PL octets, the most significant first;
   where the next one goes */
static uint8_t *put_param(uint8_t *p, uint8_t pi, uint32_t v, int pl)
{
	*p++ = pi;
	*p++ = (uint8_t)pl;
	while (pl--)
		*p++ = (uint8_t)(v >> 8 * pl);
	return p;
}

/* a parameter set identification: PI, PL 3 and the three characters of
   id; where the next parameter goes */
static uint8_t *put_id(uint8_t *p, uint8_t pi, const char *id)
{
	*p++ = pi;
	*p++ = 3;
	memcpy(p, id, 3);
	return p + 3;
}

/* the GI and GL of the group whose three header octets are at g and
   whose parameters end at end */
static void put_group(uint8_t *g, uint8_t gi, const uint8_t *end)
{
	const int gl = (int)(end - g - 3);
	g[0] = gi;
	g[1] = (uint8_t)(gl >> 8);
	g[2] = (uint8_t)gl;
}

/* a parameter value of n octets, the most significant first */
static uint32_t get_be(const uint8_t *p, int n)
{
	uint32_t v = 0;
	while (n--)
		v = v << 8 | *p++;
	return v;
}

/* a parameter value of n octets, the least significant first */
static uint32_t get_le(const uint8_t *p, int n)
{
	uint32_t v = 0;
	while (n--)
		v = v << 8 | p[n];
	return v;
}

/* a received value as a Config field: at most NO_LIMIT */
static int bounded(uint32_t v)
{
	return (int)std::min<uint32_t>(v, NO_LIMIT);
}

/* the parameters (PI, PL, PV) in the n octets at p, each given to
   fn(pi, pv, pl); one that runs past the end ends them */
template <class Fn>
static void for_each_param(const uint8_t *p, int n, Fn fn)
{
	for (int i = 0; i + 2 <= n;) {
		const int pl = p[i + 1];
		if (i + 2 + pl > n)
			break;
		fn(p[i], p + i + 2, pl);
		i += 2 + pl;
	}
}


/*
 *    the call
 *
 */

/* a new call over link: disconnected, no negotiation, REJ or busy
   condition, never up; the addresses of our role; our windows and
   information fields (bounded by what the frame slots hold); both
   rings empty, sequence numbers 0 */
void Lapm::init(Link *link, int tx_win, int rx_win, int tx_info,
		int rx_info)
{
	link_ = link;
	state_ = IDLE;
	config_ = was_up_ = reject_ = busy_ = peer_busy_ = false;
	rtx_count_ = 0;
	rx_count_ = rx_head_ = 0;
	cmd_addr_ = link->caller() ? ADDR_ORIG : ADDR_ANS;
	rsp_addr_ = link->caller() ? ADDR_ANS : ADDR_ORIG;
	tx_win_ = (unsigned)std::clamp(tx_win, 0, (int)LAPM_MAX_WIN);
	rx_win_ = (unsigned)std::clamp(rx_win, 0, (int)LAPM_MAX_WIN);
	tx_info_ = (unsigned)std::clamp(tx_info, 0, (int)LAPM_MAX_INFO);
	rx_info_ = (unsigned)std::clamp(rx_info, 0, (int)LAPM_MAX_INFO);
	for (int i = 0; i < INFO_FRAMES; i++) {
		Frame &f = info_buf_[i].frame;
		f = Frame();
		f.buf = info_buf_[i].buf;
		f.next = &info_buf_[(i + 1) % INFO_FRAMES].frame;
	}
	for (int i = 0; i < CTRL_FRAMES; i++) {
		Frame &f = ctrl_buf_[i].frame;
		f = Frame();
		f.buf = ctrl_buf_[i].buf;
		f.next = &ctrl_buf_[(i + 1) % CTRL_FRAMES].frame;
	}
	sent_info_ = tx_info_frame_ = info_list_ = &info_buf_[0].frame;
	tx_ctrl_ = ctrl_list_ = &ctrl_buf_[0].frame;
	vs_ = va_ = vr_ = 0;
}

/* the end of the call: no rings (tx_pending() is false), nothing kept
   for the DTE, disconnected; safe to repeat, and before any init() */
void Lapm::exit()
{
	sent_info_ = tx_info_frame_ = info_list_ = nullptr;
	tx_ctrl_ = ctrl_list_ = nullptr;
	rx_count_ = rx_head_ = 0;
	state_ = IDLE;
	config_ = false;
}

/* LAPM's turn after the detection phase: our frames to Hdlc, at once at
   the answerer (which waits for the originator's XID and SABME), after
   48 octets of flags at the originator, whose XID goes then
   (negotiate()) */
void Lapm::start()
{
	const bool caller = link_->caller();

	link_->hdlc().set_framer(this, !caller);
	if (caller)
		link_->set_timer(XID_DELAY, Link::Timer::Negotiate);
	link_->push_pending = false;
}

/* the originator's flags are out: its frames go from now on, the first
   of them its XID */
void Lapm::negotiate()
{
	link_->hdlc().enable_tx(true);
	configure();
}

/* we hang up (L-RELEASE, 8.7), in any state: DISC, T401 for its
   answer; no I frame goes from now on, and those held go at the
   release's sequence reset */
void Lapm::disconnect()
{
	state_ = DISC;
	tx_unnum(cmd_addr_, (uint8_t)(U_DISC | PF));
	timer_start();
}

/* the received characters the DTE did not take, offered again (8.4.7):
   once it has them all, our receiver is no longer busy, which an RR
   command says */
void Lapm::push_rest()
{
	if (state_ != DATA || !rx_count_)
		return;
	int taken = link_->write_chars(rx_buf_ + rx_head_, rx_count_);
	if (taken > 0) {
		rx_head_ += taken;
		rx_count_ -= taken;
	}
	if (rx_count_)
		return;
	busy_ = false;
	tx_super(cmd_addr_, S_RR, 0);
	link_->push_pending = false;
}


/*
 *    state changes and timers
 *
 */

/* the sequence reset (8.2.4.3): neither side busy, V(S), V(A) and V(R)
   0, the I frames held discarded */
void Lapm::reset()
{
	busy_ = peer_busy_ = false;
	vs_ = va_ = vr_ = 0;
	sent_info_ = tx_info_frame_ = info_list_;
}

/* the data phase begins, after a sequence reset: the link is up (once
   in this call or again) */
void Lapm::connect()
{
	reset();
	state_ = DATA;
	was_up_ = true;
	link_->logf("ec: connected.");
	link_->status(Status::Up);
}

/* an XID exchange of ours (L-SETPARM, 8.10.2): no I frame goes until
   its answer, for which T401 runs; in the data phase our receiver is
   busy meanwhile (8.10.1) */
void Lapm::configure()
{
	config_ = true;
	if (state_ == DATA) {
		busy_ = true;
		tx_super(cmd_addr_, S_RNR, 1);
	}
	tx_xid(cmd_addr_);
	timer_start();
}

/* T401 starts again, the retransmission count from 0 */
void Lapm::timer_start()
{
	rtx_count_ = 0;
	link_->set_timer(t401_bits(link_), Link::Timer::T401);
}

/* T401 stops, the retransmission count back to 0 */
void Lapm::timer_stop()
{
	rtx_count_ = 0;
	link_->set_timer(0, Link::Timer::None);
}

/* T401 has run out: below N400 retransmissions, the frame that awaits
   an answer once more and T401 again, the count kept (the XID, 8.10.3;
   SABME, 8.3.2.2; DISC, 8.7.3; a poll in the data phase, 8.4.8 a); at
   N400, LAPM fails: a remote that never answered has no LAPM (the
   Link goes on without it), an establishment or release ends
   disconnected, the data phase ends with our DISC (8.4.8 b, 8.4.9);
   while disconnected there is nothing to send again, and T401 stays
   off */
void Lapm::t401_timeout()
{
	if (state_ == IDLE && !config_) {
		rtx_count_ = 0;
		return;
	}
	if (rtx_count_ < MAX_RTX) {
		rtx_count_++;
		if (config_)
			tx_xid(cmd_addr_);
		else if (state_ == ESTAB)
			tx_unnum(cmd_addr_, (uint8_t)(U_SABME | PF));
		else if (state_ == DISC)
			tx_unnum(cmd_addr_, (uint8_t)(U_DISC | PF));
		else
			tx_super(cmd_addr_, busy_ ? S_RNR : S_RR, 1);
		link_->set_timer(t401_bits(link_), Link::Timer::T401);
		return;
	}
	rtx_count_ = 0;
	if (!was_up_ && (config_ || state_ == ESTAB)) {
		link_->logf("no LAPM from the remote: async");
		config_ = false;
		state_ = IDLE;
		/* before the Link's next mode sets its own timer */
		timer_stop();
		link_->detected(false);
		return;
	}
	switch (state_) {
	case ESTAB:
	case DISC:
		link_->logf("ec: no answer to %s: disconnected.",
			    state_ == ESTAB ? "SABME" : "DISC");
		state_ = IDLE;
		link_->status(Status::Released);
		break;
	case DATA:
		link_->logf("ec: no answer to %d polls: release.", MAX_RTX);
		disconnect();
		break;
	case IDLE:
		break;
	}
}


/*
 *    frames to send
 *
 */

/* the control ring's next free slot, queued (to be filled before the
   framer asks for a frame); NULL when seven frames wait already: the
   eighth slot holds the frame going out until the framer asks for the
   next one */
Frame *Lapm::ctrl_frame()
{
	if (ctrl_list_->next == tx_ctrl_) {
		link_->logf("err: ec: control queue full: frame dropped.");
		return nullptr;
	}
	Frame *f = ctrl_list_;
	ctrl_list_ = f->next;
	return f;
}

/* a U frame queued: address addr, control octet ctrl (P/F included);
   -1 if the queue is full */
int Lapm::tx_unnum(uint8_t addr, uint8_t ctrl)
{
	Frame *f = ctrl_frame();
	if (!f)
		return -1;
	f->buf[0] = addr;
	f->buf[1] = ctrl;
	f->size = 2;
	return 0;
}

/* an S frame queued: address addr, function ctrl, N(R) = V(R) now and
   P/F pf; -1 if the queue is full */
int Lapm::tx_super(uint8_t addr, uint8_t ctrl, int pf)
{
	Frame *f = ctrl_frame();
	if (!f)
		return -1;
	f->buf[0] = addr;
	f->buf[1] = ctrl;
	f->buf[2] = (uint8_t)(vr_ << 1 | (pf ? 1 : 0));
	f->size = 3;
	return 0;
}

/* our XID (12.2) queued, a command (P 0) or, with our response address,
   a response (F 0), from the Link's configuration and our k and N401:
   a command offers the compression asked for (V.42bis, and V.44 in the
   user data subfield), a response says what was agreed (one algorithm,
   7.3/V.44); -1 if the queue is full */
int Lapm::tx_xid(uint8_t addr)
{
	Frame *f = ctrl_frame();
	if (!f)
		return -1;
	const Config &c = link_->cfg();
	const bool response = addr == rsp_addr_;
	/* Table 11a's directions are the initiator's (its Note 2) */
	const unsigned info_i = response ? rx_info_ : tx_info_;
	const unsigned info_r = response ? tx_info_ : rx_info_;
	const unsigned win_i = response ? rx_win_ : tx_win_;
	const unsigned win_r = response ? tx_win_ : rx_win_;
	/* P0 counts from the initiator, for V.42bis and V.44 alike */
	const int p0 = response ? c.comp & 3 : swap_dirs(c.comp);
	const bool v44 = c.comp && c.v44;
	const bool v42bis = c.comp && !(response && c.v44);
	uint8_t *p = f->buf, *g;

	*p++ = addr;
	*p++ = U_XID;
	*p++ = FI_GENERAL;
	g = p;
	p = put_param(p + 3, PI_HDLC_OPT, HDLC_OPT, 3);
	p = put_param(p, PI_N401_TX, info_i * 8, 2);
	p = put_param(p, PI_N401_RX, info_r * 8, 2);
	p = put_param(p, PI_K_TX, win_i, 1);
	p = put_param(p, PI_K_RX, win_r, 1);
	put_group(g, GI_PARAM, p);
	if (v42bis) {
		g = p;
		p = put_id(p + 3, PI_V42BIS_SET, "V42");
		p = put_param(p, PI_V42BIS_P0, p0, 1);
		p = put_param(p, PI_V42BIS_N2, c.dict_size, 2);
		p = put_param(p, PI_V42BIS_N7, c.max_string, 1);
		put_group(g, GI_PRIVATE, p);
	}
	if (v44) {
		*p++ = GI_USER;
		p = put_id(p, PI_V44_SET, "V44");
		p = put_param(p, PI_V44_C0, 0, 1);
		p = put_param(p, PI_V44_P0, p0, 1);
		p = put_param(p, PI_V44_P1T, c.v44_codewords, 2);
		p = put_param(p, PI_V44_P1R, c.v44_codewords, 2);
		p = put_param(p, PI_V44_P2T, c.v44_max_string, 1);
		p = put_param(p, PI_V44_P2R, c.v44_max_string, 1);
		p = put_param(p, PI_V44_P3T, c.v44_history, 2);
		p = put_param(p, PI_V44_P3R, c.v44_history, 2);
	}
	f->size = (int)(p - f->buf);
	return 0;
}

/* "an I frame can be sent" (8.4.1, 8.4.2.2): the remote is not busy,
   fewer than k frames are outstanding, and one waits to be sent (again,
   after a go back) or is filled now with up to N401 of the DTE's
   characters; 1 if so.  A read error is reported to the Link */
int Lapm::tx_info()
{
	if (peer_busy_ || (unsigned)seq_diff(vs_, va_) >= tx_win_)
		return 0;
	if (tx_info_frame_ != info_list_)
		return 1;
	Frame *f = info_list_;
	int n = link_->read_chars(f->buf + 3, (int)tx_info_);
	if (n < 0) {
		link_->logf("err: ec: DTE read error (%d).", n);
		link_->status(Status::Error);
		return 0;
	}
	if (!n)
		return 0;
	f->size = 3 + n;
	info_list_ = f->next;
	return 1;
}

/* the framer's next frame: the oldest control frame waiting, else in
   the data phase (no negotiation going on) an I frame if one can be
   sent, numbered now (N(S) = V(S), which advances, and N(R) = V(R)),
   T401 started if the bit timer is not running; NULL for flags */
Frame *Lapm::next_tx_frame()
{
	if (tx_ctrl_ != ctrl_list_) {
		Frame *f = tx_ctrl_;
		tx_ctrl_ = f->next;
		return f;
	}
	if (state_ != DATA || config_ || !tx_info())
		return nullptr;
	Frame *f = tx_info_frame_;
	f->buf[0] = cmd_addr_;
	f->buf[1] = (uint8_t)(vs_ << 1);
	f->buf[2] = (uint8_t)(vr_ << 1);
	vs_ = (uint8_t)((vs_ + 1) & SEQ_MASK);
	tx_info_frame_ = f->next;
	if (!link_->timer())
		timer_start();
	return f;
}

/* a frame is out: nothing to do (I frames are freed by their
   acknowledgement, control slots by the order of their ring) */
void Lapm::tx_complete(Frame *f)
{
	(void)f;
}


/*
 *    received frames
 *
 */

/* a good frame from Hdlc: too short a frame, or an I frame longer than
   our largest information field, dropped (8.1.3, 8.5.4); I and S
   frames as the state allows, U frames in every state; ours is the
   response address of the commands the remote sends */
void Lapm::rx_complete(Frame *f)
{
	if (f->count < 2) {
		link_->logf("err: ec: frame of %d octets dropped.", f->count);
		return;
	}
	const bool cmd = f->buf[0] == rsp_addr_;
	const uint8_t ctrl = f->buf[1];
	if ((ctrl & 3) == 3) {
		if (cmd)
			rx_unnum_cmd(f);
		else
			rx_unnum_rsp(f);
		return;
	}
	if (f->count < 3) {
		log_frame("err: ec: I or S frame too short", cmd, f);
		return;
	}
	if (!(ctrl & 1)) {
		if (f->count - 3 > LAPM_MAX_INFO) {
			log_frame("err: ec: I frame too long", cmd, f);
			return;
		}
		if (valid_data_state())
			rx_info(f);
		return;
	}
	if (!valid_data_state())
		return;
	if (cmd)
		rx_super_cmd(f);
	else
		rx_super_rsp(f);
}

/* whether an I or S frame is for the data phase: not while disconnected
   (8.8); awaiting establishment, the frame means that the UA answering
   our SABME was lost, so the data phase begins with it (8.3.2.1; T401
   runs on); awaiting release, the link is released at once */
bool Lapm::valid_data_state()
{
	switch (state_) {
	case IDLE:
		return false;
	case ESTAB:
		connect();
		return true;
	case DATA:
		return true;
	case DISC:
		reset();
		state_ = IDLE;
		link_->logf("ec: released (data frame while releasing).");
		link_->status(Status::Released);
		return false;
	}
	return false;
}

/* the N(R) of an I, RR, RNR or REJ frame (8.4.3.2): our I frames up to
   N(R) - 1 acknowledged and freed (not past V(S)); out of timer
   recovery T401 stops when some were, and starts again while others
   are outstanding; how many */
int Lapm::ack_info(uint8_t nr)
{
	const int sent = seq_diff(vs_, va_);
	int n = seq_diff(nr, va_);
	if (n > sent) {
		link_->logf("err: ec: N(R) %d past V(S) %d (V(A) %d).", nr, vs_,
			    va_);
		n = sent;
	}
	for (int i = 0; i < n; i++)
		sent_info_ = sent_info_->next;
	va_ = (uint8_t)((va_ + n) & SEQ_MASK);
	if (n && !rtx_count_) {
		timer_stop();
		if (vs_ != va_)
			timer_start();
	}
	return n;
}

/* go back N (8.4.4, 8.4.8): in the data phase the frames outstanding
   wait to be sent again from the oldest (with new numbers), before any
   new one, and are counted as sent again; how many */
int Lapm::reject_info()
{
	if (state_ != DATA)
		return 0;
	const int n = seq_diff(vs_, va_);
	vs_ = va_;
	tx_info_frame_ = sent_info_;
	link_->stats().tx_again += n;
	return n;
}

/* an I frame in the data phase (8.4.2, 8.4.7, 8.5.1): its N(R) first;
   discarded while we are busy (RNR when polled) or out of sequence (one
   REJ until the frame comes); else its information to the DTE, what
   it does not take kept (our receiver busy until push_rest() delivers
   it), V(R) on, and an answer at its own address unless an I frame
   about to go acknowledges it; 1 if it was taken */
int Lapm::rx_info(Frame *f)
{
	const uint8_t ns = f->buf[1] >> 1, nr = f->buf[2] >> 1;
	const int pf = f->buf[2] & 1;

	ack_info(nr);
	if (busy_) {
		if (pf)
			tx_super(rsp_addr_, S_RNR, 1);
		return 0;
	}
	if (ns != vr_) {
		if (!reject_) {
			tx_super(rsp_addr_, S_REJ, pf);
			reject_ = true;
		}
		return 0;
	}
	reject_ = false;
	const int n = f->count - 3;
	int taken = link_->write_chars(f->buf + 3, n);
	if (taken < 0)
		taken = 0;
	if (taken < n) {
		rx_head_ = 0;
		rx_count_ = n - taken;
		memcpy(rx_buf_, f->buf + 3 + taken, rx_count_);
		busy_ = true;
		link_->push_pending = true;
	}
	vr_ = (uint8_t)((vr_ + 1) & SEQ_MASK);
	if (busy_)
		tx_super(f->buf[0], S_RNR, pf);
	else if (peer_busy_ || pf || !tx_info())
		tx_super(f->buf[0], S_RR, pf);
	return 1;
}

/* an S command in the data phase: RR and REJ clear the remote's busy
   condition, RNR sets it (8.4.6), all acknowledge; a REJ out of timer
   recovery sends from its N(R) again (8.4.4 a; in it only the N(R)
   counts, 8.4.4 c); a poll is answered with our receiver's state, and
   an RR or REJ also when no I frame can go to acknowledge it; SREJ
   was never agreed (8.4.5.1) */
void Lapm::rx_super_cmd(Frame *f)
{
	const uint8_t nr = f->buf[2] >> 1;
	const int pf = f->buf[2] & 1;

	switch (f->buf[1]) {
	case S_RR:
		peer_busy_ = false;
		ack_info(nr);
		break;
	case S_RNR:
		peer_busy_ = true;
		ack_info(nr);
		if (pf)
			tx_super(rsp_addr_, busy_ ? S_RNR : S_RR, 1);
		return;
	case S_REJ:
		peer_busy_ = false;
		ack_info(nr);
		if (!rtx_count_) {
			timer_stop();
			reject_info();
		}
		break;
	case S_SREJ:
		log_frame("err: ec: SREJ", true, f);
		return;
	default:
		log_frame("err: ec: unknown S frame", true, f);
		return;
	}
	if (pf || !tx_info())
		tx_super(rsp_addr_, busy_ ? S_RNR : S_RR, pf);
}

/* an S response in the data phase: with F 1 it ends our timer recovery
   (8.4.8: an RR or RNR sends again what is outstanding; an unsolicited
   one is ignored, Table 9), and an RNR keeps T401 running so that the
   polls go on (8.4.6); a REJ sends again from its N(R) out of timer
   recovery or with F 1 (8.4.4 a, b), else only acknowledges (8.4.4 c) */
void Lapm::rx_super_rsp(Frame *f)
{
	const uint8_t nr = f->buf[2] >> 1;
	const bool fin = f->buf[2] & 1;

	if (fin && !rtx_count_) {
		log_frame("err: ec: unsolicited final response", false, f);
		return;
	}
	/* (from here on, F 1 means timer recovery) */
	switch (f->buf[1]) {
	case S_RR:
		peer_busy_ = false;
		ack_info(nr);
		if (fin) {
			reject_info();
			timer_stop();
		}
		break;
	case S_RNR:
		peer_busy_ = true;
		ack_info(nr);
		if (fin) {
			reject_info();
			timer_stop();
		}
		if (!rtx_count_)
			timer_start();
		break;
	case S_REJ:
		peer_busy_ = false;
		ack_info(nr);
		if (!rtx_count_ || fin) {
			reject_info();
			timer_stop();
		}
		break;
	case S_SREJ:
		log_frame("err: ec: SREJ", false, f);
		break;
	default:
		log_frame("err: ec: unknown S frame", false, f);
		break;
	}
}

/* a U command, in any state: SABME (re-)establishes and is always
   accepted (8.3.2.1, 8.4.9.1); DISC releases (8.7.2), or gets a DM
   while disconnected (8.8); an XID is answered with what was agreed
   (8.10); UI (break) and TEST (loop-back) are not implemented */
void Lapm::rx_unnum_cmd(Frame *f)
{
	const uint8_t pf = f->buf[1] & PF;

	switch (f->buf[1] & ~PF) {
	case U_SABME:
		connect();
		tx_unnum(rsp_addr_, (uint8_t)(U_UA | pf));
		timer_stop();
		break;
	case U_DISC:
		if (state_ == IDLE) {
			tx_unnum(rsp_addr_, (uint8_t)(U_DM | PF));
			break;
		}
		reset();
		state_ = IDLE;
		tx_unnum(rsp_addr_, (uint8_t)(U_UA | pf));
		timer_stop();
		link_->logf("ec: released by the remote.");
		link_->status(Status::Released);
		break;
	case U_XID:
		rx_xid(f, false);
		tx_xid(rsp_addr_);
		break;
	case U_UI:
	case U_TEST:
		break;
	default:
		log_frame("err: ec: unknown U frame", true, f);
		break;
	}
}

/* a U response: UA completes an establishment or release; DM with F 1
   answers our SABME or DISC (or a poll, in timer recovery) and ends
   the link; DM with F 0 asks for the link while it has not been up in
   this call (8.8, Table 9), and ends the data phase (8.4.9.1); the
   XID answering ours ends the negotiation, then the establishment
   begins (or, in the data phase, our receiver is free again); UI and
   FRMR are ignored */
void Lapm::rx_unnum_rsp(Frame *f)
{
	const bool fin = f->buf[1] & PF;
	/* the link's end: sequence reset, disconnected, Released */
	auto release = [this](bool stop_t401) {
		reset();
		state_ = IDLE;
		if (stop_t401)
			timer_stop();
		link_->logf("ec: released.");
		link_->status(Status::Released);
	};
	/* L-ESTABLISH (8.3): SABME after a sequence reset, T401 for the UA */
	auto establish = [this]() {
		reset();
		state_ = ESTAB;
		tx_unnum(cmd_addr_, (uint8_t)(U_SABME | PF));
		timer_start();
	};

	switch (f->buf[1] & ~PF) {
	case U_UA:
		if (state_ == ESTAB) {
			connect();
			timer_stop();
		} else if (state_ == DISC)
			release(true);
		break;
	case U_DM:
		if (fin) {
			if (state_ == ESTAB || state_ == DISC)
				release(true);
			else if (state_ == DATA && rtx_count_)
				release(false);
		} else if (state_ == IDLE && !was_up_) {
			config_ = false;
			establish();
		} else if (state_ == DATA)
			release(false);
		break;
	case U_XID:
		if (!config_)
			break;
		rx_xid(f, true);
		config_ = false;
		timer_stop();
		if (state_ == IDLE)
			establish();
		else if (state_ == DATA) {
			busy_ = false;
			tx_super(cmd_addr_, S_RR, 0);
		}
		break;
	case U_UI:
	case U_FRMR:
		break;
	default:
		log_frame("err: ec: unknown U frame", false, f);
		break;
	}
}

/* the remote's XID (a response to ours, or a command), read (12.2.2;
   what we do not know is passed over, and nothing past the frame's
   end): k and N401 settled by 9.2.3 and 9.2.4, the compression it asks
   for (V.42bis's private parameters, V.44's user data subfield) to
   the Link, which decides; nothing without the general purpose
   format */
void Lapm::rx_xid(Frame *f, bool response)
{
	const uint8_t *x = f->buf + 2;
	const int len = f->count - 2;

	if (len < 1 || x[0] != FI_GENERAL) {
		link_->logf("ec: XID %s without parameters.",
			    response ? "response" : "command");
		return;
	}
	Config r;
	int p0 = 0, v44_p0 = 0;
	bool v44_set = false;
	r.dict_size = V42BIS_DFLT_N2;
	r.max_string = V42BIS_DFLT_N7;
	r.v44_codewords = r.v44_max_string = r.v44_history = NO_LIMIT;
	/* Table 11a's directions are the initiator's (its Note 2): ours
	   when this answers our command */
	unsigned &info_i = response ? tx_info_ : rx_info_;
	unsigned &info_r = response ? rx_info_ : tx_info_;
	unsigned &win_i = response ? tx_win_ : rx_win_;
	unsigned &win_r = response ? rx_win_ : tx_win_;

	auto param = [&](uint8_t pi, const uint8_t *pv, int pl) {
		const uint32_t v = get_be(pv, pl);
		switch (pi) {
		case PI_N401_TX:
			info_i = settle(info_i, v / 8, LAPM_DFLT_INFO);
			break;
		case PI_N401_RX:
			info_r = settle(info_r, v / 8, LAPM_DFLT_INFO);
			break;
		case PI_K_TX:
			win_i = settle(win_i, v, LAPM_DFLT_WIN);
			break;
		case PI_K_RX:
			win_r = settle(win_r, v, LAPM_DFLT_WIN);
			break;
		}
	};
	auto priv = [&](uint8_t pi, const uint8_t *pv, int pl) {
		switch (pi) {
		case PI_V42BIS_P0:
			p0 = (int)(get_le(pv, pl) & 3);
			break;
		case PI_V42BIS_N2:
			r.dict_size = bounded(get_be(pv, pl));
			break;
		case PI_V42BIS_N7:
			r.max_string = bounded(get_be(pv, pl));
			break;
		}
	};
	auto user = [&](uint8_t pi, const uint8_t *pv, int pl) {
		const int v = bounded(get_be(pv, pl));
		switch (pi) {
		case PI_V44_SET:
			v44_set = pl == 3 && !memcmp(pv, "V44", 3);
			break;
		case PI_V44_P0:
			v44_p0 = v;
			break;
		case PI_V44_P1T:
		case PI_V44_P1R:
			r.v44_codewords = std::min(r.v44_codewords, v);
			break;
		case PI_V44_P2T:
		case PI_V44_P2R:
			r.v44_max_string = std::min(r.v44_max_string, v);
			break;
		case PI_V44_P3T:
		case PI_V44_P3R:
			r.v44_history = std::min(r.v44_history, v);
			break;
		}
	};

	for (int i = 1; i < len;) {
		const uint8_t gi = x[i];
		if (gi == GI_USER) {
			for_each_param(x + i + 1, len - i - 1, user);
			break;
		}
		if (i + 3 > len)
			break;
		const int gl = x[i + 1] << 8 | x[i + 2];
		if (i + 3 + gl > len)
			break;
		if (gi == GI_PARAM)
			for_each_param(x + i + 3, gl, param);
		else if (gi == GI_PRIVATE)
			for_each_param(x + i + 3, gl, priv);
		i += 3 + gl;
	}

	/* V.44 when its set is named and it asks for a direction: its P0
	   then says which */
	r.v44 = v44_set && v44_p0;
	if (r.v44)
		p0 = v44_p0 & 3;
	r.comp = response ? swap_dirs(p0) : p0;
	r.tx_win = (int)tx_win_;
	r.rx_win = (int)rx_win_;
	r.tx_info = (int)tx_info_;
	r.rx_info = (int)rx_info_;
	r.ec = r.detect = true;
	link_->logf("ec: XID %s: compression %d%s, k %u/%u, N401 %u/%u.",
		    response ? "response" : "command", r.comp,
		    r.v44 ? " (V.44)" : "", tx_win_, rx_win_, tx_info_,
		    rx_info_);
	link_->update_config(r);
}

/* a line about the frame f (a command, or a response): title, then its
   length and first octets */
void Lapm::log_frame(const char *title, bool cmd, const Frame *f)
{
	char hex[3 * 3 + 1] = "";
	const int n = std::min(f->count, 3);

	for (int i = 0; i < n; i++)
		snprintf(hex + 3 * i, sizeof(hex) - 3 * i, " %02x", f->buf[i]);
	link_->logf("%s (%s, %d octets):%s.", title,
		    cmd ? "command" : "response", f->count, hex);
}

} // namespace dlink
