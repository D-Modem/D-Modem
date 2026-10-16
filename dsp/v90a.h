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
 *    bits, noise).  Phase 4 (9.4.2/V.90): Ri and its reversal, TRN2d,
 *    MP and Ed through the data mode decoder on our CPt's
 *    constellations, B1d on CP's.
 *
 */

#ifndef V90A_H
#define V90A_H

#include "v90.h"

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

/* MP (Table 16/V.90), the digital modem's modulation parameters */
struct V90Mp {
	int type;               /* 1: with precoding coefficients */
	int rate_max;           /* most upstream data rate, N * 2400 */
	int trellis, nonlinear, expanded, ack;
	int rate_mask;          /* bit 0: 4800 .. bit 12: 33 600 */
	int h[6];

	/* the bits of a frame of len (86 or 188) bits from its frame sync
	   on: 0, or -1 (fields unchanged) on a bad CRC */
	int parse(const unsigned char *b, int len);
};

/* the constellations of a CP as the decoder takes them: by frame
   interval its points by label (0: the largest Ucode), each with its
   level at our receiver (the equaliser's scale) */
struct V90Con {
	int D, S, K;
	int M[6];
	unsigned char ucode[6][128];
	float lev[6][128];

	/* from a CP of D bits per data frame and the levels of each
	   Ucode in each interval: -1 when it can't carry them */
	int init(const V90Cp &cp, int D_, const float lv[6][128]);
	/* the average power (Table 15/V.90, the transmitter's linear
	   values lin) */
	double power(const int *lin) const;
};

/* the analogue modem's constellations (8.5.2/V.90) from the levels of
   each Ucode at our receiver in each interval (0: not to be used), the
   noise on them (rms, the same scale), the limit of Table 15 (rms, 16
   bit scale) and Jd's rates: CPt of 4 points per interval (24 kbit/s)
   at about 1 dB below the limit; CP the fastest set whose points are
   10 noise apart (a symbol error in 10^6), within the limit and 3 dB
   of CPt's power.  Returns -1 when there is no such set */
int  v90a_design_cpt(V90Cp *cp, int alaw, const int *lin,
		     const float lv[6][128], double limit);
int  v90a_design_cp(V90Cp *cp, int alaw, const int *lin,
		    const float lv[6][128], double noise, double limit,
		    double cpt_power, unsigned jd_rates);

/* the line at 9600 Hz to the network's 8000 Hz for the PCM receiver: an
   interpolator that keeps the whole band below 4800 Hz (the line's
   Nyquist), as the PCM signal fills it up to 4000 Hz and past; the
   samples at 8000 Hz alias it as the network's did (dsp_util's
   Resampler cuts below 4000 Hz, leaving intersymbol interference no
   equaliser takes away: some 32 dB below the signal) */
#define V90_INTERP_TAPS 125     /* per phase, 5 phases at 48 kHz: a delay
				   of 52 samples at 8000 Hz */
struct V90Interp {
	float h[5][V90_INTERP_TAPS];
	float hist[2 * V90_INTERP_TAPS];
	int pos, t;

	void init();
	/* one sample at 9600 Hz in; 0 or 1 out at 8000 Hz */
	int run(float x, float *out);
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
		RI,             /* phase 4: Ri, then its reversal */
		FRAMES,         /* TRN2d, MP, Ed, B1d: data frames */
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
	/* after TRN1d's least length until Jd (the DIL's loud codes later
	   reach into the equaliser's span): UINFO's magnitudes by frame
	   interval
	   (their spread about each interval's own mean is the noise: a
	   robbed bit or the network's gain moves the mean, not the
	   decisions) */
	double usum[6], usq[6];
	long un[6];

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

	/* phase 4: Ri (+ + + - - - of UINFO) frames in a row, Ri-bar's
	   first symbol; from TRN2d (24 on) the data frames: the
	   constellations (CPt's, then CP's for B1d), the frame's values,
	   frames decoded, the sign and descrambler state; TRN2d's
	   symbols as the digital modem's encoder makes them (ref, Ucode +
	   1 for the linear values) and the errors on them; the frame
	   bits that make MP sequences; Ed (2 frames of zeros after an
	   MP' sequence's last), B1d's 48 frames of ones */
	int ri_run;
	long ri_seen, ribar_at, frames_at;
	V90Con con_trn, con_data;
	const V90Con *con;
	float fy[6];
	long frames, frames_b1;
	int flast;
	unsigned fscr;
	V90Enc ref;
	int ref_lin[128];
	float lv4[6][128];      /* levels(), at phase4() */
	int rs[6];              /* Ri: the frame's signs */
	long trn_sym_err, trn_sym;
	unsigned char mpb[200];
	int mp_n, mp_len;
	unsigned msr;
	long mp_frame;          /* frame the last good MP ended in */
	int ed_zero;            /* zero frames after an MP' */
	long b1_err;
	double fe_sum;
	long fe_n;

	int ev_ri;              /* Ri there */
	int ev_ribar;           /* its reversal: TRN2d follows */
	int ev_mp;              /* MP frames in */
	int ev_mpack;           /* MP' */
	int ev_ed;
	int ev_b1d;             /* B1d's frames in */
	V90Mp mp;

	void init(int alaw_, int uinfo_);
	/* the DIL to measure (N 0: none) */
	void dil_set(const V90Dil &d);
	void dil_analyse();
	/* the expected level of each Ucode in each frame interval (0: not
	   to be used): the DIL's, or without one lin[]; the noise (rms) on
	   the data mode decisions */
	void levels(float lv[6][128]) const;
	float noise() const;
	/* phase 4 on CPt's constellations (TRN2d, MP, Ed) and CP's (B1d,
	   data): -1 when they are not usable */
	int phase4(const V90Cp &cpt, const V90Cp &cp);
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
	void adapt(long kk, float e);
	void ri_frame(const int *s);
	void frame();
	void mp_bit(int bit);
};

} // namespace dsp

#endif
