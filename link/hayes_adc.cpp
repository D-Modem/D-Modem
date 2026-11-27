/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hayes_adc.cpp - Hayes ADC (see hayes_adc.h)
 *
 *    Each function names the firmware routine that it copies (V-series
 *    Smartmodem 9600, firmware 1.4, ROM bank 10).  The normative model
 *    is analysis/tools/adc_ref.py of the firmware analysis, which was
 *    compared with the firmware in a Z80 emulator; link/test/hayes_test
 *    has the firmware's own output as test vectors.
 *
 */

#include "hayes_adc.h"

namespace dlink {

/* the octet that starts a two-octet code in the output */
static const u8 ADC_ESC = 0xfa;
/* the second octets after it: a data octet FAh, the end of a frame
   (the pad nibble before it is dropped) */
static const u8 ADC_ESC_DATA = 0x03, ADC_ESC_END = 0xff;
/* the characters between two recalculations of the thresholds */
static const s16 ADC_TICK = 0x80;

/* a new stream: the initial model (7B2Ah), no nibble that waits, the
   decoder at its first nibble, no output */
void HayesAdc::init()
{
	model_init();
	half_ = false;
	hi_ = 0;
	state_ = 0;
	esc_ = false;
	encoder_ = false;
	olen_ = ohead_ = 0;
}

/* the model's initial values (7B2Ah, 7BA7h): rank r is character r,
   all counts 0, t1 4, t2 BFh, off 3Ch, no run */
void HayesAdc::model_init()
{
	for (s16 i = 0; i < 256; i++) {
		count_[i] = 0;
		rank_[i] = char_[i] = static_cast<u8>(i);
	}
	tick_ = ADC_TICK;
	t1_ = 4;
	t2_ = 0xbf;
	off_ = 0x3c;
	prev_ = 0;
	run_ = -2;
}

/* t1, t2 and off from the counts (6EFCh): for e = 0 to 15, the sum
   (8 bits, with wrap) of the next 16 counts from the bottom rank up is
   compared with count[e]; the first e with a smaller count[e] gives
   k = max(e - 1, 0), and no such e gives k = 15; then t1 = k,
   t2 = 255 - 16k, off = 15k */
void HayesAdc::recalc()
{
	u8 low = 0;
	s16 e = 0, k = 15;

	for (;;) {
		u8 total = 0;
		for (s16 i = 0; i < 16; i++) {
			low = static_cast<u8>(low - 1);
			total = static_cast<u8>(total + count_[low]);
		}
		if (count_[e] < total) {
			/* SBC A,0 / ADC A,0 at 6F2Dh */
			k = e == 0 ? 0 : static_cast<s16>(e - 1);
			break;
		}
		if (e++ == 15)
			break;
	}
	t1_ = static_cast<u8>(k);
	t2_ = static_cast<u8>(~(k << 4));
	off_ = static_cast<u8>((k << 4) - k);
}

/* the character c was coded (6EF1h): each 128 characters the
   thresholds again and all counts halved (6F76h); c's count up, and c
   past the first of the 16 ranks above it with a smaller count */
void HayesAdc::update(u8 c)
{
	if (--tick_ == 0) {
		tick_ = ADC_TICK;
		recalc();
		for (s16 i = 0; i < 256; i++)
			count_[i] = static_cast<u8>(count_[i] >> 1);
	}
	const s16 r = rank_[c];
	count_[r] = static_cast<u8>(count_[r] + 1);
	if (r == 0)
		return;
	const u8 n = count_[r];
	for (s16 j = r > 16 ? static_cast<s16>(r - 16) : 0; j < r; j++) {
		if (count_[j] >= n)
			continue;
		const u8 cj = char_[j];
		count_[r] = count_[j];
		count_[j] = n;
		char_[r] = cj;
		rank_[cj] = static_cast<u8>(r);
		char_[j] = c;
		rank_[c] = static_cast<u8>(j);
		return;
	}
}

/* one octet into the output ring, as it is (dropped if the ring is
   full: the caller takes the output after each character) */
void HayesAdc::put_raw(u8 b)
{
	if (olen_ == static_cast<s16>(sizeof(obuf_)))
		return;
	s16 at = static_cast<s16>((ohead_ + olen_) %
				  static_cast<s16>(sizeof(obuf_)));
	obuf_[at] = b;
	olen_++;
}

/* one octet of code out; FAh as FAh 03h (6880h) */
void HayesAdc::put_octet(u8 b)
{
	put_raw(b);
	if (b == ADC_ESC)
		put_raw(ADC_ESC_DATA);
}

/* one nibble of code out: the high nibble waits for the low one
   (684Ch) */
void HayesAdc::nibble(u8 n)
{
	if (!half_) {
		hi_ = static_cast<u8>(n & 0x0f);
		half_ = true;
		return;
	}
	put_octet(static_cast<u8>(hi_ << 4 | (n & 0x0f)));
	half_ = false;
}

/* the rank code of the character c (67F0h): one nibble, an octet
   (two nibbles, 6863h) or the nibble 15 and an octet; then the model */
void HayesAdc::code(u8 c)
{
	const u8 r = rank_[c];
	if (r < t1_) {
		nibble(r);
	} else if (r <= t2_) {
		const u8 v = static_cast<u8>(r + off_);
		nibble(static_cast<u8>(v >> 4));
		nibble(v);
	} else {
		nibble(15);
		nibble(static_cast<u8>(r >> 4));
		nibble(r);
	}
	update(c);
}

/* one character from the DTE (67C8h): the first three equal ones are
   coded, the next ones counted (a count of 15 goes out at once, and
   the count starts again); a different one sends the count first;
   returns 0 */
s16 HayesAdc::encode(u8 ch)
{
	encoder_ = true;
	if (ch == prev_) {
		run_++;
		if (run_ <= 0) {
			code(ch);
			return 0;
		}
		if (run_ == 15) {
			nibble(15);
			run_ = 0;
		}
		return 0;
	}
	if (run_ >= 0)
		nibble(static_cast<u8>(run_));
	run_ = -2;
	prev_ = ch;
	code(ch);
	return 0;
}

/* the end of a frame on an octet boundary (6818h): the nibble that
   waits with the count; else with a pad nibble 15 and FAh FFh; a count
   alone with the pad nibble and FAh FFh; nothing if nothing waits;
   returns 0 */
s16 HayesAdc::flush_encoder()
{
	encoder_ = true;
	if (half_) {
		const u8 hi = hi_;
		half_ = false;
		if (run_ >= 0) {
			const u8 b = static_cast<u8>(hi << 4 | run_);
			run_ = -2;
			put_octet(b);
		} else {
			put_raw(static_cast<u8>(hi << 4 | 0x0f));
			put_raw(ADC_ESC);
			put_raw(ADC_ESC_END);
		}
		return 0;
	}
	if (run_ < 0)
		return 0;
	put_raw(static_cast<u8>(run_ << 4 | 0x0f));
	run_ = -2;
	put_raw(ADC_ESC);
	put_raw(ADC_ESC_END);
	return 0;
}

/* a decoded character to the output, and the model (6B19h) */
void HayesAdc::out_char(u8 c)
{
	if (c == prev_) {
		run_++;
	} else {
		prev_ = c;
		run_ = -2;
	}
	put_raw(c);
	update(c);
}

/* one nibble of code (6AA2h): a count after three equal characters,
   else the first, second or third nibble of a rank */
void HayesAdc::dec_nibble(u8 n)
{
	if (state_ == 0 && run_ >= 0) {
		run_ = n == 15 ? 0 : -2;
		for (u8 i = 0; i < n; i++)
			put_raw(prev_);
		return;
	}
	switch (state_) {
	case 0:
		if (n < t1_) {
			out_char(char_[n]);
		} else {
			hi_ = static_cast<u8>(n << 4);
			state_ = 1;
		}
		break;
	case 1: {
		const u8 v = static_cast<u8>((hi_ | n) - off_);
		if (v <= t2_) {
			state_ = 0;
			out_char(char_[v]);
		} else {
			hi_ = static_cast<u8>(n << 4);
			state_ = 2;
		}
		break;
	}
	default:
		state_ = 0;
		out_char(char_[hi_ | n]);
		break;
	}
}

/* one octet of a received frame (6B39h; the rules for FAh, 6B8Dh):
   FAh 03h is the octet FAh, FAh FFh drops the code that is not
   complete, FAh 00h or 01h sets the model to its initial values
   (7B13h), another octet after FAh is ignored; returns 0 */
s16 HayesAdc::decode(u8 ch)
{
	if (esc_) {
		esc_ = false;
		if (ch == ADC_ESC_DATA) {
			ch = ADC_ESC;
		} else if (ch == ADC_ESC_END) {
			state_ = 0;
			if (run_ >= 1)
				run_ = -2;
			return 0;
		} else if (ch < 0x02) {
			model_init();
			state_ = 0;
			return 0;
		} else {
			return 0;
		}
	} else if (ch == ADC_ESC) {
		esc_ = true;
		return 0;
	}
	dec_nibble(static_cast<u8>(ch >> 4));
	dec_nibble(static_cast<u8>(ch & 0x0f));
	return 0;
}

/* the end of a received frame: an FAh at its end has no second octet
   (each frame starts without one); returns 0 */
s16 HayesAdc::flush_decoder()
{
	esc_ = false;
	return 0;
}

/* up to n octets of the output; returns how many.  The encoder gives
   an FAh only with the octet after it, so that a frame never ends
   between the two */
s16 HayesAdc::out(u8 *buf, s16 n)
{
	s16 k = n < olen_ ? n : olen_;
	const s16 size = static_cast<s16>(sizeof(obuf_));

	if (encoder_ && k > 0 && k < olen_ &&
	    obuf_[(ohead_ + k - 1) % size] == ADC_ESC)
		k--;
	for (s16 i = 0; i < k; i++)
		buf[i] = obuf_[(ohead_ + i) % size];
	ohead_ = static_cast<s16>((ohead_ + k) % size);
	olen_ = static_cast<s16>(olen_ - k);
	return k;
}

} // namespace dlink
