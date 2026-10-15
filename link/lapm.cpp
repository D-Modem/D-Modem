/*
 * SPDX-License-Identifier: AGPL-3.0-only AND BSD-3-Clause
 * Copyright (C) 2026 Neo-Desktop
 * (Smart Link's code in it: Smart Link's notice and licence below)
 *
 *    lapm.cpp - V.42 LAPM
 *
 *    From slmodemd's modem_ec.c:
 *
 *    Copyright (c) 2002, Smart Link Ltd.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions
 *    are met:
 *
 *        1. Redistributions of source code must retain the above copyright
 *           notice, this list of conditions and the following disclaimer.
 *        2. Redistributions in binary form must reproduce the above
 *           copyright notice, this list of conditions and the following
 *           disclaimer in the documentation and/or other materials provided
 *           with the distribution.
 *        3. Neither the name of the Smart Link Ltd. nor the names of its
 *           contributors may be used to endorse or promote products derived
 *           from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <string.h>

#include <algorithm>

#include "link.h"

namespace dlink {

/* frame types */
enum { FRAME_I = 0x00, FRAME_S = 0x01, FRAME_U = 0x03 };
/* supervisory frames */
enum { S_RR = 0x01, S_RNR = 0x05, S_REJ = 0x09, S_SREJ = 0x0d };
/* unnumbered frames (without the P/F bit) */
enum {
	U_SABME = 0x6f, U_DM = 0x0f, U_UI = 0x03, U_DISC = 0x43,
	U_UA = 0x63, U_FRMR = 0x87, U_XID = 0xaf, U_TEST = 0xe3,
};
/* XID */
enum {
	FI_GENERAL = 0x82,
	GI_PARAM_NEGOTIATION = 0x80,
	GI_PRIVATE_NEGOTIATION = 0xf0,
	PI_HDLC_OPTFUNCS = 0x03,
	PI_TX_INFO_MAXSIZE = 0x05,
	PI_RX_INFO_MAXSIZE = 0x06,
	PI_TX_WINDOW_SIZE = 0x07,
	PI_RX_WINDOW_SIZE = 0x08,
	PI_PARAMETER_SET_ID = 0x00,
	PI_V42BIS_REQUEST = 0x01,
	PI_V42BIS_CW_NUMBER = 0x02,
	PI_V42BIS_MAX_STRING = 0x03,
	/* V.44: the user data subfield, to the end of the frame */
	GI_USER_DATA = 0xff,
	PI_V44_SET_ID = 0x40,           /* "V44" */
	PI_V44_C0 = 0x41,               /* the method: 0, stream */
	PI_V44_P0 = 0x42,               /* the directions, as V.42bis P0 */
	PI_V44_N2T = 0x43, PI_V44_N2R = 0x44,   /* codewords */
	PI_V44_N7T = 0x45, PI_V44_N7R = 0x46,   /* the longest string */
	PI_V44_N8T = 0x47, PI_V44_N8R = 0x48,   /* the history */
};

/* the frame's type by its control field: FRAME_I, FRAME_S or FRAME_U */
static int frame_type(const Frame *f)
{
	return f->buf[1] & 1 ? f->buf[1] & 3 : FRAME_I;
}
/* the first control octet: an S or U frame's command */
static int frame_ctrl(const Frame *f) { return f->buf[1]; }
/* the address octet */
static int frame_addr(const Frame *f) { return f->buf[0]; }
/* the P/F bit of an I frame (modulo 128: the second control octet) */
static int i_pf(const Frame *f) { return f->buf[2] & 1; }
/* the P/F bit of an S frame */
static int s_pf(const Frame *f) { return f->buf[2] & 1; }
/* the P/F bit of a U frame (0x10, not shifted) */
static int u_pf(const Frame *f) { return f->buf[1] & 0x10; }
/* N(S) of an I frame */
static int frame_ns(const Frame *f) { return f->buf[1] >> 1; }
/* N(R) of an I or S frame */
static int frame_nr(const Frame *f) { return f->buf[2] >> 1; }

/* a parameter value of len octets, the first the least significant */
static uint32_t get_le(const uint8_t *buf, int len)
{
	uint32_t v = 0;
	while (len--) {
		v <<= 8;
		v |= buf[len];
	}
	return v;
}

