/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    x2.h - USRobotics' x2 (1997), the server's (digital modem's)
 *           downstream: its training and records, and the mapper of
 *           its data frames
 *
 *    The downstream goes in frames of 6 PCM codes.  Each frame takes
 *    MD + B scrambled bits (1 + x^-18 + x^-23): MD signs, then B bits
 *    that, as a number, pick one of each position's levels (mixed
 *    radix, the first position the lowest digit).  The MD signs go to
 *    the positions of the smallest levels, each sent as the change from
 *    the last one sent that way; the other position's sign holds the
 *    running sum down.  Before the data: a 24 code preamble, a two level
 *    TRN of scrambled ones, the word 09B0 over and over, ones at a
 *    higher level, a fixed alignment pattern and then mapped frames of
 *    ones and the server's record (17 ones, a zero, words each followed
 *    by a zero, CRC-16).  As the I-modem's firmware sends them, seen on
 *    the line in courier-emu; see docs/dsp/x2-details.md.
 *
 */

#ifndef X2_H
#define X2_H


#include "dm_types.h"

namespace dsp {

#define X2_FRAME        6       /* codes a frame */
#define X2_MAXBITS      48      /* data bits a frame at most */
#define X2_MAXLEVELS    128
#define X2_SYNC_MIN     10002   /* X2_SYNC's codes at least (1667 frames) */
/* the training's free sign: negative when the frame's other values sum
   above this (k56_value()'s 14 bit units) */
#define X2_FRAME_DC     805

/* a record's bits (the client's MP, the server's own): 17 ones, a zero,
   each 16 bit word from its least significant bit and a zero after it,
   the CRC (from FFFF, 8408 reflected, the words' bits only, not
   inverted), least significant bit first; n words into b, its length
   returned (without the zeros after it) */
s16 x2_record(const u16 *w, s16 n, unsigned char *b);
/* the words of a record in b (its 17 ones on), n of them: 0 when its
   zeros and CRC are good */
s16 x2_record_parse(const unsigned char *b, s16 n, u16 *w);

/* the levels of one rate: B bits a frame in levels[i] of each position,
   MD signs */
struct X2Rate {
	s16 B, MD;
	s16 n[X2_FRAME];                        /* levels of position i */
	const unsigned char *lv[X2_FRAME];      /* as G.711 octets (positive),
						   the largest first */
};

/* the downstream's mapper: frames of MD + B data bits to 6 codes (as
   K56Enc's: bit 7 positive, bits 0 to 6 the magnitude's mu-law index) */
struct X2Map {
	X2Rate r;
	u32 scr;                /* the scrambler, shared with X2Train */
	s16 last;               /* the last sign carrying a data bit */
	s32 rds;                /* the running sum of the sent values */
	s16 frame_dc;           /* the free sign by the frame's own sum */

	void init(const X2Rate &rate);
	/* the data bits a frame */
	s16 frame_bits() const { return r.B + r.MD; }
	/* frame_bits() data bits (one a byte) to 6 codes */
	void frame(const unsigned char *bits, unsigned char code[X2_FRAME]);
	/* 6 codes sent as they are (into the running sum) */
	void raw(const unsigned char code[X2_FRAME]);
};

/* the start-up rates' levels: A for the ones before the record, B the
   first data rate's (19 bits and 5 signs, 32000 bit/s) */
extern const X2Rate x2_rate_a, x2_rate_b;
/* the rate the I-modem went on to after its records in the data */
extern const X2Rate x2_rate_c;

/* the server's training, the codes before the data */
enum X2Seg {
	X2_SIL,         /* digital silence, until the client's J */
	X2_PRE,         /* the preamble, 363 */
	X2_TRN,         /* scrambled ones at two levels, 20004 */
	X2_JS,          /* 09B0 99 times and 099F once, 1200 */
	X2_ONES,        /* ones a level higher, 6000 */
	X2_ALIGN,       /* the frame alignment pattern, 1152 */
	X2_SYNC,        /* mapped frames (levels A) of ones, until the
			   client's MP */
	X2_REC,         /* our record over and over, until the client's E */
	X2_START,       /* a frame of ones in levels A, then ones in the
			   data levels, 4086 */
	X2_DATA,        /* the data frames (X2Map's) */
	X2_NSEGS
};

struct X2Train {
	s16 seg;
	s32 n, len;             /* codes sent of the segment, its length
				   (-1 held) */
	u32 scr;
	X2Map map;
	unsigned char codes[X2_FRAME];
	s16 pos;                /* the next code of codes */
	unsigned char rec[96];  /* our record's bits, 4 frames of them */
	s16 rec_pos;
	s16 rec_n;              /* records sent */
	u16 rec_w[3];
	s16 go;                 /* a held segment: on to the next (at a
				   frame; X2_REC at a record's end) */
	const X2Rate *data_rate;        /* X2_START's and X2_DATA's */
	const unsigned char *raw;       /* X2_DATA: this frame's codes as
					   they are (X2Map::raw()) */

	void init();
	void segment(s16 s);
	/* the next code; X2_DATA's: from data (frame_bits() bits a frame,
	   when pos is 0) */
	unsigned char next(const unsigned char *data = nullptr);
};

extern const s32 x2_seg_len[X2_NSEGS];

} // namespace dsp

#endif
