/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    lapm.h - V.42 LAPM: the error correcting protocol over HDLC frames
 *
 *    Modulo 128 sequence numbers, windows of up to 15 frames of up to
 *    128 octets, REJ recovery (no SREJ), parameter negotiation in XID
 *    (with V.42bis's private parameters), T401 counted in transmitted
 *    bits (2 s at the data pump's rate).
 *
 */

#ifndef DMODEM_LINK_LAPM_H
#define DMODEM_LINK_LAPM_H


#include "dm_types.h"
#include "hdlc.h"

namespace dlink {

enum {
	LAPM_MAX_WIN = 15,      /* the largest window we offer */
	LAPM_MAX_INFO = 128,    /* the largest information field */
	LAPM_DFLT_WIN = 15,     /* the defaults of V.42 */
	LAPM_DFLT_INFO = 128,
};

class Lapm : public Framer {
public:
	/* a call: our windows (frames) and information fields (octets) */
	void init(Link *link, s16 tx_win, s16 rx_win, s16 tx_info,
		  s16 rx_info);
	/* the end of the call: no frame lists */
	void exit();
	/* the link is up at the bit level */
	void start();
	/* we hang up: DISC */
	void disconnect();
	/* characters the DTE did not take: try again */
	void push_rest();
	/* I frames not sent yet or not acknowledged */
	bool tx_pending() const {
		return sent_info_ && info_list_ && sent_info_ != info_list_;
	}

	/* the HDLC framer's side */
	Frame *next_tx_frame() override;
	void tx_complete(Frame *f) override;
	void rx_complete(Frame *f) override;

	/* the bit timers (Link::Timer::T401, Negotiate) */
	void t401_timeout();
	void negotiate();

private:
	enum State { IDLE, ESTAB, DATA, DISC };
	enum {
		INFO_FRAMES = LAPM_MAX_WIN + 1,
		CTRL_FRAMES = 8,
		MAX_RTX = 5,            /* N400 */
	};
	struct Slot {
		Frame frame;
		u8 buf[4 + LAPM_MAX_INFO];
	};

	void reset();
	void connect();
	void configure();
	s32 t401_bits() const;
	void timer_start();
	void timer_stop();
	Frame *ctrl_frame();
	s16 tx_unnum(u8 addr, u8 ctrl);
	s16 tx_super(u8 addr, u8 ctrl, s16 pf);
	s16 tx_xid(u8 addr);
	void rx_xid(Frame *f, bool response);
	s16 tx_info();
	s16 reject_info();
	bool nr_ok(u8 nr);
	s16 ack_info(u8 nr);
	s16 rx_info(Frame *f);
	void rx_super_cmd(Frame *f);
	void rx_super_rsp(Frame *f);
	void rx_unnum_cmd(Frame *f);
	void rx_unnum_rsp(Frame *f);
	bool valid_data_state();
	void log_frame(const char *title, bool cmd, const Frame *f);

	Link *link_ = nullptr;
	State state_ = IDLE;
	bool config_ = false;   /* an XID exchange is going on */
	bool was_up_ = false;   /* LAPM has been up in this call */
	bool reject_ = false;
	bool busy_ = false;     /* we cannot take more */
	bool peer_busy_ = false;
	u8 cmd_addr_ = 0, rsp_addr_ = 0;
	u8 vs_ = 0, va_ = 0, vr_ = 0;
	u8 vs_hi_ = 0;                  /* the highest V(S) since V(A) */
	s16 rtx_count_ = 0;
	u16 tx_win_ = 0, rx_win_ = 0, tx_info_ = 0, rx_info_ = 0;
	/* rings of frames: info frames from sent (not acknowledged) over
	   tx (not sent) to info_list (free); control frames from tx_ctrl
	   (not sent) to ctrl_list (free) */
	Frame *sent_info_ = nullptr, *tx_info_frame_ = nullptr;
	Frame *info_list_ = nullptr;
	Frame *tx_ctrl_ = nullptr, *ctrl_list_ = nullptr;
	/* received characters the DTE has not taken */
	s16 rx_count_ = 0, rx_head_ = 0;
	u8 rx_buf_[LAPM_MAX_INFO];
	Slot info_buf_[INFO_FRAMES];
	Slot ctrl_buf_[CTRL_FRAMES];
};

} // namespace dlink

#endif
