/*
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
};

static int frame_type(const Frame *f)
{
	return f->buf[1] & 1 ? f->buf[1] & 3 : FRAME_I;
}
static int frame_ctrl(const Frame *f) { return f->buf[1]; }
static int frame_addr(const Frame *f) { return f->buf[0]; }
static int i_pf(const Frame *f) { return f->buf[2] & 1; }
static int s_pf(const Frame *f) { return f->buf[2] & 1; }
static int u_pf(const Frame *f) { return f->buf[1] & 0x10; }
static int frame_ns(const Frame *f) { return f->buf[1] >> 1; }
static int frame_nr(const Frame *f) { return f->buf[2] >> 1; }

static uint32_t get_le(const uint8_t *buf, int len)
{
	uint32_t v = 0;
	while (len--) {
		v <<= 8;
		v |= buf[len];
	}
	return v;
}

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

#define EC_ERR(...) link_->logf("err: ec: " __VA_ARGS__)


/*
 *    frames
 *
 */

Frame *Lapm::ctrl_frame()
{
	if (ctrl_list_->next == tx_ctrl_)
		return nullptr;
	Frame *f = ctrl_list_;
	ctrl_list_ = ctrl_list_->next;
	return f;
}

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

void Lapm::rx_xid(Frame *f)
{
	/* what the remote asks for; V.42bis off unless it asks */
	Config cfg;
	cfg.ec = true;
	cfg.detect = true;
	cfg.comp = 0;
	cfg.dict_size = 512;
	cfg.max_string = 6;
	const uint8_t *buf = f->buf + 2;
	int len = f->count - 2;

	if (*buf != FI_GENERAL)
		return;
	buf++;
	len--;
	while (len > 0) {
		uint8_t gid = buf[0];
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
	link_->update_config(cfg);
}

int Lapm::tx_xid(uint8_t addr)
{
	const int size = 2 + 1 + (3 + 19) + (3 + 15);
	Frame *f = ctrl_frame();
	if (!f) {
		EC_ERR("tx_xid: cannot alloc frame.");
		return -1;
	}
	uint8_t *buf = f->buf;
	const Config &c = link_->cfg();
	int len = 0, glen;
	uint32_t v;

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
	/* V.42bis */
	if (c.comp) {
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
		*buf++ = (uint8_t)c.comp;
		*buf++ = PI_V42BIS_CW_NUMBER;
		*buf++ = 2;
		*buf++ = (uint8_t)(c.dict_size >> 8);
		*buf++ = (uint8_t)c.dict_size;
		*buf++ = PI_V42BIS_MAX_STRING;
		*buf++ = 1;
		*buf++ = (uint8_t)c.max_string;
		len += glen;
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

void Lapm::timer_start()
{
	rtx_count_ = 0;
	link_->set_timer(t401_bits(link_), t401_timeout);
}

void Lapm::timer_stop()
{
	link_->set_timer(0, nullptr);
	rtx_count_ = 0;
}

void Lapm::t401_timeout(Link *link)
{
	Lapm *l = &link->lapm_;
	if (l->rtx_count_ > MAX_RTX) {
		l->rtx_count_ = 0;
		switch (l->state_) {
		case ESTAB:
		case DISC:
			l->state_ = IDLE;
			link->status(Status::Released);
			break;
		case DATA:
			l->disconnect();
			break;
		default:
			break;
		}
		return;
	}
	l->rtx_count_++;
	if (l->config_)
		l->tx_xid(l->cmd_addr_);
	else {
		switch (l->state_) {
		case ESTAB:
			l->tx_unnum(l->cmd_addr_, U_SABME | 0x10);
			break;
		case DISC:
			l->tx_unnum(l->cmd_addr_, U_DISC | 0x10);
			break;
		case DATA:
			l->tx_super(l->cmd_addr_, l->busy_ ? S_RNR : S_RR, 1);
			break;
		default:
			break;
		}
	}
	link->set_timer(t401_bits(link), t401_timeout);
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
	return n;
}

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

void Lapm::rx_unnum_cmd(Frame *f)
{
	switch (frame_ctrl(f) & 0xef) {
	case U_SABME:
		reset();
		state_ = DATA;
		tx_unnum(rsp_addr_, (uint8_t)(U_UA | u_pf(f)));
		timer_stop();
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
			link_->set_timer(0, nullptr);
			rtx_count_ = 0;
			link_->status(Status::Released);
		}
		break;
	case U_XID:
		rx_xid(f);
		tx_xid(rsp_addr_);
		break;
	case U_TEST:            /* optional */
		break;
	default:
		EC_ERR("rx unnum cmd: unknown frame 0x%02x", frame_ctrl(f));
		break;
	}
}

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
			if (!u_pf(f))
				link_->status(Status::Up);
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
			rx_xid(f);
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

void Lapm::tx_complete(Frame *f)
{
	(void)f;
}

bool Lapm::valid_data_state()
{
	switch (state_) {
	case ESTAB:
		reset();
		state_ = DATA;
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

void Lapm::rx_complete(Frame *f)
{
	bool cmd = frame_addr(f) == rsp_addr_;
	switch (frame_type(f)) {
	case FRAME_I:
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

void Lapm::connect()
{
	reset();
	state_ = ESTAB;
	tx_unnum(cmd_addr_, U_SABME | 0x10);
	timer_start();
}

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

void Lapm::disconnect()
{
	state_ = DISC;
	tx_unnum(cmd_addr_, U_DISC | 0x10);
	timer_start();
}

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
void Lapm::negotiate(Link *link)
{
	link->hdlc_.enable_tx(true);
	link->lapm_.configure();
}

void Lapm::start()
{
	link_->hdlc_.set_framer(this, !link_->caller());
	if (link_->caller())
		link_->set_timer(48 * 8, negotiate);
	link_->push_pending = false;
}

void Lapm::init(Link *link, int tx_win, int rx_win, int tx_info,
		int rx_info)
{
	link_ = link;
	state_ = IDLE;
	config_ = reject_ = busy_ = peer_busy_ = false;
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

void Lapm::exit()
{
	reset();
	info_list_ = nullptr;
	ctrl_list_ = nullptr;
	sent_info_ = tx_info_frame_ = nullptr;
}

} // namespace dlink
