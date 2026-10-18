/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    mnp5.cpp - MNP 5 data compression (see mnp5.h)
 *
 */

#include <string.h>

#include "mnp5.h"

namespace dlink {

/* the flush: eleven 1s */
static const uint32_t FLUSH = 0x7ff;
static const int FLUSH_LEN = 11;
/* the longest run counted after the first three */
static const int MAX_COUNT = 250;

/* a new stream: rank r is character r, all counts 0 */
void Mnp5::init()
{
	for (int i = 0; i < 256; i++) {
		char_[i] = rank_[i] = (uint8_t)i;
		count_[i] = 0;
	}
	first_ = true;
	run_ch_ = 0;
	run_ = 0;
	acc_ = 0;
	bits_ = 0;
	dirty_ = false;
	olen_ = ohead_ = 0;
}

/* the character at rank r came once more: up past those with fewer;
   all counts halved when the top one reaches 255 */
void Mnp5::rank_up(int r)
{
	count_[r]++;
	while (r > 0 && count_[r] > count_[r - 1]) {
		uint8_t c = count_[r];
		count_[r] = count_[r - 1];
		count_[r - 1] = c;
		c = char_[r];
		char_[r] = char_[r - 1];
		char_[r - 1] = c;
		rank_[char_[r]] = (uint8_t)r;
		rank_[char_[r - 1]] = (uint8_t)(r - 1);
		r--;
	}
	if (count_[0] == 255)
		for (auto &c : count_)
			c >>= 1;
}

/* an octet out */
void Mnp5::out_char(uint8_t ch, int n)
{
	while (n-- > 0 && olen_ < (int)sizeof(obuf_))
		obuf_[(ohead_ + olen_++) % sizeof(obuf_)] = ch;
}

/* len bits out, the first the most significant of each octet */
void Mnp5::put(uint32_t bits, int len)
{
	acc_ = acc_ << len | bits;
	bits_ += len;
	while (bits_ >= 8) {
		bits_ -= 8;
		out_char((uint8_t)(acc_ >> bits_), 1);
	}
	acc_ &= (1u << bits_) - 1;
}

/* the code of value v (a rank or a count): the header h (the highest
   bit of v; 0 for 0 and 1) and the body (v without that bit; 255 one
   0 more) */
void Mnp5::put_value(int v)
{
	if (v < 2) {
		put((uint32_t)v, 4);
		return;
	}
	int h = 0;              /* (the highest bit's place) */
	while ((v >> (h + 1)) != 0)
		h++;
	uint32_t code = (uint32_t)h << h | (uint32_t)(v - (1 << h));
	if (v == 255)
		put(code << 1, 3 + h + 1);
	else
		put(code, 3 + h);
}

/* a character by its rank, which it then moves up from */
void Mnp5::put_char(uint8_t ch)
{
	int r = rank_[ch];
	put_value(r);
	rank_up(r);
}

/* the encoder: the first three of a run as characters, the rest
   counted (a count when another comes, or at 250) */
int Mnp5::encode(uint8_t ch)
{
	dirty_ = true;
	if (first_) {
		first_ = false;
		run_ch_ = ch;
		run_ = 1;
		put_char(ch);
		return 0;
	}
	if (ch == run_ch_) {
		run_++;
		if (run_ - 3 == MAX_COUNT) {
			put_value(MAX_COUNT);
			first_ = true;
			run_ = 0;
		} else if (run_ <= 3)
			put_char(ch);
		return 0;
	}
	if (run_ >= 3)
		put_value(run_ - 3);
	run_ch_ = ch;
	run_ = 1;
	put_char(ch);
	return 0;
}

/* the encoder's flush (when the DTE has nothing more): the count of a
   run of three or more, the flush, the rest of the octet 1s */
int Mnp5::flush_encoder()
{
	if (!dirty_)
		return 0;
	dirty_ = false;
	if (run_ >= 3)
		put_value(run_ - 3);
	put(FLUSH, FLUSH_LEN);
	if (bits_)
		put((1u << (8 - bits_)) - 1, 8 - bits_);
	first_ = true;
	run_ = 0;
	return 0;
}

/* the decoder: a value, after three equal characters a count, else a
   rank (which moves up) */
void Mnp5::token(int v)
{
	if (first_) {
		first_ = false;
		run_ch_ = char_[v];
		run_ = 1;
		out_char(run_ch_, 1);
		rank_up(v);
		return;
	}
	if (run_ >= 3) {
		out_char(run_ch_, v);
		first_ = true;
		run_ = 0;
		return;
	}
	uint8_t ch = char_[v];
	if (ch == run_ch_)
		run_++;
	else {
		run_ch_ = ch;
		run_ = 1;
	}
	out_char(ch, 1);
	rank_up(v);
}

/* the decoder: an octet's bits after those held, each whole code
   taken; the flush drops the rest of its octet (and a count three
   equal characters wait for: none) */
int Mnp5::decode(uint8_t ch)
{
	acc_ = acc_ << 8 | ch;
	bits_ += 8;
	while (bits_ >= 3) {
		const int h = (int)(acc_ >> (bits_ - 3)) & 7;
		const int blen = h ? h : 1;
		if (bits_ < 3 + blen)
			break;
		int v = (h ? 1 << h : 0) +
			(int)((acc_ >> (bits_ - 3 - blen)) & ((1u << blen) - 1));
		int len = 3 + blen;
		if (v == 255) {
			if (bits_ < 11)
				break;
			if (acc_ >> (bits_ - 11) & 1) {
				/* the flush: its octet's rest is padding */
				bits_ = 0;
				if (run_ > 3)
					out_char(run_ch_, run_ - 3);
				first_ = true;
				run_ = 0;
				break;
			}
			len = 11;
		}
		bits_ -= len;
		token(v);
	}
	acc_ &= bits_ ? (1u << bits_) - 1 : 0;
	return 0;
}

/* the output so far, up to n octets; how many */
int Mnp5::out(uint8_t *buf, int n)
{
	int k = 0;
	while (k < n && olen_) {
		buf[k++] = obuf_[ohead_];
		ohead_ = (ohead_ + 1) % (int)sizeof(obuf_);
		olen_--;
	}
	return k;
}

} // namespace dlink
