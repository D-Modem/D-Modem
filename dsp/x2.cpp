/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    x2.cpp - x2's downstream: the server's training, records and data
 *             mapper (x2.h)
 *
 *    The codes are the ones the I-modem (Ie030002) sends to a Courier in
 *    courier-emu's closed loop, segment for segment; how they are made
 *    was read off its line octets.  See docs/dsp/x2-details.md.
 *
 */

#include <string.h>

#include "bits.h"
#include "k56flex.h"
#include "x2.h"

namespace dsp {

const s32 x2_seg_len[X2_NSEGS] = {
	-1, 363, 20004, 1200, 6000, 1152, -1, -1, 4080, -1,
};

/* the preamble: 22 codes over and over (as G.711 octets), 16 times,
   then its first 11 with their signs turned */
static const unsigned char x2_pre[22] = {
	0xab, 0xbd, 0xab, 0x2b, 0x3d, 0x2b, 0xab, 0x2b, 0x3d, 0xbd, 0x3d, 0xbd,
	0xab, 0xbd, 0x3d, 0x2b, 0x3d, 0xbd, 0x3d, 0x2b, 0xab, 0x2b,
};
#define X2_PRE_TURN     352

/* the alignment pattern: frame f is these turned f places left */
static const unsigned char x2_align[X2_FRAME] = {
	0x9e, 0x28, 0xa1, 0x21, 0xa8, 0x1e,
};

/* TRN's and J's level, the ones' after them (G.711, positive) */
#define X2_TRN_LEVEL    0xb1
#define X2_ONES_LEVEL   0xc1

/* the words of X2_JS: J's (09B0) 99 times, then 099F */
#define X2_J            0x09b0
#define X2_J_END        0x099f
#define X2_J_COUNT      99

/* the levels: A's the same in each position, B's too */
static const unsigned char x2_lv_a[9] = {
	0xa5, 0xa7, 0xad, 0xaf, 0xb7, 0xbd, 0xc5, 0xcf, 0xe5,
};
static const unsigned char x2_lv_b[9] = {
	0xa5, 0xa8, 0xab, 0xae, 0xb3, 0xb9, 0xbf, 0xcb, 0xdf,
};

const X2Rate x2_rate_a = {
	19, 5, { 9, 9, 9, 9, 9, 9 },
	{ x2_lv_a, x2_lv_a, x2_lv_a, x2_lv_a, x2_lv_a, x2_lv_a },
};
const X2Rate x2_rate_b = {
	19, 5, { 9, 9, 9, 9, 9, 9 },
	{ x2_lv_b, x2_lv_b, x2_lv_b, x2_lv_b, x2_lv_b, x2_lv_b },
};

/* the CRC of a record's words (bits, one a byte) */
static u16 x2_crc(const unsigned char *b, s16 n)
{
	u16 crc = 0xffff;
	for (s16 i = 0; i < n; i++) {
		u16 v = static_cast<u16>(crc ^ (b[i] & 1));
		crc = static_cast<u16>((v >> 1) ^ (v & 1 ? 0x8408 : 0));
	}
	return crc;
}

/* (see x2.h) */
s16 x2_record(const u16 *w, s16 n, unsigned char *b)
{
	unsigned char d[16 * 8];
	s16 k = 0, i, j;
	for (i = 0; i < 17; i++)
		b[k++] = 1;
	b[k++] = 0;
	for (i = 0; i < n; i++) {
		for (j = 0; j < 16; j++) {
			d[16 * i + j] = (w[i] >> j) & 1;
			b[k++] = d[16 * i + j];
		}
		b[k++] = 0;
	}
	u16 crc = x2_crc(d, static_cast<s16>(16 * n));
	for (j = 0; j < 16; j++)
		b[k++] = (crc >> j) & 1;
	return k;
}

/* (see x2.h) */
s16 x2_record_parse(const unsigned char *b, s16 n, u16 *w)
{
	unsigned char d[16 * 8];
	s16 k = 18, i, j;
	if (b[17] & 1)
		return -1;
	for (i = 0; i < n; i++) {
		u16 v = 0;
		for (j = 0; j < 16; j++) {
			d[16 * i + j] = b[k] & 1;
			v = static_cast<u16>(v | (b[k++] & 1) << j);
		}
		if (b[k++] & 1)
			return -1;
		w[i] = v;
	}
	u16 crc = 0;
	for (j = 0; j < 16; j++)
		crc = static_cast<u16>(crc | (b[k++] & 1) << j);
	return crc == x2_crc(d, static_cast<s16>(16 * n)) ? 0 : -1;
}

/* the levels of rate; the scrambler and signs as they are */
void X2Map::init(const X2Rate &rate)
{
	r = rate;
}

/* (see x2.h): scrambled, the digits, the signs, the free one */
void X2Map::frame(const unsigned char *bits, unsigned char code[X2_FRAME])
{
	unsigned char s[X2_MAXBITS];
	s16 nb = frame_bits(), d[X2_FRAME], car[X2_FRAME], i, j;
	u64 x = 0;
	for (i = 0; i < nb; i++)
		s[i] = static_cast<unsigned char>(scramble(&scr, SCR_GPC,
							   bits[i] & 1));
	for (i = 0; i < r.B; i++)
		x |= static_cast<u64>(s[r.MD + i]) << i;
	for (i = 0; i < X2_FRAME; i++) {
		d[i] = static_cast<s16>(x % static_cast<u64>(r.n[i]));
		x /= static_cast<u64>(r.n[i]);
		car[i] = 0;
	}
	/* the signs: the MD positions of the highest digits (the smallest
	   levels; of equal ones the first), in their order */
	for (j = 0; j < r.MD; j++) {
		s16 best = -1;
		for (i = 0; i < X2_FRAME; i++)
			if (!car[i] && (best < 0 || d[i] > d[best]))
				best = i;
		car[best] = 1;
	}
	for (i = 0, j = 0; i < X2_FRAME; i++) {
		code[i] = k56_code(r.lv[i][d[i]], 0);
		if (car[i]) {
			last = static_cast<s16>(s[j++] ^ last);
			if (!last)
				code[i] &= 0x7f;
			rds += k56_value(code[i], 0);
		}
	}
	/* the others against the running sum */
	for (i = 0; i < X2_FRAME; i++) {
		if (car[i])
			continue;
		if (rds > 0)
			code[i] &= 0x7f;
		rds += k56_value(code[i], 0);
	}
}

/* silence, with our record's words */
void X2Train::init()
{
	memset(this, 0, sizeof *this);
	rec_w[0] = 0xf33c;
	rec_w[1] = 0x7fff;
	rec_w[2] = 0x0000;
	segment(X2_SIL);
}

/* on to segment s, from its start */
void X2Train::segment(s16 s)
{
	seg = s;
	n = 0;
	len = x2_seg_len[s];
	go = 0;
	pos = 0;
	switch (s) {
	case X2_SYNC:
		map.init(x2_rate_a);
		map.last = 1;
		break;
	case X2_REC:
		/* our record, in whole frames (zeros after it) */
		memset(rec, 0, sizeof rec);
		x2_record(rec_w, 3, rec);
		rec_pos = 0;
		rec_n = 0;
		break;
	case X2_START:
		map.init(data_rate ? *data_rate : x2_rate_b);
		break;
	default:
		break;
	}
}

/* (see x2.h) */
unsigned char X2Train::next(const unsigned char *data)
{
	unsigned char code, bits[X2_MAXBITS];
	s16 b;
	switch (seg) {
	case X2_SIL:
	default:
		code = 0;
		break;
	case X2_PRE:
		code = n < X2_PRE_TURN ? k56_code(x2_pre[n % 22], 0) :
			static_cast<unsigned char>(
				k56_code(x2_pre[n - X2_PRE_TURN], 0) ^ 0x80);
		break;
	case X2_TRN:
	case X2_JS:
	case X2_ONES:
		b = 1;
		if (seg == X2_JS) {
			s32 w = n / 12;
			b = ((w < X2_J_COUNT ? X2_J : X2_J_END) >> (n % 12)) &
				1;
		}
		/* positive for a scrambled 0 */
		b = scramble(&map.scr, SCR_GPC, b);
		code = k56_code(seg == X2_ONES ? X2_ONES_LEVEL : X2_TRN_LEVEL,
				0);
		if (b)
			code &= 0x7f;
		break;
	case X2_ALIGN:
		code = k56_code(x2_align[(n % X2_FRAME + n / X2_FRAME) %
					 X2_FRAME], 0);
		break;
	case X2_SYNC:
	case X2_REC:
	case X2_START:
	case X2_DATA:
		if (!pos) {
			s16 nb = map.frame_bits();
			if (seg == X2_DATA && data) {
				memcpy(bits, data, static_cast<size_t>(nb));
			} else if (seg == X2_REC) {
				for (s16 i = 0; i < nb; i++) {
					bits[i] = rec[rec_pos++];
					if (rec_pos == sizeof rec) {
						rec_pos = 0;
						rec_n++;
					}
				}
			} else {
				memset(bits, 1, sizeof bits);
			}
			map.frame(bits, codes);
		}
		code = codes[pos];
		pos = static_cast<s16>((pos + 1) % X2_FRAME);
		break;
	}
	n++;
	if (len >= 0 && n >= len) {
		segment(static_cast<s16>(seg + 1));
	} else if (len < 0 && go) {
		/* a held segment ends: on a frame, a record */
		bool at = seg < X2_SYNC ||
			(!pos && (seg != X2_REC || !rec_pos));
		if (at && seg != X2_DATA)
			segment(static_cast<s16>(seg + 1));
	}
	return code;
}

} // namespace dsp