/* a parameter value of len octets, the first the most significant */
static uint32_t get_be(const uint8_t *buf, int len)
{
	uint32_t v = 0;
	while (len--) {
		v <<= 8;
		v |= *buf++;
	}
	return v;
}

/* 2 s at the data pump's rate (a fixed count of 57600 bits would be 48 s
   at 1200 bit/s) */
static long t401_bits(Link *link)
{
	int rate = link->tx_rate();
	return rate ? rate * 2L : 28800 * 2L;
}

/* an error line in the link's log */
#define EC_ERR(...) link_->logf("err: ec: " __VA_ARGS__)


/*
 *    frames
 *
 */

/* the next free control frame, queued to go out (the caller fills it
   before the framer asks); one of the ring is kept free; NULL if none */
Frame *Lapm::ctrl_frame()
{
	if (ctrl_list_->next == tx_ctrl_)
		return nullptr;
	Frame *f = ctrl_list_;
	ctrl_list_ = ctrl_list_->next;
	return f;
}

/* queues a U frame: address and control octet (with its P/F bit); -1
   if no control frame is free */
int Lapm::tx_unnum(uint8_t addr, uint8_t ctrl)
{
	Frame *f = ctrl_frame();
	if (!f) {
		EC_ERR("tx_unnum: cannot alloc frame.");
		return -1;
	}
	f->buf[0] = addr;
	f->buf[1] = ctrl;
	f->size = 2;
	return 0;
}

/* queues an S frame with N(R) = V(R) and the P/F bit pf; -1 if no
   control frame is free */
int Lapm::tx_super(uint8_t addr, uint8_t ctrl, int pf)
{
	Frame *f = ctrl_frame();
	if (!f) {
		EC_ERR("tx_super: cannot alloc frame.");
		return -1;
	}
	f->buf[0] = addr;
	f->buf[1] = ctrl;
	f->buf[2] = (uint8_t)(vr_ << 1 | (pf ? 1 : 0));
	f->size = 3;
	return 0;
}

/* 6.4/V.42: a value between ours and the default */
static void set_param(unsigned &param, unsigned value, unsigned dflt)
{
	if ((value < dflt && param >= dflt) ||
	    (value >= dflt && param < dflt))
		param = dflt;
	else if ((value < dflt && param < value) ||
		 (value >= dflt && param > value))
		param = value;
}

/* the V.44 parameters of the user data subfield; the smaller of each
   pair (we use the same both ways) */
static void rx_v44(Config &cfg, const uint8_t *p, int len)
{
	bool v44 = false;
	int dir = 0;
	cfg.v44_codewords = cfg.v44_max_string = cfg.v44_history = 1 << 30;
	while (len >= 2) {
		uint8_t pid = p[0], plen = p[1];
		p += 2;
		len -= 2 + plen;
		if (len < 0)
			break;
		int v = (int)get_be(p, plen);
		switch (pid) {
		case PI_V44_SET_ID:
			v44 = plen == 3 && !memcmp(p, "V44", 3);
			break;
		case PI_V44_P0:
			dir = v;
			break;
		case PI_V44_N2T: case PI_V44_N2R:
			cfg.v44_codewords = std::min(cfg.v44_codewords, v);
			break;
		case PI_V44_N7T: case PI_V44_N7R:
			cfg.v44_max_string = std::min(cfg.v44_max_string, v);
			break;
		case PI_V44_N8T: case PI_V44_N8R:
			cfg.v44_history = std::min(cfg.v44_history, v);
			break;
		default:
			break;
		}
		p += plen;
	}
	if (v44 && dir) {
		cfg.v44 = true;
		cfg.comp = dir;
	}
}

/* an XID command or response: our windows and information fields
   negotiated in place (set_param(), sizes in bits on the line), the
   remote's V.42bis private parameters and V.44 user data subfield to
   the Link (update_config()); without the general format identifier
   it is ignored */
/* V.42bis's and V.44's P0 count from the XID's initiator (bit 0: the
   initiator to the responder); Config::comp from us (bit 0: what we
   receive): the same at the responder, swapped at the initiator */
static int p0_swap(int p0)
{
	return (p0 >> 1 & 1) | (p0 & 1) << 1;
}

