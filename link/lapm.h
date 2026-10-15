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

#include <stdint.h>

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
	void init(Link *link, int tx_win, int rx_win, int tx_info,
		  int rx_info);
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
		uint8_t buf[4 + LAPM_MAX_INFO];
	};

	void reset();
	void connect();
	void configure();
	void timer_start();
	void timer_stop();
	Frame *ctrl_frame();
	int tx_unnum(uint8_t addr, uint8_t ctrl);
	int tx_super(uint8_t addr, uint8_t ctrl, int pf);
	int tx_xid(uint8_t addr);
	void rx_xid(Frame *f, bool response);
	int tx_info();
	int reject_info();
	int ack_info(uint8_t nr);
	int rx_info(Frame *f);
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
	uint8_t cmd_addr_ = 0, rsp_addr_ = 0;
	uint8_t vs_ = 0, va_ = 0, vr_ = 0;
	int rtx_count_ = 0;
	unsigned tx_win_ = 0, rx_win_ = 0, tx_info_ = 0, rx_info_ = 0;
	/* rings of frames: info frames from sent (not acknowledged) over
	   tx (not sent) to info_list (free); control frames from tx_ctrl
	   (not sent) to ctrl_list (free) */
	Frame *sent_info_ = nullptr, *tx_info_frame_ = nullptr;
	Frame *info_list_ = nullptr;
	Frame *tx_ctrl_ = nullptr, *ctrl_list_ = nullptr;
	/* received characters the DTE has not taken */
	int rx_count_ = 0, rx_head_ = 0;
	uint8_t rx_buf_[LAPM_MAX_INFO];
	Slot info_buf_[INFO_FRAMES];
	Slot ctrl_buf_[CTRL_FRAMES];
};

} // namespace dlink

#endif
