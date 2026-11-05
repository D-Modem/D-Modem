/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hst_link.h - USRobotics HST's own link layer, the one a Courier
 *                 runs after the switch to HST (docs/dsp/hst-details.md):
 *                 HDLC frames without an address, the first octet the
 *                 frame's type
 *
 *    Read from two emulated Couriers calling each other (the protocol
 *    was never published), the frames are:
 *
 *        04 ns <data>          data (64 octets at first, up to 224)
 *        08 ns nr 08 <data>    data with an acknowledgement
 *        05 nr 08              acknowledgement (RR)
 *        03 nr 08              reject: from nr + 1 again
 *        06 k 04               the back channel's side asks to turn
 *                              the channels around (k counts its asks)
 *        06 k 3x               the forward channel's receiver asks for
 *                              rate x (ASL)
 *        07 k                  ask k granted
 *
 *        s<<4|8|l <data>       short data: s the low 4 bits of
 *                              N(S), l (1 to 7) octets of data
 *
 *    Each side numbers its data frames from 1, modulo 256, and nr is
 *    the last of the other's received in order (0: none yet).  Both
 *    start with an RR of 0, and send one again while idle.  The side on
 *    the back channel with data to send asks to turn the channels
 *    around; the forward channel's side grants it once its frames are
 *    out (here: and its DTE has nothing more, or it has sent 32 since
 *    it got the channel), and both pumps turn (Link::hst_turn()): the
 *    one that granted as soon as its grant is on the line, the other as
 *    it receives it.  Rate changes (ASL) are neither asked for nor
 *    granted.  Go back N with a window of 8; T1 in the transmitter's
 *    bits, as Lapb has it.  When nothing comes after our grant turned
 *    the channels, the remote missed it: we turn them back.
 *
 */

#ifndef DMODEM_LINK_HST_LINK_H
#define DMODEM_LINK_HST_LINK_H


#include "backlog.h"
#include "dm_types.h"
#include "hdlc.h"

namespace dlink {

enum {
	HST_WIN = 8,            /* data frames outstanding */
	HST_INFO = 64,          /* our data frames' largest information */
	HST_RX_INFO = 256,      /* the largest we take (a Courier's grow
				   to 224 once its frames are acknowledged) */
};

class HstLink : public Framer, public LinkProto {
public:
	/* a call over link: nothing heard or queued */
	void init(Link *link);
	/* HST's pump has connected: our first RR; Up once the remote's
	   first frame comes */
	void start();
	/* we hang up: HST sends nothing for it; Released soon after */
	void disconnect() override;
	/* characters the DTE did not take: try again */
	void push_rest() override;
	/* data frames not sent yet or not acknowledged */
	bool tx_pending() const override { return sent_ != free_; }

	/* the HDLC framer's side */
	Frame *next_tx_frame() override;
	void tx_complete(Frame *f) override;
	void rx_complete(Frame *f) override;

	/* the bit timer (Link::Timer::Hst) */
	void timeout();

private:
	enum State { IDLE, WAIT, DATA, DISC };
	enum {
		INFO_FRAMES = HST_WIN + 1,
		CTRL_FRAMES = 8,
		N2 = 10,                /* T1s with no answer */
	};
	struct Slot {
		Frame frame;
		u8 buf[4 + HST_INFO];
	};

	void ctrl(u8 type, u8 a, u8 b, s16 n);
	bool tx_info();
	s32 rate() const;
	s32 t1_bits() const;
	s32 idle_bits() const;
	void timer_restart();
	void ack(u8 nr);
	void go_back();
	void rx_info(const u8 *d, s16 n, u8 ns);
	void up();
	void release(const char *why);
	bool granting() const;
	bool grant_ready() const;
	void turn();
	/* b - a, modulo 256 */
	static s16 diff(u8 b, u8 a) { return static_cast<u8>(b - a); }

	Link *link_ = nullptr;
	State state_ = IDLE;
	/* N(S) of our next new frame, of our oldest unacknowledged one,
	   and the remote's last received in order */
	u8 vs_ = 1, va_ = 1, vr_ = 0;
	bool ack_due_ = false;          /* an RR to send */
	bool reject_ = false;           /* our REJ is outstanding */
	bool asks_logged_ = false;
	/* the channels: we send the forward one; our ask to turn them is
	   out (its count ask_k_); the remote's ask grant_k_ (-1 none) and
	   our grant of it on its way (then we turn: turn_wait_); our data
	   frames sent since we got the forward channel */
	bool fwd_ = false;
	bool asked_ = false;
	u8 ask_k_ = 0;
	s16 grant_k_ = -1;
	const Frame *grant_frame_ = nullptr;
	bool turn_wait_ = false;
	s16 fwd_frames_ = 0;
	/* our last turn was our grant's; a frame heard since the last turn;
	   the timer's runs since then without one */
	bool granted_ = false, heard_ = false;
	s16 quiet_ = 0;
	s16 tries_ = 0;
	/* data frames: from sent_ (unacknowledged) over next_ (to send) to
	   free_, ring order; control frames from ctrl_tx_ to ctrl_free_ */
	s16 sent_ = 0, next_ = 0, free_ = 0;
	s16 ctrl_tx_ = 0, ctrl_free_ = 0;
	Slot info_[INFO_FRAMES];
	Frame ctrl_frame_[CTRL_FRAMES];
	u8 ctrl_buf_[CTRL_FRAMES][3];
	/* received characters the DTE has not taken */
	DteBacklog<HST_RX_INFO> backlog_;
};

} // namespace dlink

#endif