void Lapm::rx_xid(Frame *f, bool response)
{
	/* what the remote asks for; V.42bis off unless it asks */
	Config cfg;
	cfg.ec = true;
	cfg.detect = true;
	cfg.comp = 0;
	cfg.dict_size = 512;
	cfg.max_string = 6;
	cfg.v44 = false;
	const uint8_t *buf = f->buf + 2;
	int len = f->count - 2;

	if (*buf != FI_GENERAL)
		return;
	buf++;
	len--;
	while (len > 0) {
		uint8_t gid = buf[0];
		if (gid == GI_USER_DATA) {
			rx_v44(cfg, buf + 1, len - 1);
			break;
		}
		int glen = buf[1] << 8 | buf[2];
		buf += 3;
		len -= 3 + glen;
		if (len < 0)
			break;
		const uint8_t *p = buf;
		int plen_left = glen;
		while (plen_left > 0) {
			uint8_t pid = p[0], plen = p[1];
			p += 2;
			plen_left -= 2 + plen;
			if (plen_left < 0)
				break;
			if (gid == GI_PARAM_NEGOTIATION) {
				switch (pid) {
				case PI_TX_INFO_MAXSIZE:
					set_param(tx_info_, get_be(p, plen) >> 3,
						  LAPM_DFLT_INFO);
					break;
				case PI_RX_INFO_MAXSIZE:
					set_param(rx_info_, get_be(p, plen) >> 3,
						  LAPM_DFLT_INFO);
					break;
				case PI_TX_WINDOW_SIZE:
					set_param(tx_win_, get_be(p, plen),
						  LAPM_DFLT_WIN);
					break;
				case PI_RX_WINDOW_SIZE:
					set_param(rx_win_, get_be(p, plen),
						  LAPM_DFLT_WIN);
					break;
				default:
					break;
				}
			} else if (gid == GI_PRIVATE_NEGOTIATION) {
				switch (pid) {
				case PI_V42BIS_REQUEST:
					cfg.comp = (int)get_le(p, plen);
					break;
				case PI_V42BIS_CW_NUMBER:
					cfg.dict_size = (int)get_be(p, plen);
					break;
				case PI_V42BIS_MAX_STRING:
					cfg.max_string = (int)get_be(p, plen);
					break;
				default:
					break;
				}
			}
			p += plen;
		}
		buf += glen;
	}
	cfg.tx_win = (int)tx_win_;
	cfg.rx_win = (int)rx_win_;
	cfg.tx_info = (int)tx_info_;
	cfg.rx_info = (int)rx_info_;
	/* (a response: we sent the command) */
	if (response)
		cfg.comp = p0_swap(cfg.comp);
	link_->update_config(cfg);
}

/* queues an XID frame (addr: cmd_addr_ for the command, rsp_addr_ for
   our response): the HDLC optional functions, our sizes and windows,
   V.42bis (the command, or a response without V.44) and V.44 when the
   call has it; -1 if no frame is free or it does not fit */
