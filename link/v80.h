/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v80.h - V.80 Synchronous Access Mode (8.8/V.80): the DTE's
 *            start-stop octets, with in-band commands after an EM
 *            (19h), as a synchronous bit stream on the line
 *
 *    Two sub-modes, which the DTE switches between: transparent (the
 *    DTE's octets go out as they are, least significant bit first, and
 *    the line's bits come back as octets), and framed (HDLC frames:
 *    the DTE marks each frame's end with <EM><flag>; the DCE adds the
 *    zero insertion, the flags and, if asked, the FCS, and hands the
 *    received frames' octets back with <EM><flag> after a good one and
 *    <EM><err> after a bad one).  A call starts transparent.
 *
 *    The DTE's EM, 99h (EM with bit 7 set) and, with DC1/DC3 flow
 *    control, DC1 and DC3 are sent shielded: <EM><t1> and so on
 *    (Table 9/V.80).  The commands the DCE acts on itself (<EM><ecs>,
 *    <EM><eot>, <EM><rate>, <EM><rtn>, <EM><rrn>) go to the link's
 *    owner.
 *
 */

#ifndef DMODEM_LINK_V80_H
#define DMODEM_LINK_V80_H

#include "dm_types.h"
#include "hdlc.h"

namespace dlink {

/* the in-band codes (Table 9/V.80): after EM in either direction */
namespace v80 {
const u8 EM = 0x19, EM8 = 0x99;         /* the escape, either form */
const u8 DC1 = 0x11, DC3 = 0x13;
/* one shielded octet: 19h, 99h, DC1, DC3 */
const u8 T1 = 0x5c, T2 = 0x76, T3 = 0xa0, T4 = 0xa1;
/* two: 19h 19h, 99h 99h, DC1 DC1, DC3 DC3 */
const u8 T5 = 0x5d, T6 = 0x77, T7 = 0xa2, T8 = 0xa3;
/* the other pairs, T9 (19h 99h) to T20 (DC3 DC1): A4h to AFh */
const u8 T9 = 0xa4, T20 = 0xaf;
const u8 MARK = 0xb0;   /* DTE: transparent; DCE: an abort (7 ones) */
const u8 FLAG = 0xb1;   /* DTE: a flag (framed); DCE: a good frame */
const u8 ERR = 0xb2;    /* DTE: an abort; DCE: a bad frame, or a flag
			   after the hunt */
const u8 HUNT = 0xb3;   /* DTE: the receiver hunts */
const u8 UNDER = 0xb4;  /* DCE: transmit underrun */
const u8 TOVER = 0xb5;  /* DCE: transmit overrun */
const u8 ROVER = 0xb6;  /* DCE: receive overrun */
const u8 RESUME = 0xb7; /* DTE: after an underrun (framed_un_ov 1) */
const u8 BNUM = 0xb8;   /* DCE: octets in the transmit buffer (2 more) */
const u8 UNUM = 0xb9;   /* DCE: octets discarded (2 more) */
const u8 EOT = 0xba;    /* DTE: end the carrier; DCE: the carrier is gone */
const u8 ECS = 0xbb;    /* DTE: online command state; DCE: confirmed */
const u8 RRN = 0xbc;    /* rate renegotiation, asked or seen */
const u8 RTN = 0xbd;    /* retrain, asked or seen */
const u8 RATE = 0xbe;   /* DTE: max tx, rx rates; DCE: the rates (2 more) */

/* the <tx> <rx> code of a rate (Table 10/V.80): 20h 1200 bit/s, 21h
   2400, 22h 4800 and up in steps of 2400 to 2Eh 33600 (the codes stop
   there: faster rates are 2Eh) */
u8 rate_code(s32 rate);
/* the rate of a code; 0 for none */
s32 code_rate(u8 code);
} // namespace v80

/* +ESA and +ITF (8.2, 8.4/V.80), and what +IFC means for it */
struct SyncConfig {
	s16 framed_idle = 0;    /* framed, between frames: 0 flags, 1 marks */
	s16 framed_un_ov = 0;   /* an underrun in a frame: 0 an abort; 1 a
				   flag, then the DTE's octets dropped
				   until <EM><resume> */
	s16 crc = 0;            /* framed: 0 no FCS, 1 FCS-16, 2 FCS-32 */
	u8 syn1 = 0xff;         /* transparent: the octet sent on underrun */
	s16 report_period = 0;  /* <EM><bnum> every this many 10 ms, 0 none */
	bool shield_dc = false; /* DC1 and DC3 to the DTE shielded (it has
				   DC1/DC3 flow control from us) */
	bool dc_flow = false;   /* DC1 and DC3 from the DTE are its flow
				   control, not data */
};

/* what the DTE asks of the DCE in band */
enum class SyncCommand {
	Ecs,            /* online command state */
	Eot,            /* end the carrier */
	Rate,           /* the highest rates (two codes) */
	Retrain,
	Reneg,
};

class SyncAccess : public BitFramer<SyncAccess> {
public:
	/* the call is in Synchronous Access Mode: transparent, an
	   underrun first */
	void start(Link *link, const SyncConfig &cfg);
	/* bits went out: <EM><bnum> when its period has passed */
	void clock(s32 bits);
	/* an indication to the DTE: <EM><code> and n more octets */
	void indicate(u8 code, const u8 *par = nullptr, s16 n = 0);
	/* what waits for the DTE, as much as it takes */
	void flush();
	/* the framed sub-mode (rather than transparent) */
	bool framed() const { return framed_; }

private:
	friend class BitFramer<SyncAccess>;
	bool tx_fill();
	void rx_scan();

