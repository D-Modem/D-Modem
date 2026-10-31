/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    k56flex.h - K56flex's data mode: the digital modem's encoder (data
 *                bits to PCM codes) and the analogue modem's decoder
 *
 *    The downstream is sent in frames of 8 symbols (8 kHz PCM codes).
 *    Each frame takes R / 1000 data bits (R the rate, 32000 to 56000
 *    bit/s in steps of 2000), fewer on robbed-bit lines.  The bits are
 *    scrambled (1 + x^-5 + x^-23, least significant bit of each octet
 *    first), then K of them pick 8 rings through V.34's shell mapper,
 *    and the rest pick a point within each ring and its sign.  Every
 *    r-th symbol's sign carries no data: it is set against the running
 *    sum of the sent values, which keeps the spectrum's DC low.  See
 *    docs/dsp/k56flex-details.md.
 *
 */

#ifndef K56FLEX_H
#define K56FLEX_H


#include "dm_types.h"

namespace dsp {

#define K56_NRATES      13      /* 32000 to 56000 bit/s */
#define K56_MAXBITS     56      /* data bits a frame at most */

/* a rate's shell mapper and shaping parameters */
struct K56Rate {
	unsigned char K;        /* bits through the shell mapper */
	unsigned char r;        /* every r-th symbol's sign is the shaping's */
	unsigned char u;        /* u - 1 bits a symbol choose within its ring */
	unsigned char M[3];     /* rings, for a pad of 0, 3 and 6 dB */
};

extern const K56Rate k56_rates[K56_NRATES];
extern const unsigned char k56_rbs_loss[7];
extern const unsigned char *const k56_ring[3][K56_NRATES];
extern const unsigned char *const k56_bios[3][K56_NRATES];

/* the codec's line: rate (bit/s), the digital pad (0, 1: 3 dB, 2: 6 dB),
   the law and which symbols of the 6 symbol robbed-bit cycle are robbed
   (bit k: symbol k) */
struct K56Line {
	s32 rate;
	s16 pad, alaw;
	unsigned char robbed;
};

/* the parts that the encoder and decoder share */
struct K56Codec {
	s16 K, M, q, r;         /* q = u - 1 */
	s16 nrob;               /* robbed symbols in 6 (5 robs all 6) */
	s16 loss;               /* robbed bits a frame */
	s16 alaw;
	s32 rate;
	const unsigned char *ring, *bios;
	unsigned char rob[24];  /* robbed, per symbol of 3 frames */
	s16 nrob_f[3];          /* robbed symbols in each of the 3 frames */
	u32 g2[64], g4[128], z8[128];  /* (0 past their ends: the mapper reads there) */
	s16 f;                  /* the frame in the 3 frame cycle */
	s16 rc;                 /* symbols since the last shaping sign */
	s16 half;               /* r = 16: the frame in a pair */
	s32 rds;                /* the running sum of the sent values */
	u32 scr;

	s16 init(const K56Line &l);
	s16 next_frame();
	s16 shell_bits(s16 *adj) const;
	bool shaping_next();
	void sent(unsigned char code);
};

/* the digital modem's encoder */
struct K56Enc : K56Codec {
	/* the data bits the next frame takes */
	s16 frame_bits() const;
	/* a frame: frame_bits() data bits (one a byte, in time order) to 8
	   codes (bit 7: negative, bits 0 to 6: the magnitude's mu-law index,
	   0 the smallest) */
	void frame(const unsigned char *bits, unsigned char code[8]);
};

/* the analogue modem's decoder */
struct K56Dec : K56Codec {
	unsigned char inv[128];         /* magnitude -> point + 1 (0: none) */
	unsigned char inv_ring[32];     /* group -> ring */
	s32 errors;                     /* codes and frames that cannot be */

	s16 init(const K56Line &l);
	/* a frame: 8 received codes (as K56Enc's) to data bits; returns
	   how many */
	s16 frame(const unsigned char code[8], unsigned char *bits);
};

/* a code (as K56Enc's) as a G.711 octet of the law, and back */
unsigned char k56_g711(unsigned char code, s16 alaw);
unsigned char k56_code(unsigned char g711, s16 alaw);
/* a code's value, mu-law codes in 14 bit units, A-law ones in 13 */
s32 k56_value(unsigned char code, s16 alaw);

} // namespace dsp

#endif