int Lapm::tx_xid(uint8_t addr)
{
	const int size = 2 + 1 + (3 + 19) + (3 + 15) + 34;
	Frame *f = ctrl_frame();
	if (!f) {
		EC_ERR("tx_xid: cannot alloc frame.");
		return -1;
	}
	uint8_t *buf = f->buf;
	const Config &c = link_->cfg();
	int len = 0, glen;
	uint32_t v;
	/* P0 from the initiator's side in a command (see p0_swap()) */
	uint8_t p0 = (uint8_t)(addr == rsp_addr_ ? c.comp : p0_swap(c.comp));

	*buf++ = addr;
	*buf++ = U_XID;
	*buf++ = FI_GENERAL;
	len += 3;
	/* parameter negotiation */
	*buf++ = GI_PARAM_NEGOTIATION;
	glen = 19;
	*buf++ = (uint8_t)(glen >> 8);
	*buf++ = (uint8_t)glen;
	len += 3;
	*buf++ = PI_HDLC_OPTFUNCS;
	*buf++ = 3;
	*buf++ = 0x8a;
	*buf++ = 0x89;
	*buf++ = 0x00;
	*buf++ = PI_TX_INFO_MAXSIZE;
	v = tx_info_ << 3;
	*buf++ = 2;
	*buf++ = (uint8_t)(v >> 8);
	*buf++ = (uint8_t)v;
	*buf++ = PI_RX_INFO_MAXSIZE;
	v = rx_info_ << 3;
	*buf++ = 2;
	*buf++ = (uint8_t)(v >> 8);
	*buf++ = (uint8_t)v;
	*buf++ = PI_TX_WINDOW_SIZE;
	*buf++ = 1;
	*buf++ = (uint8_t)tx_win_;
	*buf++ = PI_RX_WINDOW_SIZE;
	*buf++ = 1;
	*buf++ = (uint8_t)rx_win_;
	len += glen;
	/* the command offers V.42bis and V.44; the response has what was
	   agreed */
	bool response = addr == rsp_addr_;
	bool v44 = c.comp && c.v44;
	/* V.42bis */
	if (c.comp && !(response && v44)) {
		*buf++ = GI_PRIVATE_NEGOTIATION;
		glen = 15;
		*buf++ = (uint8_t)(glen >> 8);
		*buf++ = (uint8_t)glen;
		len += 3;
		*buf++ = PI_PARAMETER_SET_ID;
		*buf++ = 3;
		*buf++ = 0x56;          /* "V42" */
		*buf++ = 0x34;
		*buf++ = 0x32;
		*buf++ = PI_V42BIS_REQUEST;
		*buf++ = 1;
		*buf++ = p0;
		*buf++ = PI_V42BIS_CW_NUMBER;
		*buf++ = 2;
		*buf++ = (uint8_t)(c.dict_size >> 8);
		*buf++ = (uint8_t)c.dict_size;
		*buf++ = PI_V42BIS_MAX_STRING;
		*buf++ = 1;
		*buf++ = (uint8_t)c.max_string;
		len += glen;
	}
	/* V.44, as the Agere modem has it */
	if (v44) {
		*buf++ = GI_USER_DATA;
		*buf++ = PI_V44_SET_ID;
		*buf++ = 3;
		*buf++ = 'V';
		*buf++ = '4';
		*buf++ = '4';
		*buf++ = PI_V44_C0;
		*buf++ = 1;
		*buf++ = 0;                     /* the stream method */
		*buf++ = PI_V44_P0;
		*buf++ = 1;
		*buf++ = p0;
		for (int k = 0; k < 2; k++) {
			*buf++ = (uint8_t)(PI_V44_N2T + k);
			*buf++ = 2;
			*buf++ = (uint8_t)(c.v44_codewords >> 8);
			*buf++ = (uint8_t)c.v44_codewords;
		}
		for (int k = 0; k < 2; k++) {
			*buf++ = (uint8_t)(PI_V44_N7T + k);
			*buf++ = 1;
			*buf++ = (uint8_t)c.v44_max_string;
		}
		for (int k = 0; k < 2; k++) {
			*buf++ = (uint8_t)(PI_V44_N8T + k);
			*buf++ = 2;
			*buf++ = (uint8_t)(c.v44_history >> 8);
			*buf++ = (uint8_t)c.v44_history;
		}
		len += 34;
	}
	if (len > size) {
		EC_ERR("tx_xid: result length(%d)  > size (%d).", len, size);
		return -1;
	}
	f->size = len;
	return 0;
}


/*
 *    timers
 *
 */

/* T401 from now, the retransmissions counted from 0 */
void Lapm::timer_start()
{
	rtx_count_ = 0;
	link_->set_timer(t401_bits(link_), Link::Timer::T401);
}

/* no T401 */
void Lapm::timer_stop()
{
	link_->set_timer(0, Link::Timer::None);
	rtx_count_ = 0;
}

/* T401 expired: our XID, SABME or DISC again, in the data phase an RR
   or RNR command with P = 1 (a poll); after N400 of them async (LAPM
   never came up), released, or DISC */
