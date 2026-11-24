/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hayes_adc.h - Hayes ADC (Adaptive Data Compression), one direction
 *                  (an encoder or a decoder), as the Hayes V-series
 *                  Smartmodem 9600 (firmware 1.4, ROM bank 10) has it
 *
 *    ADC is an adaptive order-0 rank code in 4-bit units (nibbles),
 *    with a run-length rule (docs/hayes-details.md):
 *
 *    - The model keeps a rank for each of the 256 octet values and a
 *      count for each rank.  A coded character moves up past the first
 *      of the 16 ranks above it that has a smaller count.
 *    - A rank r is sent as 1, 2 or 3 nibbles: r < t1 one nibble r;
 *      t1 <= r <= t2 the octet (r + off) mod 256; r > t2 the nibble 15,
 *      then the octet r.  Each 128 coded characters, t1, t2 and off are
 *      calculated again from the counts, and the counts are halved.
 *    - After three equal characters, the next equal ones are only
 *      counted.  The count goes out as one nibble (15: one more follows).
 *    - Two nibbles make one octet, high nibble first.  The octet FAh is
 *      sent as FAh 03h.  The end of a frame is on an octet boundary:
 *      a pad nibble 15 is followed by FAh FFh.  FAh 00h or FAh 01h sets
 *      the decoder's model to its initial values.
 *
 *    The model goes on from frame to frame for the whole connection.
 *    The firmware's addresses are in the comments of hayes_adc.cpp.
 *
 */

#ifndef DMODEM_LINK_HAYES_ADC_H
#define DMODEM_LINK_HAYES_ADC_H


#include "dm_types.h"

namespace dlink {

enum {
	/* the most octets that one encode() can add to the output (a
	   count nibble, an escape of 3 nibbles, FAh doubled) */
	ADC_MAX_PER_CHAR = 4,
	/* the most octets that flush_encoder() can add */
	ADC_MAX_FLUSH = 3,
};

class HayesAdc {
public:
	/* a new stream: the initial model, no nibble that waits, no output */
	void init();
	/* (no tables to allocate) */
	bool ready() const { return true; }

	/* the encoder: one character from the DTE; then out() gives the
	   octets it made */
	s16 encode(u8 ch);
	/* the encoder: the end of a frame, on an octet boundary */
	s16 flush_encoder();
	/* the decoder: one octet of an I frame's information field */
	s16 decode(u8 ch);
	/* the decoder: the end of the frame (an FAh that is the last octet
	   of the frame is dropped) */
	s16 flush_decoder();
	/* the output so far, up to n octets; an FAh with its second octet
	   still in the output is not given without that octet */
	s16 out(u8 *buf, s16 n);
	/* the octets in the output that out() did not give yet */
	s16 pending() const { return olen_; }

private:
	void model_init();
	void recalc();
	void update(u8 c);
	void put_octet(u8 b);
	void put_raw(u8 b);
	void nibble(u8 n);
	void code(u8 c);
	void out_char(u8 c);
	void dec_nibble(u8 n);

	/* the model: count[] by rank (E000h), rank[] by character (E100h),
	   char[] by rank (E200h) */
	u8 count_[256], rank_[256], char_[256];
	/* characters until the next recalculation (FC0Eh), the thresholds
	   (FC0Fh, FC10h, FC11h), the last coded character (FC12h), the run
	   counter (FC13h, signed) */
	s16 tick_ = 0x80;
	u8 t1_ = 4, t2_ = 0xbf, off_ = 0x3c;
	u8 prev_ = 0;
	s16 run_ = -2;
	/* the encoder's high nibble that waits for a low nibble (FC14h) */
	bool half_ = false;
	u8 hi_ = 0;
	/* the decoder: 0 first nibble, 1 second, 2 after the escape
	   (FC05h); an FAh received */
	s16 state_ = 0;
	bool esc_ = false;
	/* this one encodes (out() then keeps FAh with its second octet) */
	bool encoder_ = false;
	/* output, a ring */
	s16 olen_ = 0, ohead_ = 0;
	u8 obuf_[512];
};

} // namespace dlink

#endif
