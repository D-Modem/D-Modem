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
	   codes (bit 7: positive, as G.711's, bits 0 to 6: the magnitude's
	   mu-law index,
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

/* the digital modem's training (phases 3 and 4), the codes it sends
   before the data: segments of fixed lengths, each on to the next unless
   held (len -1).  One scrambler (1 + x^-5 + x^-23) runs through them
   all.  TRN1A's signs are its scrambled ones, its magnitudes two
   levels chosen by the scrambled bit 24 symbols back; from TRN1B on
   every other sign is set against the running sum.  TRN1B sends 3
   scrambled bits a symbol in 6 magnitudes, its other signs the
   scrambled bit 23 back's; in TRN1C, TRN2A, Js and MARK the other signs
   are scrambled ones (TRN1C, TRN2A), J's 16 bits over and over (Js) or
   zeros (MARK).  TRN2B, MP, MP' and E send 2 scrambled bits a symbol
   in 4 magnitudes, their signs the running sum's or the scrambled bit
   24 back.  See docs/dsp/k56flex-details.md */
enum K56Seg {
	K56_SIL1,       /* silence, 512 */
	K56_R1,         /* R: + + - - (as mu-law index 84), R-bar its
			   last 24, 288 */
	K56_TRN1A,      /* 16368 */
	K56_TRN1B,      /* 8448 (not in SM56's isdn_tx; its receiver's) */
	K56_TRN1C,      /* 8448 */
	K56_JS,         /* 128 */
	K56_SIL2,       /* 12288 */
	K56_R2,         /* 288 */
	K56_TRN2A,      /* 16336 */
	K56_MARK,       /* 32 */
	K56_TRN2B,      /* 16368 */
	K56_MP,         /* 704: 11 octets over and over */
	K56_MPP,        /* MP', 704 */
	K56_E,          /* 12 */
	K56_DATA,       /* the frames that follow */
	K56_NSEGS
};

extern const s32 k56_seg_len[K56_NSEGS];

struct K56Train {
	s16 seg;
	s32 n, len;             /* symbols sent of the segment, its length */
	s16 alaw;
	u32 scr, hist;          /* the scrambler, its output bits */
	s32 rds;
	s16 alt;                /* this symbol's sign the running sum's */
	s16 idx, pos, nb;       /* R's phase, J's bit, MP's octet and bit;
				   bits a symbol */
	s16 rcount;             /* R: symbols left */
	u16 js;                 /* J's 16 bits, the first sent the lowest */
	s16 no_trn1b;           /* TRN1B left out (as isdn_tx) */
	unsigned char mp[11], mpp[11];

	void init(s16 law);
	void segment(s16 s);
	/* the next code (as K56Enc's); K56_DATA's are not this */
	unsigned char next();

private:
	unsigned char level(s16 in, unsigned char mag1);
	unsigned char put(unsigned char code);
};

/* a code (as K56Enc's) as a G.711 octet of the law, and back */
unsigned char k56_g711(unsigned char code, s16 alaw);
unsigned char k56_code(unsigned char g711, s16 alaw);
/* a code's value, mu-law codes in 14 bit units, A-law ones in 13 */
s32 k56_value(unsigned char code, s16 alaw);

} // namespace dsp

#endif