void Lapm::t401_timeout()
{
	if (rtx_count_ >= MAX_RTX) {
		rtx_count_ = 0;
		/* the originator's XID or SABME unanswered (N400 times)
		   before LAPM was ever up: a remote without V.42 (we may go
		   to LAPM without the detection phase), async then, not a
		   wait for nothing */
		if (!was_up_ && (config_ || state_ == ESTAB)) {
			link_->logf("no LAPM from the remote: async");
			config_ = false;
			state_ = IDLE;
			link_->set_timer(0, Link::Timer::None);
			link_->detected(false);
			return;
		}
		switch (state_) {
		case ESTAB:
		case DISC:
			state_ = IDLE;
			link_->status(Status::Released);
			break;
		case DATA:
			disconnect();
			break;
		default:
			break;
		}
		return;
	}
	rtx_count_++;
	if (config_)
		tx_xid(cmd_addr_);
	else {
		switch (state_) {
		case ESTAB:
			tx_unnum(cmd_addr_, U_SABME | 0x10);
			break;
		case DISC:
			tx_unnum(cmd_addr_, U_DISC | 0x10);
			break;
		case DATA:
			tx_super(cmd_addr_, busy_ ? S_RNR : S_RR, 1);
			break;
		default:
			break;
		}
	}
	link_->set_timer(t401_bits(link_), Link::Timer::T401);
}


/*
 *    information frames
 *
 */

/* is there an I frame to send (taking the DTE's characters for it)? */
int Lapm::tx_info()
{
	if (peer_busy_ || ((vs_ - va_) & 0x7f) >= tx_win_)
		return 0;
	if (tx_info_frame_ == info_list_) {
		if (info_list_->next == tx_info_frame_ ||
		    info_list_->next == sent_info_) {
			EC_ERR("tx_info: cannot alloc frame.");
			return 0;
		}
		Frame *f = info_list_;
		int n = link_->read_chars(f->buf + 3, (int)tx_info_);
		if (n < 0) {
			EC_ERR("tx_info: get chars error.");
			link_->status(Status::Error);
			return 0;
		}
		if (n == 0)
			return 0;
		f->size = n + 3;
		info_list_ = info_list_->next;
	}
	return 1;
}

/* go back to the oldest frame not acknowledged */
int Lapm::reject_info()
{
	uint8_t n = (vs_ - va_) & 0x7f;
	if (state_ != DATA)
		return 0;
	vs_ = va_;
	tx_info_frame_ = sent_info_;
	link_->stats().tx_again += n;
	return n;
}

/* N(R) = nr acknowledges our I frames up to nr - 1: they are free;
   T401 again for those still in flight (not while we poll); returns
   how many, -1 (and DISC) if more than a window is in flight */
int Lapm::ack_info(uint8_t nr)
{
	int n = 0;
	/* the frames in flight fit the window (slmodemd checked va <= nr
	   <= vs in a way that is always true, and so does not check it) */
	if (((vs_ - va_) & 0x7f) > (int)tx_win_) {
		EC_ERR("nr sequince error. disconnect!");
		disconnect();
		return -1;
	}
	while (va_ != nr) {
		if (sent_info_ == tx_info_frame_) {
			EC_ERR("ack info: VERY BAD: empty sent list: va %d, "
			       "vs %d", va_, vs_);
			break;
		}
		sent_info_ = sent_info_->next;
		va_ = (va_ + 1) & 0x7f;
		n++;
	}
	if (n > 0 && !rtx_count_) {
		timer_stop();
		if ((vs_ - va_) & 0x7f)
			timer_start();
	}
	return n;
}

/* the characters of an I frame the DTE did not take: to it again; once
   all are taken, RR ends our busy condition */
void Lapm::push_rest()
{
	if (state_ != DATA || !rx_count_)
		return;
	int ret = link_->write_chars(rx_buf_ + rx_head_, rx_count_);
	if (ret > 0) {
		rx_head_ += ret;
		rx_count_ -= ret;
		if (!rx_count_) {
			rx_head_ = 0;
			busy_ = false;
			tx_super(cmd_addr_, S_RR, 0);
			link_->push_pending = false;
		}
	}
}

/* an I frame in the data phase: its N(R) for our frames; in sequence,
   its characters to the DTE (what it does not take kept, and RNR),
   else one REJ until it is; RR or RNR unless an I frame of ours
   carries the acknowledgment; -1 out of sequence */
