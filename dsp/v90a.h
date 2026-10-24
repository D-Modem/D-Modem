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


#include "dm_types.h"
#include "v90.h"
#include "v92.h"

namespace dsp {

/* Jd (Table 13/V.90), the digital modem's capabilities */
struct V90Jd {
	u32 rate_mask;          /* bit 0: 28 000 bit/s ... bit 22: 56 000 */
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
		    double cpt_power, u32 jd_rates);

/* the line at 9600 Hz (or 8000) to 16 000 Hz, two samples a PCM symbol,
   for the PCM receiver's T/2 equaliser: a windowed sinc cut at the
   line's Nyquist (4800 Hz) keeps the whole band, whatever the phase of
   the symbols against our samples (at 8000 Hz the band past 4000 Hz
   has folded onto itself in the codec, and half a symbol off it
   cancels: no equaliser brings it back, and the constellations are
   designed on what is left).  The output's time moves with shift():
   the timing loop keeps two samples on each of the network's
   symbols */
#define V90_INTERP_T    64      /* taps each side, at the line's rate */
#define V90_INTERP_P    256     /* phases tabulated, interpolated
				   between */
struct V90Interp {
	float g[V90_INTERP_P + 1][2 * V90_INTERP_T];
	float hist[4 * V90_INTERP_T];
	s32 nin;                /* samples in */
	double t;               /* the next output's time, in input
				   samples */
	double step;            /* input samples an output: 0.6, or 0.5 */

	/* the line's rate, 9600 or 8000 */
	void init(int rate);
	/* one sample of the line in; how many out at 16 000 Hz (0 to 3) */
	int run(float x, float *out);
	/* the outputs from now on dt input samples later */
	void shift(double dt) { t += dt; }
};

#define V90RX_HIST      2048    /* samples kept at 16 000 Hz (a power of
				   2) */
#define V90RX_HIST8     1024    /* Sd's: every other one, 8000 Hz */
/* the equaliser's taps, T/2 spaced (64 symbols): 86 to 88 dB at any
   phase on a raised cosine line, where a T-spaced one's 90 dB falls to
   21 half a symbol off */
#define V90RX_TAPS      128
#define V90RX_EQ_D      (V90RX_TAPS / 2)        /* symbol k's sample b + 2 k
						   at this tap */
#define V90RX_LS        1020    /* TRN1d symbols the equaliser is first
				   solved on (least squares) */
#define V90RX_DIL_ROUNDS 2      /* the DIL's rounds measured */
#define V90RX_Q         1024    /* data mode bits held for the caller */
#define V90RX_SEQ       8192    /* MP, or V.92's SUVd or CPd, bits */

struct V90Rx {
	enum Stage {
		WAIT_SD,        /* until Sd */
		SD,             /* in Sd, until its reversal */
		TRAIN,          /* the reversal seen: S-bar d, TRN1d */
		DECIDE,         /* trained: decisions, Jd and Jd' */
		RI,             /* phase 4: Ri, then its reversal */
		FRAMES,         /* TRN2d, MP, Ed, B1d: data frames */
		RD,             /* a rate renegotiation: Rd-bar's 24
				   symbols, then FRAMES again */
	};

	int alaw, uinfo;
	int lin[128];           /* Ucode -> linear value, positive */
	enum Stage stage;

	V90Interp ip;           /* the line at 16 000 Hz */
	float x[V90RX_HIST];    /* its samples */
	s32 n;                  /* samples in */
	float x8[V90RX_HIST8];  /* every other one, for Sd */
	s32 n8;

	/* Sd: running sums over the last SD_WIN samples of x(n)^2,
	   x(n) x(n - 6), x(n) x(n - 3), and over the last 6 of the first
	   two (the reversal) */
	double e24, a6_24, a3_24, e6, a6_6;
	int sd_run;             /* samples Sd has been there */
	int sd_dip;             /* the 24 sample sums say it is reversing */
	double dip;             /* the reversal: its least a6_6 / e6 */
	s32 dip_at;

