/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    k56flex_train.cpp - K56flex's digital modem training (phases 3 and
 *                        4), the codes it sends before the data
 *
 *    As SM56's isdn_tx sends them (the digital modem's transmitter the
 *    analogue modem's driver carries unused), code for code; see
 *    docs/dsp/k56flex-details.md.
 *
 */

#include <string.h>

#include "bits.h"
#include "k56flex.h"

namespace dsp {

const s32 k56_seg_len[K56_NSEGS] = {
	512, 288, 16368, 8448, 8448, 128, 12288, 288, 16336, 32, 16368,
	704, 704, 12, -1,
};

/* the training from its first silence, in µ-law or A-law (law) */
void K56Train::init(s16 law)
{
	memset(this, 0, sizeof *this);
	alaw = law;
	nb = 1;
	js = 0x8990;
	segment(K56_SIL1);
}

/* start segment s: its length, and its scrambler and sign state */
void K56Train::segment(s16 s)
{
	if (s == K56_TRN1B && no_trn1b)
		s = K56_TRN1C;
	seg = s;
	n = 0;
	len = k56_seg_len[s];
	switch (s) {
	case K56_R1:
		/* the scrambler and the running sum start here */
		scr = 0;
		rds = 0;
		/* fall through */
	case K56_R2:
		rcount = 288;
		idx = 0;
		break;
	case K56_TRN1A:
	case K56_TRN1B:
	case K56_TRN2A:
		alt = 0;
		break;
	case K56_JS:
		idx = 0;
		break;
	case K56_TRN2B:
		nb = 2;
		break;
	case K56_MP:
	case K56_MPP:
		idx = pos = 0;
		break;
	default:
		break;
	}
}

/* a code sent: the running sum, the sign that alternates.  The codes
   are built with bit 7 negative, as in isdn_tx; ours (K56Enc's, and
   SM56's decoder's) have it positive, as G.711's */
unsigned char K56Train::put(unsigned char code)
{
	code ^= 0x80;
	rds += k56_value(code, alaw);
	alt ^= 1;
	return code;
}

/* TRN1C to E: nb scrambled bits of in (the lowest first).  TRN1C, Js,
   TRN2A and MARK (mag1 their magnitude): the sign the scrambled bit's
   (1 positive).  TRN2B on (mag1 0): one bit one of 2 magnitudes, two
   one of 4, the sign the scrambled bit 24 back's.  Every other sign is
   the running sum's */
unsigned char K56Train::level(s16 in, unsigned char mag1)
{
	static const unsigned char mag2[4] = { 0x2f, 0x45, 0x51, 0x57 };
	unsigned char b[2] = { 0, 0 }, mag;
	bool neg;
	for (s16 i = 0; i < nb; i++) {
		b[i] = static_cast<unsigned char>(
			scramble(&scr, SCR_GPA, (in >> i) & 1));
		hist = hist << 1 | b[i];
	}
	if (mag1)
		mag = mag1;
	else if (nb == 1)
		mag = b[0] ? 0x3d : 0x55;
	else
		mag = mag2[b[0] | b[1] << 1];
	if (alt)
		neg = rds > 0;
	else if (mag1)
		neg = !b[0];
	else
		neg = (hist >> 24) & 1;
	return put(static_cast<unsigned char>(mag | (neg ? 0x80 : 0)));
}

/* the next code of the training, moving on at each segment's end */
unsigned char K56Train::next()
{
	/* silence: the smallest positive code */
	unsigned char c = 0x80;
	switch (seg) {
	case K56_R1:
	case K56_R2: {
		c = (idx >> 1) & 1 ? 0x54 : 0xd4;
		if ((289 - rcount) % 264 == 0)
			idx = static_cast<s16>(idx + 2);
		rcount--;
		idx = (idx + 1) & 3;
		break;
	}
	case K56_TRN1A: {
		s16 b = scramble(&scr, SCR_GPA, 1);
		hist = hist << 1 | static_cast<u32>(b);
		c = put(static_cast<unsigned char>(
			((hist >> 24) & 1 ? 0x4f : 0x4e) | (b ? 0 : 0x80)));
		break;
	}
	case K56_TRN1B: {
		/* 3 bits (the first the lowest) a symbol, alternate signs
		   the running sum's too */
		static const unsigned char mag3[8] = {
			0x40, 0x41, 0x4b, 0x4c, 0x50, 0x51, 0x50, 0x51
		};
		s16 m = 0;
		for (s16 i = 0; i < 3; i++) {
			s16 b = scramble(&scr, SCR_GPA, 1);
			hist = hist << 1 | static_cast<u32>(b);
			m = static_cast<s16>(m | b << i);
		}
		bool neg = alt ? rds > 0 : !((hist >> 23) & 1);
		c = put(static_cast<unsigned char>(mag3[m] | (neg ? 0x80 : 0)));
		break;
	}
	case K56_TRN1C:
	case K56_TRN2A:
		c = alt ? put(rds > 0 ? 0xcf : 0x4f) : level(1, 0x4f);
		break;
	case K56_MARK:
		c = alt ? put(rds > 0 ? 0xcf : 0x4f) : level(0, 0x4f);
		break;
	case K56_JS:
		if (alt)
			c = put(rds > 0 ? 0xcf : 0x4f);
		else {
			c = level((js >> idx) & 1, 0x4f);
			idx = (idx + 1) & 15;
		}
		break;
	case K56_TRN2B:
		c = level(1, 0);
		break;
	case K56_MP:
	case K56_MPP: {
		const unsigned char *m = seg == K56_MP ? mp : mpp;
		s16 in = m[idx] >> pos;
		pos = static_cast<s16>(pos + nb);
		if (pos == 8) {
			pos = 0;
			if (++idx == 11)
				idx = 0;
		}
		c = level(in, 0);
		break;
	}
	case K56_E:
		c = level(3, 0);
		break;
	default:
		/* silence (and the data, which is not ours) */
		break;
	}
	n++;
	if (len >= 0 && n >= len && seg < K56_DATA)
		segment(static_cast<s16>(seg + 1));
	return c;
}

} // namespace dsp