int Lapm::rx_info(Frame *f)
{
	int ret;

	ack_info((uint8_t)frame_nr(f));
	if (busy_) {
		if (i_pf(f))    /* 8.4.7 */
			tx_super(rsp_addr_, S_RNR, 1);
		return 0;
	}
	if (frame_ns(f) != vr_) {
		if (reject_)
			return -1;
		tx_super(rsp_addr_, S_REJ, i_pf(f));
		reject_ = true;
		return -1;
	}
	reject_ = false;
	ret = link_->write_chars(f->buf + 3, f->count - 3);
	if (ret < 0)
		ret = 0;
	if (ret != f->count - 3) {
		/* keep the rest, and be busy until the DTE takes it */
		rx_head_ = 0;
		rx_count_ = f->count - 3 - ret;
		memcpy(rx_buf_, f->buf + 3 + ret, rx_count_);
		busy_ = true;
		link_->push_pending = true;
	}
	vr_ = (vr_ + 1) & 0x7f;
	if (busy_)
		tx_super((uint8_t)frame_addr(f), S_RNR, i_pf(f));
	else if (peer_busy_ || i_pf(f) || !tx_info())
		tx_super((uint8_t)frame_addr(f), S_RR, i_pf(f));
	return 0;
}


/*
 *    supervisory frames
 *
 */

/* an RR, RNR or REJ command: the remote's busy condition, its N(R),
   REJ goes back to N(R); a poll (P = 1) gets RR or RNR with F = 1 */
void Lapm::rx_super_cmd(Frame *f)
{
	/* 8.4.7: while busy, RR, RNR and REJ with P = 1 get RNR */
	switch (frame_ctrl(f)) {
	case S_RR:
		peer_busy_ = false;
		ack_info((uint8_t)frame_nr(f));
		if (s_pf(f) || !tx_info())
			tx_super((uint8_t)frame_addr(f), busy_ ? S_RNR : S_RR,
				 s_pf(f));
		break;
	case S_RNR:
		peer_busy_ = true;
		ack_info((uint8_t)frame_nr(f));
		if (s_pf(f))
			tx_super(rsp_addr_, busy_ ? S_RNR : S_RR, s_pf(f));
		break;
	case S_REJ:
		peer_busy_ = false;
		ack_info((uint8_t)frame_nr(f));
		if (!rtx_count_) {
			timer_stop();
			reject_info();
		}
		if (s_pf(f) || !tx_info())
			tx_super((uint8_t)frame_addr(f), busy_ ? S_RNR : S_RR,
				 s_pf(f));
		break;
	case S_SREJ:
		EC_ERR("unsupported SREJ command!");
		break;
	default:
		EC_ERR("unknown s frame: %02x.", frame_ctrl(f));
		break;
	}
}

/* an RR, RNR or REJ response: its N(R); the final one (F = 1) to our
   poll stops T401 and sends again from N(R); an F = 1 we did not ask
   for is ignored */
void Lapm::rx_super_rsp(Frame *f)
{
	if (!rtx_count_ && s_pf(f)) {
		EC_ERR("rx_super: unsolisited final response!");
		return;
	}
	switch (frame_ctrl(f)) {
	case S_RR:
		peer_busy_ = false;
		ack_info((uint8_t)frame_nr(f));
		if (rtx_count_ && s_pf(f)) {
			reject_info();
			timer_stop();
		}
		break;
	case S_RNR:
		peer_busy_ = true;
		ack_info((uint8_t)frame_nr(f));
		if (rtx_count_ && s_pf(f)) {
			reject_info();
			timer_stop();
		}
		if (!rtx_count_)
			timer_start();
		break;
	case S_REJ:
		peer_busy_ = false;
		ack_info((uint8_t)frame_nr(f));
		if (!rtx_count_ || s_pf(f)) {
			reject_info();
			timer_stop();
		}
		break;
	case S_SREJ:
		EC_ERR("unsupported SREJ response!");
		break;
	default:
		EC_ERR("rx_s_rsp: unknown header %02x.", frame_ctrl(f));
		break;
	}
}


/*
 *    unnumbered frames
 *
 */

/* a U command: SABME (the data phase from the start, UA), DISC (UA and
   released, DM when idle), XID (the parameters, our response); UI and
   TEST are ignored */