	/* symbol k (0: the first of S-bar d) is around sample b + 2 k */
	s32 b;
	s32 k;                  /* the next symbol to decide */
	int refined;
	float c[V90RX_TAPS];
	/* least squares: the normal equations, R's Cholesky factor (some
	   250 KB: the pump is on the heap) */
	double R[V90RX_TAPS][V90RX_TAPS], p[V90RX_TAPS];
	double L[V90RX_TAPS][V90RX_TAPS];
	u32 trn_scr;            /* TRN1d's signs: GPC on ones from zero */
	float level;            /* lin[uinfo] */
	/* UINFO's level at each symbol position mod 6, as the equaliser
	   gives it (a robbed bit lowers it in its interval): its targets
	   in training, from the loudest position's level; the positions'
	   mean magnitudes of late */
	float tlev[6];
	double tmag[6];
	double err_sum;
	s32 err_n;
	float mse;              /* of the decisions, to the points' power */
	/* the timing loop, from the equaliser's error times its slope
	   (its output a sample either side): the frequency term (16 kHz
	   samples a symbol) */
	int t_on;
	s32 t_n;                /* symbols it has followed */
	double t_freq, t_pow;   /* t_pow: the targets' power */
	double t_phase;         /* the phase it has moved (16 kHz samples) */
	double t_sx, t_sy, t_sxx, t_sxy;        /* the line fitted to it */
	s32 t_sn;
	/* the second solution (ls2_n symbols in), then over NOISE_N
	   symbols (before our S: well before the DIL, whose loud codes
	   reach into the equaliser's span) UINFO's magnitudes by frame
	   interval (their spread about each interval's own mean is the
	   noise: a robbed bit or the network's gain moves the mean, not
	   the decisions) */
	int ls2_n;
	double usum[6], usq[6];
	s32 un[6];

	/* Jd: the decisions' signs differentially decoded and
	   descrambled */
	int last_sign;
	u32 dscr;
	unsigned char jb[72];   /* the last 72 bits, the newest at jpos - 1 */
	int jpos;
	s32 jbits;
	s32 jd_end;             /* bit count at the end of the last Jd */
	int zeros;              /* zeros since then */

	/* events (cleared by the caller) */
	int ev_sd;              /* Sd there */
	int ev_sdbar;           /* its reversal */
	int ev_trained;
	int ev_ready;           /* solved again on the locked timing, the
				   noise measured: our S may go */
	int ev_jd;              /* Jd frames received */
	int ev_jdp;             /* Jd' after them */
	int ev_dil;             /* the DIL's rounds measured */
	V90Jd jd;
	/* V.92 (v92): Jd's bit 47 makes it Jp (Table 22/V.92), whose
	   fraction of a symbol extends our Su-bar; its Jp' follows (as
	   Jd' does in V.90) */
	int v92;
	int ev_jp;
	s32 jp_end;
	double jp_frac;
	int jp_con, jp_con_rn;  /* TRN2u and CPu on 8 points: in training,
				   in renegotiation */

	/* the DIL our Ja asked for (8.4.1/V.90): it follows Jd' (in data
	   frame interval 0), round after round; the decisions' values
	   (the equaliser still) by frame interval and Ucode, the sign
	   SP's.  Its first symbol, a round's length, where in it we are */
	V90Dil dil;
	int dil_on;
	s32 dil_start, dil_len;
	int dil_seg, dil_k;
	double dsum[6][128], dsq[6][128];
	s32 dcnt[6][128];
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
	s32 ri_seen, ribar_at, frames_at;
	V90Con con_trn, con_data;
	V90Con con_next;        /* data_cp_next()'s, if have_next */
	int have_next;
	V90Con *con;
	float fy[6];
	s32 frames, frames_b1;
	int flast;
	u32 fscr;
	V90Enc ref;
	int ref_lin[128];
	float lv4[6][128];      /* levels(), at phase4() */
	int rs[6];              /* Ri: the frame's signs */
	s32 trn_sym_err, trn_sym;
	unsigned char mpb[V90RX_SEQ];
	int mp_n, mp_len;
	u32 msr;
	s32 mp_frame;           /* frame the last good MP ended in */
	int ed_zero;            /* zero frames after an MP' */
	s32 b1_err;
	double fe_sum;
	s32 fe_n;

