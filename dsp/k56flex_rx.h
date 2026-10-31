/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    k56flex_rx.h - K56flex's analogue modem: the PCM receiver
 *
 *    The digital modem's training (K56Train's codes, as their voltages
 *    through the network's codec and our line), then its data frames
 *    (K56Dec).  R's reversal gives the symbols' grid; from there on the
 *    training up to Js is known code for code, so the equaliser (T/2,
 *    as V.90's analogue modem's) is solved on TRN1A and follows TRN1A,
 *    TRN1B and TRN1C, and Js's bits are decoded from the signs.  After
 *    the silence the second R, TRN2A, MARK and TRN2B on the decisions;
 *    the MP and MP' frames from TRN2B's magnitudes (2 bits a symbol),
 *    and the data from 12 symbols after the last MP'.  See
 *    docs/dsp/k56flex-details.md.
 *
 */

#ifndef K56FLEX_RX_H
#define K56FLEX_RX_H


#include "dm_types.h"
#include "k56flex.h"
#include "v34.h"
#include "v90a.h"

namespace dsp {

#define K56RX_HIST      2048    /* samples kept at 16 000 Hz */
#define K56RX_HIST8     1024    /* R's: every other one */
#define K56RX_TAPS      128     /* T/2: 64 symbols */
#define K56RX_EQ_D      (K56RX_TAPS / 2)
#define K56RX_LS        1020    /* symbols the equaliser is solved on */
#define K56RX_Q         1024    /* data bits held for the caller */

struct K56Rx {
	enum Stage {
		WAIT_R,         /* until R and its reversal */
		TRAIN,          /* R-bar, TRN1A: the equaliser's solution */
		RUN,            /* symbols decided */
	};

	s16 alaw;
	s16 lin[128];           /* code magnitude -> linear value */
	enum Stage stage;

	V90Interp ip;
	float x[K56RX_HIST];
	s32 n;
	float x8[K56RX_HIST8];
	s32 n8;

	/* R (+ + - -, 2000 Hz): over the last R_WIN 8 kHz samples x(n)^2,
	   x(n) x(n - 4), x(n) x(n - 2); over the last 4, the first two
	   (the reversal) */
	double e16, a4_16, a2_16, e4, a4_4;
	s16 r_run, r_dip;
	double dip;
	s32 dip_at;

	/* symbol k (0: R's first reversed one, its 265th) at sample
	   b + 2 k */
	s32 b, k;
	s16 refined;
	float c[K56RX_TAPS];
	double R[K56RX_TAPS][K56RX_TAPS], p[K56RX_TAPS];
	double L[K56RX_TAPS][K56RX_TAPS];
	/* the digital modem's training as it sends it, from R's 265th
	   symbol to Js; its first TRN1A values (refine()) */
	K56Train ref;
	float trn0[96];
	float level;            /* TRN1A's: lin[0x4f] */
	double err_sum;
	s32 err_n;
	float mse;              /* of late, to level's power */
	/* the timing loop (as V90Rx's) */
	s16 t_on;
	s32 t_n;
	double t_freq, t_pow, t_phase;
	double t_sx, t_sy, t_sxx, t_sxy;
	s32 t_sn;
	s16 ls2_n;

	/* where the segments start (symbols); -1 not yet known */
	s32 k_js, k_sil2, k_r2, k_trn2a, k_trn2b, k_mp, k_data;
	K56Train r2;            /* the second R's codes */
	u32 dscr;               /* the descrambler */
	u16 js_sr;
	s16 js_n;
	u16 js;                 /* Js: J's 16 bits, the first the lowest */
	s16 ones;               /* TRN2A's descrambled ones in a row */
	/* TRN2B on: the descrambled bits (2 a symbol), the last 88 for
	   the MP frames */
	unsigned char mpw[88];
	s32 mp_bits;
	s32 mp_last;            /* the last MP frame's first symbol */
	V34Mp mp;

	/* data mode: the decoder, its frame's codes, frames decoded (the
	   first 6 B1's), the bits for the caller; the signal's power and
	   the decisions' error of late */
	K56Dec dec;
	s16 dec_on;
	unsigned char mags[128];        /* the rate's magnitudes, rising */
	s16 nmags;
	unsigned char code[8];
	s16 fpos;
	s32 frames;
	unsigned char q[K56RX_Q];
	s16 qh, qn;
	float dpow, de_mse;
	s32 dec_err0;

	/* events (cleared by the caller) */
	s16 ev_r;               /* R's reversal */
	s16 ev_trained;
	s16 ev_js;              /* Js decoded */
	s16 ev_r2;              /* the second R */
	s16 ev_trn2a;           /* TRN2A's ones */
	s16 ev_mp;              /* an MP frame (mp) */
	s16 ev_mpack;           /* an MP' frame */
	s16 ev_data;            /* the data frames have begun */
	s16 bad;                /* the MP's rate is not one we decode */

	/* the line at rate (9600 Hz), the law */
	void init(s16 alaw_, s16 rate = 9600);
	/* one sample of the line */
	void line(float v);
	/* data mode: up to n of its bits into b, oldest first (how many) */
	s16 take(unsigned char *b, s16 n);
	/* the codec's clock against the network's, parts per million */
	double ppm() const { return t_freq / 2 * 1e6; }

private:
	/* the sample i of the history */
	float at(s32 i) const { return x[i & (K56RX_HIST - 1)]; }
	/* the sample i of the 8 kHz history */
	float at8(s32 i) const { return x8[i & (K56RX_HIST8 - 1)]; }
	/* tap i's input for symbol kk, d samples later */
	float xin(s32 kk, s16 i, s16 d = 0) const
	{
		return at(b + 2 * kk + K56RX_EQ_D + d - i);
	}
	void sample(float v);
	void r_detect(float v);
	void refine();
	void solve();
	float equalize(s32 kk, s16 d = 0) const;
	void adapt(s32 kk, float e);
	void timing(s32 kk, float y, float a);
	void timing_free();
	void timing_step(double dt);
	float value(unsigned char code) const;
	void symbol(float y);
	void mp_bit(s16 bit);
	void data(float y);
	void ls_add(float a);
};

} // namespace dsp

#endif