void Lapm::rx_unnum_cmd(Frame *f)
{
	switch (frame_ctrl(f) & 0xef) {
	case U_SABME:
		reset();
		state_ = DATA;
		tx_unnum(rsp_addr_, (uint8_t)(U_UA | u_pf(f)));
		timer_stop();
		was_up_ = true;
		link_->status(Status::Up);
		break;
	case U_UI:              /* break: not supported */
		break;
	case U_DISC:
		if (state_ == IDLE)
			tx_unnum(rsp_addr_, U_DM | 0x10);
		else {
			state_ = IDLE;
			reset();
			tx_unnum(rsp_addr_, (uint8_t)(U_UA | u_pf(f)));
			link_->set_timer(0, Link::Timer::None);
			rtx_count_ = 0;
			link_->status(Status::Released);
		}
		break;
	case U_XID:
		rx_xid(f, false);
		tx_xid(rsp_addr_);
		break;
	case U_TEST:            /* optional */
		break;
	default:
		EC_ERR("rx unnum cmd: unknown frame 0x%02x", frame_ctrl(f));
		break;
	}
}

/* a U response to our SABME, DISC or XID: DM, UA, XID; UI and FRMR are
   ignored */
void Lapm::rx_unnum_rsp(Frame *f)
{
	switch (frame_ctrl(f) & 0xef) {
	case U_DM:
		switch (state_) {
		case ESTAB:
		case DISC:
			if (u_pf(f)) {
				state_ = IDLE;
				reset();
				timer_stop();
				link_->status(Status::Released);
			}
			break;
		case DATA:
			if (rtx_count_ || !u_pf(f)) {
				state_ = IDLE;
				reset();
				link_->status(Status::Released);
			}
			break;
		case IDLE:
			/* unsolicited, F = 0: the remote asks for the link
			   (8.8/V.42), an SABME of ours (our XID's answer no
			   longer awaited), unless it has been up in this
			   call (released: the call ends) */
			if (!u_pf(f) && !was_up_) {
				config_ = false;
				connect();
			}
			break;
		}
		break;
	case U_UI:
		break;
	case U_UA:
		switch (state_) {
		case ESTAB:
			state_ = DATA;
			reset();
			timer_stop();
			was_up_ = true;
			link_->status(Status::Up);
			break;
		case DISC:
			state_ = IDLE;
			reset();
			timer_stop();
			link_->status(Status::Released);
			break;
		default:        /* unsolicited */
			break;
		}
		break;
	case U_FRMR:
		break;
	case U_XID:
		if (config_) {
			rx_xid(f, true);
			config_ = false;
			timer_stop();
			if (state_ == IDLE)
				connect();
			else if (state_ == DATA) {
				busy_ = false;
				tx_super(cmd_addr_, S_RR, 0);
			}
		}
		break;
	default:
		EC_ERR("rx unnum rsp: unknown frame 0x%02x", frame_ctrl(f));
		break;
	}
}


/*
 *    the HDLC framer's side
 *
 */

/* the framer's next frame: a queued control frame first, else (in the
   data phase, the remote not busy, no XID going on) the next I frame,
   given N(S) = V(S) and N(R) = V(R), T401 started if it was not
   running; NULL: flags */
Frame *Lapm::next_tx_frame()
{
	Frame *f;
	if (tx_ctrl_ != ctrl_list_) {
		f = tx_ctrl_;
		tx_ctrl_ = tx_ctrl_->next;
		return f;
	}
	if (peer_busy_ || config_ || state_ != DATA)
		return nullptr;
	if (tx_info_frame_ == info_list_ && !tx_info())
		return nullptr;
	f = tx_info_frame_;
	tx_info_frame_ = tx_info_frame_->next;
	f->buf[0] = cmd_addr_;
	f->buf[1] = (uint8_t)(vs_ << 1);
	f->buf[2] = (uint8_t)(vr_ << 1);
	vs_ = (vs_ + 1) & 0x7f;
	if (!link_->timer())
		timer_start();
	return f;
}

/* nothing to do: I frames are free once acknowledged */
void Lapm::tx_complete(Frame *f)
{
	(void)f;
}

/* an I or S frame came: in ESTAB the remote took our SABME (its UA was
   lost) and the data phase starts, in DISC the link is released; true
   if the frame is for the data phase */
bool Lapm::valid_data_state()
{
	switch (state_) {
	case ESTAB:
		reset();
		state_ = DATA;
		was_up_ = true;
		link_->status(Status::Up);
		return true;
	case DATA:
		return true;
	case DISC:
		reset();
		state_ = IDLE;
		link_->status(Status::Released);
		return false;
	case IDLE:
	default:
		return false;
	}
}

/* a frame from the framer, by its type and its address (the remote's
   commands come with our response address) */