	int ev_ri;              /* Ri there */
	int ev_ribar;           /* its reversal: TRN2d follows */
	int ev_trn2;            /* TRN2d's known frames in: trn2_noise, the
				   rms error on them */
	float trn2_noise;
	float t_gain[6];        /* each interval's gain on the DIL's levels */
	double t2_yr[6], t2_rr[6], t2_yy[6];
	s32 t2_n;
	int ev_mp;              /* MP frames in */
	int ev_mpack;           /* MP' (V.92: SUVd' or CPd') */
	/* V.92's phase 4 (v92): SUVd and CPd in MP's place */
	int ev_suvd, ev_cpd;
	V92Cpd cpd;
	/* V.92's fast parameter exchange (9.9/V.92): Rf (+ + - - + + of
	   each interval's loudest data point, then the opposite: a 12
	   symbol pattern) frames in a row, Rf-bar (the pattern a frame on,
	   4 frames); then SUVd and CPd in the data mode modulation, from
	   cleared descrambler and signs, until Ed, then B1d */
	int rf_run, rf_last, rfbar_n;
	int ev_rf, ev_rfbar;
	int fpe_on;             /* the frames are SUVd, CPd, Ed */
	int ev_ed;
	int ev_b1d;             /* B1d's frames in */
	/* a rate renegotiation (9.6/V.90): Rd (+ + + - - - of each
	   interval's loudest data point, either polarity) frames in a row,
	   its polarity, Rd-bar; then TRN2d (on the decisions: it may be
	   left out), MP, Ed and B1d as in phase 4 */
	int rd_run, rd_pol;
	int ev_rd, ev_rdbar;
	int renegs;             /* phase 4 done again so many times */
	V90Cp cpt_s;            /* CPt (TRN2d and MP's constellations) */
	/* data mode: the bits of the frames after B1d (take()), the
	   signal's power (lost, it is gone), the decisions' error */
	unsigned char q[V90RX_Q];
	int qh, qn;
	float dpow, de_mse;
	V90Mp mp;

	void init(int alaw_, int uinfo_, int rate = 9600);
	/* the DIL to measure (N 0: none) */
	void dil_set(const V90Dil &d);
	void dil_analyse();
	/* the expected level of each Ucode in each frame interval (0: not
	   to be used): the DIL's, or without one lin[] (after TRN2d by the
	   gain it showed in each interval); the noise (rms) on the data
	   mode decisions */
	void levels(float lv[6][128]) const;
	float noise() const;
	/* phase 4 on CPt's constellations (TRN2d, MP, Ed): -1 when they
	   are not usable */
	int phase4(const V90Cp &cpt);
	/* CP's for B1d and data (before Ed): -1 when not usable */
	int data_cp(const V90Cp &cp);
	/* in the data mode, our fast parameter exchange's CPu (V.92): its
	   constellations for B1d and data after the exchange's Ed (the
	   frames before it stay on those they are on); -1 when not
	   usable */
	int data_cp_next(const V90Cp &cp);
	/* our own rate renegotiation (9.6.2.1/V.90): Rd next, whatever
	   the frames decode to until it comes */
	void reneg_wait()
	{
		ev_rd = ev_rdbar = rd_run = 0;
		ev_trn2 = ev_mp = ev_mpack = ev_ed = ev_b1d = 0;
		ev_suvd = ev_cpd = 0;
		have_next = 0;
	}
	/* our own fast parameter exchange (9.9.2.1/V.92): Rf next, the
	   frames data until it comes */
	void fpe_wait()
	{
		ev_suvd = ev_cpd = ev_mpack = ev_ed = ev_b1d = 0;
		have_next = 0;
	}
	/* one sample of the line (9600 or 8000 Hz: init()'s rate) */
	void line(float v);
	/* one sample at 16 000 Hz */
	void sample(float v);
	/* data mode: up to n of its bits into b, oldest first (how many) */
	int take(unsigned char *b, int n);
	/* its decisions' mean square error of late (16 bit scale) */
	float data_mse() const { return de_mse; }
	/* the codec's clock against the network's, parts per million */
	double ppm() const { return t_freq / 2 * 1e6; }

private:
	/* the sample i of the history */
	float at(s32 i) const { return x[i & (V90RX_HIST - 1)]; }
	/* the sample i of the 8 kHz history */
	float at8(s32 i) const { return x8[i & (V90RX_HIST8 - 1)]; }
	/* tap i's input for symbol kk, d samples later */
	float xin(s32 kk, int i, int d = 0) const
	{
		return at(b + 2 * kk + V90RX_EQ_D + d - i);
	}
	void timing(s32 kk, float y, float a);
	void timing_free();
	void timing_step(double dt);
	void sd_detect(float v);
	void refine();
	void solve();
	float equalize(s32 kk, int d = 0) const;
	void decide(float y);
	void jd_bit(int bit);
	void adapt(s32 kk, float e);
	void ri_frame(const int *s);
	void rd_frame(const int *s, const int *lab);
	void rf_frame(const int *s, const int *lab);
	void reneg_frames();
	void frame();
	void mp_bit(int bit);
};

} // namespace dsp

#endif
