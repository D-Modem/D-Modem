/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    lapb.h - the acknowledged link layers of ISDN's data calls on a
 *             B-channel: X.75 SLP (LAPB; T.90, the German ISDN BBSes
 *             and mailers, CAPI's B2 protocol 0) and V.120 (LAPD's
 *             frames with V.120's terminal adaption header; North
 *             American terminal adapters, CAPI's B2 protocol 9)
 *
 *    X.75: basic mode (modulo 8, SABM), a window of 7 frames, REJ
 *    recovery, T1 of 1 s counted in transmitted bits and N2 of 4 tries,
 *    as isdn4linux's X.75 had them.  The calling end commands with
 *    address B (0x01) and the called end with A (0x03), each answering
 *    at the other's; the calling end sends the SABM.  No XID: nothing is
 *    negotiated.
 *
 *    V.120: the same procedures in extended mode (modulo 128, SABME),
 *    with Q.921's two octet address: logical link 256 (V.120's
 *    default), the calling end on the user side (its commands C/R 0,
 *    the called end's 1).  Each I frame's information starts with V.120's
 *    terminal adaption header, one octet here: asynchronous mode, each
 *    frame a whole (B and F 1), 0x83; the far end's header and its
 *    extension octets are taken off.  Logical link 256 only, no UI
 *    frames, no XID.
 *
 */

#ifndef DMODEM_LINK_LAPB_H
#define DMODEM_LINK_LAPB_H


#include "dm_types.h"
#include "hdlc.h"

namespace dlink {

enum {
	LAPB_WIN = 7,           /* k */
	LAPB_TX_INFO = 256,     /* our I frames' largest information field */
	LAPB_RX_INFO = 2048,    /* the largest we take (N1: CAPI's and
				   isdn4linux's 2048) */
};

class Lapb : public Framer {
public:
	enum class Variant { X75, V120 };

	/* a call over link with X.75's or V.120's frames: disconnected,
	   nothing queued */
	void init(Link *link, Variant v);
	/* the link is up at the bit level: the calling end's SABM */
	void start();
	/* we hang up: DISC */
	void disconnect();
	/* characters the DTE did not take: try again */
	void push_rest();
	/* I frames not sent yet or not acknowledged */
	bool tx_pending() const { return sent_ != free_; }

	/* the HDLC framer's side */
	Frame *next_tx_frame() override;
	void tx_complete(Frame *f) override;
	void rx_complete(Frame *f) override;

	/* the bit timer (Link::Timer::T1) */
	void t1_timeout();

private:
	enum State { IDLE, ESTAB, DATA, DISC };
	enum {
		INFO_FRAMES = LAPB_WIN + 1,
		CTRL_FRAMES = 8,
		N2 = 4,
	};
	/* the largest address, control field and header */
	enum { HEAD_MAX = 2 + 2 + 1 };
	struct Slot {
		Frame frame;
		u8 buf[HEAD_MAX + LAPB_TX_INFO];
	};

	void reset();
	void connect();
	void establish();
	void release(const char *why);
	s32 t1_bits() const;
	void timer_start();
	void timer_stop();
	void ctrl(const u8 *addr, u8 c);
	void super(const u8 *addr, u8 fn, bool pf);
	/* n modulo 8 or 128 */
	u8 seq(s16 n) const { return static_cast<u8>(n & modulus_mask_); }
	/* b - a, modulo 8 or 128 */
	s16 seq_diff(u8 b, u8 a) const { return (b - a) & modulus_mask_; }
	bool parse(const Frame *f, u8 &fn, u8 &ns, u8 &nr, bool &pf) const;
	bool tx_info();
	bool nr_ok(u8 nr);
	void ack(u8 nr);
	void go_back();
	void rx_info(Frame *f, bool pf);
	void rx_super(Frame *f, bool cmd);
	void rx_unnum(Frame *f, bool cmd);

	Link *link_ = nullptr;
	Variant variant_ = Variant::X75;
	const char *tag_ = "x75";
	u8 modulus_mask_ = 7;           /* 7 basic, 127 extended mode */
	s16 alen_ = 1;                  /* the address's octets */
	s16 info_at_ = 2;               /* an I frame's information (ours) */
	u8 sabm_ = 0;                   /* SABM or SABME */
	State state_ = IDLE;
	bool reject_ = false;           /* a REJ of ours is outstanding */
	bool busy_ = false;             /* we cannot take more */
	bool peer_busy_ = false;
	bool heard_ = false;            /* a frame came from the remote */
	u8 cmd_addr_[2] = {}, rsp_addr_[2] = {};
	u8 vs_ = 0, va_ = 0, vr_ = 0;
	s16 tries_ = 0;
	/* I frames: from sent_ (unacknowledged) over next_ (to send) to
	   free_, ring order; the control frames' ring from ctrl_tx_ to
	   ctrl_free_ */
	s16 sent_ = 0, next_ = 0, free_ = 0;
	s16 ctrl_tx_ = 0, ctrl_free_ = 0;
	Slot info_[INFO_FRAMES];
	Frame ctrl_frame_[CTRL_FRAMES];
	u8 ctrl_buf_[CTRL_FRAMES][4];
	/* received characters the DTE has not taken */
	s16 rx_count_ = 0, rx_head_ = 0;
	u8 rx_buf_[LAPB_RX_INFO];
};

} // namespace dlink

#endif