void Lapm::rx_complete(Frame *f)
{
	bool cmd = frame_addr(f) == rsp_addr_;
	/* I and S frames have two control octets, U frames one; an I frame
	   no more information than our rx_buf_ holds (N401) */
	if (f->count < 2 || (frame_type(f) != FRAME_U && f->count < 3)) {
		EC_ERR("frame too short.");
		return;
	}
	switch (frame_type(f)) {
	case FRAME_I:
		if (f->count - 3 > LAPM_MAX_INFO) {
			EC_ERR("information field too long.");
			break;
		}
		if (valid_data_state())
			rx_info(f);
		break;
	case FRAME_S:
		if (!valid_data_state())
			return;
		if (cmd)
			rx_super_cmd(f);
		else
			rx_super_rsp(f);
		break;
	case FRAME_U:
		if (cmd)
			rx_unnum_cmd(f);
		else
			rx_unnum_rsp(f);
		break;
	default:
		EC_ERR("unknown frame type.");
		break;
	}
}


/*
 *    the protocol's procedures
 *
 */

/* the originator sets the link up: SABME with P = 1, T401 */
void Lapm::connect()
{
	reset();
	state_ = ESTAB;
	tx_unnum(cmd_addr_, U_SABME | 0x10);
	timer_start();
}

/* the XID command with our parameters, T401; in the data phase RNR
   first, until the response */
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

/* we hang up: DISC with P = 1, T401 */
void Lapm::disconnect()
{
	state_ = DISC;
	tx_unnum(cmd_addr_, U_DISC | 0x10);
	timer_start();
}

/* the data phase from the start: no busy conditions, V(S) = V(A) =
   V(R) = 0, all I frames free */
void Lapm::reset()
{
	busy_ = false;
	peer_busy_ = false;
	vs_ = va_ = vr_ = 0;
	/* the I frames go */
	sent_info_ = info_list_;
	tx_info_frame_ = info_list_;
}

/* the caller sends XID 48 octets into the flags */
void Lapm::negotiate()
{
	link_->hdlc().enable_tx(true);
	configure();
}

/* the framer is ours: the answerer's frames may go out at once, the
   caller's after 48 octets of flags (negotiate(): its XID) */
void Lapm::start()
{
	link_->hdlc().set_framer(this, !link_->caller());
	if (link_->caller())
		link_->set_timer(48 * 8, Link::Timer::Negotiate);
	link_->push_pending = false;
}

/* a call: the address octets (the originator's commands 3, the
   answerer's 1), our windows and sizes, the rings of frames */
void Lapm::init(Link *link, int tx_win, int rx_win, int tx_info,
		int rx_info)
{
	link_ = link;
	state_ = IDLE;
	config_ = reject_ = busy_ = peer_busy_ = was_up_ = false;
	rtx_count_ = 0;
	rx_count_ = rx_head_ = 0;
	cmd_addr_ = link->caller() ? 0x3 : 0x1;
	rsp_addr_ = link->caller() ? 0x1 : 0x3;
	tx_win_ = (unsigned)tx_win;
	rx_win_ = (unsigned)rx_win;
	tx_info_ = (unsigned)tx_info;
	rx_info_ = (unsigned)rx_info;
	for (int i = 0; i < INFO_FRAMES; i++) {
		Frame *f = &info_buf_[i].frame;
		f->next = &info_buf_[(i + 1) % INFO_FRAMES].frame;
		f->buf = info_buf_[i].buf;
		f->size = (int)sizeof(info_buf_[i].buf);
		f->count = 0;
	}
	info_list_ = &info_buf_[0].frame;
	for (int i = 0; i < CTRL_FRAMES; i++) {
		Frame *f = &ctrl_buf_[i].frame;
		f->next = &ctrl_buf_[(i + 1) % CTRL_FRAMES].frame;
		f->buf = ctrl_buf_[i].buf;
		f->size = (int)sizeof(ctrl_buf_[i].buf);
		f->count = 0;
	}
	ctrl_list_ = &ctrl_buf_[0].frame;
	tx_ctrl_ = ctrl_list_;
	reset();
}

/* the end of the call: no frame lists */
void Lapm::exit()
{
	reset();
	info_list_ = nullptr;
	ctrl_list_ = nullptr;
	sent_info_ = tx_info_frame_ = nullptr;
}

} // namespace dlink