	enum class In { None, Data, Done };
	In next_input(u8 &octet);
	void command(u8 code);
	void data(u8 octet);
	void idle();
	void push(u8 octet, bool raw);
	void emit();
	void enter_framed();
	void enter_transparent();
	void rx_bit(s16 b);
	void rx_octet(u8 octet);
	void rx_flag();
	void rx_abort();
	void rx_reset(bool hunt);
	void put(u8 octet);
	void put_raw(const u8 *p, s16 n);
	static void encode14(u8 *par, s32 v);

	SyncConfig cfg_;
	bool framed_ = false;
	/* the DTE's input: after EM, after <EM><rate> (the codes so far),
	   the second octet of a pair */
	s16 esc_ = 0;
	u8 rate_[2] = {};
	s16 pair_n_ = 0;
	u8 pair_ = 0;
	/* the octets for the line, oldest first: each with zero insertion
	   or as it is (raw) */
	enum { PEND = 8 };
	u8 pend_[PEND] = {};
	bool pend_raw_[PEND] = {};
	s16 pend_head_ = 0, pend_n_ = 0;
	/* transmit: the underrun reported, a frame begun (its FCS so far),
	   a flag due before the next octet, the 1s in a row, the DTE's
	   octets dropped since the underrun (framed_un_ov 1) */
	bool underrun_ = false;
	bool in_frame_ = false;
	u32 tx_fcs_ = 0;
	bool need_flag_ = false;
	s16 tx_ones_ = 0;
	bool discard_ = false;
	s32 discarded_ = 0;
	/* <EM><bnum>: bits until the next */
	s32 report_left_ = 0;
	/* receive (framed): hunting for a flag, the 1s in a row, the
	   octet so far, the bits since the flag, the octets of the frame
	   and its FCS so far, the FCS's octets held back */
	bool hunt_ = true;
	s16 rx_ones_ = 0;
	u8 rx_octet_ = 0;
	s16 rx_n_ = 0;
	s32 rx_bits_ = 0;
	s32 rx_count_ = 0;
	u32 rx_fcs_ = 0;
	u8 held_[4] = {};
	s16 held_n_ = 0;
	/* to the DTE: a ring of octets, an overrun to report */
	u8 out_[2048];
	s16 out_head_ = 0, out_count_ = 0;
	bool rover_ = false;
};

} // namespace dlink

#endif
