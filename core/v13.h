/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v13.h - V.13's simulated carrier control, at the bit level between
 *            the link and the data pump (S48=1)
 *
 *    On a line whose carrier never goes off (a 4-wire leased line's
 *    continuous carrier), the DTE's circuit 105 switches the far end's
 *    circuit 109 in band: while 105 is OFF the DCE sends the idle
 *    pattern, scrambled 1s (1 + x^-3 + x^-7, never the all-1s state:
 *    note 6), in place of data; when it turns ON, 8 scrambled 0s (the ON
 *    pattern, its scrambler going on from the idle pattern's: note 1),
 *    then the data.  The far end turns 109 OFF after 56 bits of the idle
 *    pattern (48 to 64: note 3) and ON after the ON pattern, and holds
 *    104 at 1 while 109 is OFF.  Its link gets the bits 64 bits late, so
 *    that none of the idle pattern before 109 goes OFF reaches the DTE
 *    (which note 3 allows).
 *
 *    A pty has no RTS: here 105 is ON while the DTE's data comes (the
 *    link's first 0, a start bit, turns it ON; the bits wait the ON
 *    pattern's 8 out) and OFF after 64 bits of the link's idle 1s (at
 *    least 128 bits of the idle pattern before it turns ON again: note
 *    5, the data waiting meanwhile).  A 109 left OFF for 2048 bits with
 *    no idle pattern comes ON by itself (note 8).
 */
#ifndef DMODEM_CORE_V13_H
#define DMODEM_CORE_V13_H

#include <deque>

#include <modem_defs.h>

namespace dcore {
namespace v13 {

/* 1 + x^-3 + x^-7 */
struct Scrambler {
	u8 s = 0x2a;
	/* the bit scrambled */
	u8 bit(u8 in)
	{
		u8 o = (in ^ (s >> 2) ^ (s >> 6)) & 1;
		s = static_cast<u8>(((s << 1) | o) & 0x7f);
		return o;
	}
};

struct Descrambler {
	u8 s = 0;
	/* the bit descrambled */
	u8 bit(u8 in)
	{
		u8 o = (in ^ (s >> 2) ^ (s >> 6)) & 1;
		s = static_cast<u8>(((s << 1) | in) & 0x7f);
		return o;
	}
};

class SimCarrier {
public:
	/* a call: 105 OFF here, 109 ON there until the idle pattern */
	void reset() { *this = SimCarrier(); }
	/* the link's bit for the line: the pattern's, or the data's */
	u8 tx(u8 link_bit);
	/* a bit from the line: what goes to the link (1 while 109 is
	   OFF) */
	u8 rx(u8 b);
	/* circuit 105 here (we send data) */
	bool c105() const { return on_; }
	/* circuit 109 from the far end's */
	bool c109() const { return c109_; }
	/* 109 changed since the last call */
	bool c109_changed()
	{
		bool c = c109_ != c109_told_;
		c109_told_ = c109_;
		return c;
	}

	enum {
		ON_BITS = 8, OFF_IDLE = 64, MIN_IDLE = 128, DETECT_IDLE = 56,
		DELAY = 64,
		LOST_IDLE = 2048,
	};

private:
	/* tx: 105, the ON pattern's bits to go, the data waiting, the
	   link's 1s in a row, the idle pattern's bits since 105 went
	   OFF */
	bool on_ = false;
	s16 on_left_ = 0;
	std::deque<u8> wait_;
	s32 ones_ = 0, idle_sent_ = MIN_IDLE;
	Scrambler scr_;
	/* rx: 109, its idle pattern's bits in a row, the ON pattern's,
	   the bits since the idle pattern was last there */
	bool c109_ = true, c109_told_ = true;
	s32 idle_ = 0, zeros_ = 0, no_idle_ = 0;
	Descrambler d_;
	/* the bits to the link DELAY bits late (the idle pattern's run, and
	   the 7 bits before its descrambler follows it), so that the idle
	   pattern before 109 goes OFF does not reach it */
	std::deque<u8> delay_ = std::deque<u8>(DELAY, 1);
};

}  // namespace v13
}  // namespace dcore

#endif
