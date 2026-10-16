/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v90a.h - V.90 analogue modem: the PCM receiver (docs/dsp/v90a.md)
 *
 *    The digital modem's codewords, as their voltages through the
 *    network's codec, the line and our codec, at 8000 samples a second
 *    (the network's clock).  Phase 3 so far (9.3.2/V.90): Sd and its
 *    reversal (the data frame alignment), the equaliser trained on
 *    TRN1d, Jd and Jd', the DIL measured: each codeword's level in each
 *    frame interval, what that shows of the network (its pads, robbed
 *    bits, noise).
 *
 */

#ifndef V90A_H
#define V90A_H

#include "v34.h"

namespace dsp {

/* Jd (Table 13/V.90), the digital modem's capabilities */
struct V90Jd {
	unsigned rate_mask;     /* bit 0: 28 000 bit/s ... bit 22: 56 000 */
	int cp16_train;         /* bit 47: CP, E and SCR on 16 points in
				   training */
	int cp16_reneg;         /* bit 48: and in rate renegotiation */
	int lookahead;          /* bits 49:50 */

	/* 72 bits (the first in b[0]): 0, or -1 when they are not a Jd */
	int parse(const unsigned char *b);
};

#define V90RX_HIST      1024    /* samples kept (a power of 2) */
/* the equaliser's taps, T-spaced: the symbols fall between our samples
   (the network's clock and ours converted), and a T-spaced equaliser
   makes that delay with a sinc's long tails (31 taps: 18 dB, 63: 23,
   127: 32 to 36 dB on netsim's line) */
#define V90RX_TAPS      127
#define V90RX_LS        1020    /* TRN1d symbols the equaliser is first
				   solved on (least squares) */
#define V90RX_DIL_ROUNDS 2      /* the DIL's rounds measured */

struct V90Rx {
	enum Stage {
		WAIT_SD,        /* until Sd */
		SD,             /* in Sd, until its reversal */
		TRAIN,          /* the reversal seen: S-bar d, TRN1d */
		DECIDE,         /* trained: decisions, Jd and Jd' */
	};

	int alaw, uinfo;
	int lin[128];           /* Ucode -> linear value, positive */
	enum Stage stage;

	float x[V90RX_HIST];    /* the input */
	long n;                 /* samples in */

	/* Sd: running sums over the last SD_WIN samples of x(n)^2,
	   x(n) x(n - 6), x(n) x(n - 3), and over the last 6 of the first
	   two (the reversal) */
	double e24, a6_24, a3_24, e6, a6_6;
	int sd_run;             /* samples Sd has been there */
	double dip;             /* the reversal: its least a6_6 / e6 */
	long dip_at;

	/* symbol k (0: the first of S-bar d) is around sample b + k */
	long b;
	long k;                 /* the next symbol to decide */
	int refined;
	float c[V90RX_TAPS];
	/* least squares: the normal equations, R's Cholesky factor (some
	   250 KB: the pump is on the heap) */
	double R[V90RX_TAPS][V90RX_TAPS], p[V90RX_TAPS];
	double L[V90RX_TAPS][V90RX_TAPS];
	unsigned trn_scr;       /* TRN1d's signs: GPC on ones from zero */
	float level;            /* lin[uinfo] */
	double err_sum;
	long err_n;
	float mse;              /* of the decisions, to the points' power */

	/* Jd: the decisions' signs differentially decoded and
	   descrambled */
	int last_sign;
	unsigned dscr;
	unsigned char jb[72];   /* the last 72 bits, the newest at jpos - 1 */
	int jpos;
	long jbits;
	long jd_end;            /* bit count at the end of the last Jd */
	int zeros;              /* zeros since then */

	/* events (cleared by the caller) */
	int ev_sd;              /* Sd there */
	int ev_sdbar;           /* its reversal */
	int ev_trained;
	int ev_jd;              /* Jd frames received */
	int ev_jdp;             /* Jd' after them */
	int ev_dil;             /* the DIL's rounds measured */
	V90Jd jd;

	/* the DIL our Ja asked for (8.4.1/V.90): it follows Jd' (in data
	   frame interval 0), round after round; the decisions' values
	   (the equaliser still) by frame interval and Ucode, the sign
	   SP's.  Its first symbol, a round's length, where in it we are */
	V90Dil dil;
	int dil_on;
	long dil_start, dil_len;
	int dil_seg, dil_k;
	double dsum[6][128], dsq[6][128];
	int dcnt[6][128];
	/* what it shows (dil_analyse()): each Ucode's level in each frame
	   interval (the equaliser's scale: UINFO is lin[UINFO]; 0 if not
	   measured; usable unless its own noise stands out, as a clipped
	   one's does), the noise on them (the median of each Ucode's: the
	   equaliser's mse on random data, mse, is the data mode's noise),
	   the network's digital loss (a pad: the equaliser, trained on
	   UINFO through it, takes its gain away, but its requantization
	   shows in the levels), the
	   frame intervals whose least significant bit is robbed (odd and
	   even Ucodes alike), and how many levels each interval tells
	   apart (more than 3 noise apart) */
	float dil_level[6][128];
	unsigned char dil_usable[6][128];       /* measured, its noise not
						   standing out (clipped) */
	float dil_noise;
	float dil_pad_db;       /* the network's digital loss: the pad whose
				   requantized levels fit the DIL's best */
	int dil_robbed;
	int dil_distinct[6];

	void init(int alaw_, int uinfo_);
	/* the DIL to measure (N 0: none) */
	void dil_set(const V90Dil &d);
	void dil_analyse();
	/* one sample at 8000 Hz */
	void sample(float v);

private:
	float at(long i) const { return x[i & (V90RX_HIST - 1)]; }
	void sd_detect(float v);
	void refine();
	void solve();
	float equalize(long kk) const;
	void decide(float y);
	void jd_bit(int bit);
};

} // namespace dsp

#endif
