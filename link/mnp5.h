/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    mnp5.h - MNP 5 data compression, one direction (an encoder or a
 *             decoder), as the PCTel, Agere and other soft modems have
 *             it (MNP's LR parameter 9; it is not in V.42)
 *
 *    Run-length coding: after three equal characters a count of the
 *    ones after them (0 to 250) follows.  Adaptive frequency coding:
 *    the characters are ranked by how often they came (the one that
 *    passes the count of the one above moves up; all counts are halved
 *    when the top one reaches 255) and each is sent as its rank's code,
 *    a 3-bit header h and a body of h bits (1 for h = 0): ranks 0 and 1
 *    000x, 2 and 3 001x, 4 to 7 010xx, ..., 128 to 255 111xxxxxxx; rank
 *    255 has a 0 after it, and eleven 1s are the flush, which the rest
 *    of its octet (1s) follows.  A count is coded as the rank of its
 *    value, outside the ranking.  The codes fill each octet from its
 *    most significant bit, and the stream goes on from one LT frame to
 *    the next.
 *
 */

#ifndef DMODEM_LINK_MNP5_H
#define DMODEM_LINK_MNP5_H


#include "dm_types.h"

namespace dlink {

class Mnp5 {
public:
	/* a new stream: the ranks in character order, no run, no bits */
	void init();
	/* (no tables to allocate) */
	bool ready() const { return true; }

	/* one character in; then out() gives what it produced */
	s16 encode(u8 ch);
	s16 decode(u8 ch);
	/* the encoder: the count held, the flush and the octet's rest (only
	   after characters since the last one); the decoder: nothing (the
	   stream goes on over the frames) */
	s16 flush_encoder();
	/* (see flush_encoder()) */
	s16 flush_decoder() { return 0; }
	/* the output so far, up to n octets */
	s16 out(u8 *buf, s16 n);

private:
	void rank_up(s16 r);
	void put(u32 bits, s16 len);
	void put_value(s16 v);
	void put_char(u8 ch);
	void out_char(u8 ch, s16 n);
	void token(s16 v);

	/* the characters by rank, their ranks, the counts by rank */
	u8 char_[256], rank_[256], count_[256];
	/* the run: none yet (first_), its character, how many */
	bool first_ = true;
	u8 run_ch_ = 0;
	s16 run_ = 0;
	/* the encoder's bits not out yet (the last bits_ of acc_), coded
	   since the last flush; the decoder's bits not parsed yet */
	u32 acc_ = 0;
	s16 bits_ = 0;
	bool dirty_ = false;
	/* output */
	s16 olen_ = 0, ohead_ = 0;
	u8 obuf_[1024];
};

} // namespace dlink

#endif
