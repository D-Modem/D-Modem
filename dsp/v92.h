/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v92.h - ITU-T V.92 PCM upstream, the digital modem: phases 3 and 4
 *            and the data mode, after an INFO1a that asks for PCM upstream
 *
 *    The analogue modem sends PAM symbols at 8000 symbol/s, one per G.711
 *    sample of the network; we send PCM codes as in V.90.
 *
 */

#ifndef V92_H
#define V92_H


#include "dm_types.h"
#include "v90.h"

namespace dsp {

/* INFO1a when the analogue modem asks for PCM upstream (Table 18/V.92) */
struct V92Info1a {
	s16 sections;           /* 3: z1, p1, p2 and z2 */
	s16 ltot, lmax;         /* coefficients at most: in all, per section */
	s16 md;                 /* MD, in 276 symbols */
	s16 uinfo;

	/* from INFO1a's bits, one a byte */
	void parse(const unsigned char *b);
};

/* CPd (Table 30/V.92): the analogue modem's data mode parameters */
#define V92_LC_MAX      128
/* the fastest upstream rate we design for: drn 14, 41333 bit/s, K = 62
   bits a data frame.  The original library's modulus encoder works in
   signed 64 bits and sends garbage from K = 64 on (its own encoder run
   against our decoder: no frame differs up to drn 14, 80 % from drn 15),
   and the HSF modem fails there as well, though both offer every rate in
   Ja (DSP_V92_UP_DRN still asks for any) */
#define V92_DRN_MAX     14
#define V92_PMAX        64      /* taps of a precoder or prefilter section */
struct V92Cpd {
	s16 drn;                /* (drn + 17) * 8000 / 6 bit/s */
	s16 ack;
	s16 trellis;            /* 0: 16 states, 1: 32, 2: 64 */
	double g;               /* the gain after the prefilter */
	/* LZ1, LP1, LZ2, LP2 and their coefficients: z1 and z2 in Q0.15,
	   p1 and p2 in Q1.14 */
	s16 len[4];
	s16 coef[4][V92_PMAX];
	s16 M[12];
	s16 ncon;
	s16 con[6];             /* constellation of intervals i and i + 6 */
	s16 lc[6];              /* positive points of each constellation */
	s32 pts[6][V92_LC_MAX]; /* their linear values, smallest first */

	s16 e2u_extend;         /* bit 29: E2u a symbol longer */

	/* the bits, filled with zeros to whole frames of fill bits;
	   returns their number */
	s16 build(unsigned char *b, s16 fill) const;
	/* the analogue modem's side: from the bits of a CPd of len bits
	   (to its CRC's end, v92_cpd_length()); -1 for a bad CRC or one
	   it can't take (more taps or points than we keep) */
	s16 parse(const unsigned char *b, s16 nb);
};

/* a CPd's length up to its CRC's end, from its first n bits (its
   parts' lengths are in them); 0 while more are needed */
s32 v92_cpd_length(const unsigned char *b, s16 n);

/* SUVd (Table 31/V.92) */
s16  v92_suv_build(unsigned char *b, s16 ack, s16 fill);

/* the analogue modem's DIL descriptor in Ja (Table 20/V.92): V.90's,
   with its data signalling rate mask before the CRC; the length up to
   the end of the CRC once 50 bits are in, -1 before */
s16  v92_dil_length(const unsigned char *b, s16 n);

/* CPt and CPu (Table 23/V.92) into V.90's CP; -1 for a bad CRC */
s16  v92_cp_parse(V90Cp *cp, const unsigned char *b, s16 len);

/* the analogue modem's side: its DIL descriptor for Ja (Table 20/V.92)
   asking for d, up_mask the PCM upstream rates (bit 0: 24 000 bit/s),
   and CPt (Table 23/V.92: cp's fields as V.90's, cp.cp 0), one bit a
   byte, filled with zeros to a multiple of 12 bits; return the count */
s16  v92_dil_build(unsigned char *b, const V90Dil &d,
		     u32 up_mask);
s16  v92_cpt_build(unsigned char *b, const V90Cp &cp);
/* and CPu (type 1, cp.drn for (drn + 20) * 8000 / 6 bit/s) and SUVu
   (Table 27/V.92: no wait for CPu, no level measured, no silent
   period), filled to a multiple of fill bits */
s16  v92_cpu_build(unsigned char *b, const V90Cp &cp, s16 fill);
s16  v92_suvu_build(unsigned char *b, s16 ack, s16 fill);
/* CPus (Table 24/V.92): the digital modem's rate drn and parameters as
   they are */
s16  v92_cpus_build(unsigned char *b, s16 drn, s16 ack, s16 fill);

/* frames of 17 bit blocks after a sync of 17 ones, in the decoded bits
   of the analogue modem */
/* events of V92Up::sample() */
enum {
	V92_NONE,
	V92_CONNECT,
	V92_FAIL,
	V92_RETRAIN,            /* the remote's tone A */
	V92_RENEG,              /* data again after a rate renegotiation */
};

#define V92_BUF         16384   /* received samples kept */
#define V92_EQ_F        32      /* feed-forward taps */
#define V92_EQ_PRE      8       /* of them, ahead of the main tap */
#define V92_EQ_B        16      /* feedback taps */
#define V92_EQ_N        (V92_EQ_F + V92_EQ_B)
#define V92_VT          24      /* Viterbi decoding delay, trellis frames */
#define V92_DR          4096    /* decisions kept */
/* the prefilter design: the channel estimate's taps ahead of and after
   the main one, the prefilter's and the precoder's taps */
#define V92_CH_PRE      24
#define V92_CH_POST     40
#define V92_CH          (V92_CH_PRE + 1 + V92_CH_POST)
#define V92_PF_W        64
#define V92_PF_B        32
#define V92_PF_FIT      3584    /* TRN2u symbols for the channel */
#define V92_SLIP_MIN    0.001   /* the analogue modem's timing slip (in
				   samples) a new prefilter design is for */

enum v92_rx {
	V92R_RU,        /* Ru and its reversal */
	V92R_MD,
	V92R_TRN1,      /* TRN1u: the equalizer is trained */
	V92R_JA,
	V92R_QUIET,     /* the analogue modem waits for Jd */
	V92R_SU,        /* Su, Su-bar, Su: the phase */
	V92R_SUBAR,
	V92R_SU2,       /* Su until Jp, then Su-bar */
	V92R_TRN1B,     /* the second TRN1u: the data frame alignment */
	V92R_CPT,       /* TRN1u, CPt, E1u */
	V92R_TRN2,      /* TRN2u, SUVu, CPu, E2u */
	V92R_DATA,      /* B1u and data */
	/* rate renegotiation (9.8/V.92) */
	V92R_RN_RU,     /* Ru: its reversal */
	V92R_RN_WAIT,   /* Ru-bar, until TRN2u */
};

/* (PcmFrames: our phase 4 frames, SUVd and CPd the sequences) */
struct V92Up : PcmFrames<V92Up> {
	s16 alaw;
	V92Info1a ia;
	/* the analogue modem's negative data points a step out (the ESS
	   modem's), and the first symbols' grid mse that tells it */
	s16 neg_step, ns_n;
	float ns_odd, ns_even;
	/* the analogue modem's precoder on the grid unit below (the ESS
	   modem's), the first symbols' grid mse with and without, and its
	   outputs rebuilt by data symbol */
	s16 drn_cap;            /* the rate's most (a renegotiation down
				   for the margin), 0 none */
	s16 pq_on;
	float pq_eq, pq_ed;
	double pq_x[V92_PMAX];
	/* without the prefilter, with precoding: the analogue modem's
	   precoder outputs rebuilt from the decoded points, by data
	   symbol (in the equalizer's units), and the first of its run
	   (0, or a fast parameter exchange's B1u), and whether they are
	   still to be had (not run away) */
	double pq_xr[V92_PMAX];
	s32 pq_xs0;
	s16 pq_xok;
	CoreBits link;          /* the data */
	s32 n;                  /* samples since the start */
	s16 connected;

	/* transmitter */
	V90Tx tx;
	s16 tstep;
	s32 tstep_at;
	V90Cp cpt, cpu;         /* the analogue modem's CPt and CPu */
	s16 seq_cpd;            /* the sequence is CPd */
	s16 cpd_sent;
	s32 cpd_at;             /* frames after our last CPd */
	V92Cpd cpd;             /* ours */

	/* receiver */
	enum v92_rx rx;
	float buf[V92_BUF];
	/* the period 6 tones (Ru, Su) and their reversals */
	float th[32];
	cplx tbh[32];
	unsigned char tcoh[32];
	s32 tone_n, tone_miss;
	s16 rev_n, rev_hold;
	float rev_min;
	s32 rev_cand, rev_at;
	Goertzel ta_g;          /* tone A */
	float ta_e;
	s16 ta_blocks;
	s16 revs;               /* reversals of Su so far */
	cplx su_ref;            /* Su's phase */
	s16 su_back;            /* samples of Su-bar back in Su's phase */
	/* the equalizer */
	double f[V92_EQ_F], b[V92_EQ_B];
	float dh[V92_EQ_B];     /* past decisions, newest first */
	float dring[V92_DR];    /* the decisions, by symbol */
	s32 t0;                 /* the sample of TRN1u's first symbol (est.) */
	s32 eqL;                /* the sample of symbol 0's main tap */
	s32 sym;                /* the next symbol to equalize */
	s16 trained;
	float pr;               /* received power, for the step size */
	double mse;
	s16 md_done;            /* MD is over */
	float mse_trn;          /* after TRN1u's training (and, with the
				   precoder, its fit) */
	/* bits */
	s16 last_d;             /* differential decoding */
	u32 dscr;               /* GPA descrambler */
	s16 order;              /* TRN2u: 0 sign bit first, 1 the other */
	u32 dscr2[2];
	s32 ones2[2];
	SyncFrames fr;
	s16 got_ja, got_cpt, got_e1u, got_suvu, got_cpu, got_ack, got_e2u;
	s32 frame_zero;         /* zero bits of the current data frame */
	s32 cpt_sym;            /* the symbol the last CPt ended on */
	s32 seq_sym;            /* and the last SUVu or CPu */
	V90Dil dil;
	u32 up_mask;            /* the analogue modem's rates, bit 0: 24 000 */
	s32 e2u_sym;
	/* rate renegotiation */
	s16 rn;                 /* one is going on */
	s16 rn_req;             /* we start one */
	s16 rn_sent;            /* our Rd has been asked for */
	s16 rn_tx_init;         /* the encoder starts on CPt again */
	s16 cpd_new;            /* a new CPd for it */
	s32 rn_trn;             /* the symbol of TRN2u's start */
	s32 rn_at;              /* its start (samples) */
	s16 renegs;
	s16 rn_silence;         /* SUVu asked for a silent period */
	s16 rn_wait_suv2;       /* in it: until SUVu with bit 32 clear */
	s16 rn_suv2;            /* that SUVu is in */
	s16 rn_rxd;             /* its TRN2u has come */
	/* fast parameter exchange (9.9/V.92): no training, the sequences
	   in the data mode modulation */
	s16 fpe;                /* one is going on */
	s16 fpe_req;            /* we start one */
	s16 fpe_rf;             /* our Rf has gone out (or is asked for) */
	s16 fpe_sw;             /* the receiver's front end is on cpd_fpe,
				   the decisions not yet */
	s16 fpe_tx_init;        /* the encoder starts again on CPu */
	s16 fpe_rx;             /* 1 RM, 2 RM', 3 SUVu to E2u, 4 FB1u,
				   5 B1u */
	s16 fpe_rm, fpe_fb;     /* frames of RM, of FB1u */
	s32 fpe_suv;            /* the decision where SUVu begins */
	s32 fpe_b1;             /* B1u's first symbol (data symbols) */
	s32 dsn, ddn;           /* data symbols in, decided */
	V92Cpd cpd_fpe;         /* the new CPd */
	V92Cpd cpd_old;         /* the decisions' until B1u */
	s16 fpes;
	s32 fpe_at;             /* our Rf asked for here (samples) */
	s16 fpe_none;           /* no RM came to it: the analogue modem has
				   no fast parameter exchange, renegotiations
				   instead */
	s16 fpe_drn;            /* test: the rate it asks for */
	s32 fpe_test_at;        /* test: ours, this far into data */
	s16 rn_drn;             /* test: the rate it asks for */
	s32 rn_test_at;         /* test: our own, this far into data */
	s32 connect_n;          /* data from here on (samples) */
	s32 rn_low, rn_high; /* data symbols with too little, more than
				   enough margin */
	s32 rn_last;            /* the last one ended here (samples) */
	double margin_used;     /* the margin of our CPd's rate */
	/* data mode */
	s16 b_frozen;           /* b is the precoder's (in CPd) */
	s16 trellis_on;         /* Viterbi decoding of the trellis code */
	float tu[4];            /* the symbols of a trellis frame, in grid
				   units (2 eta + 1) */
	Viterbi4d<V92_VT> vit;
	s16 vi;                 /* data frame interval of the next decided
				   symbol */
	s16 dk[12];
	s16 dprev;              /* the modulus encoder's sign */
	s32 dframes, derrs, dfar;
	s16 b1_bad;
	unsigned char b1z[48];
	double dmse;
	/* the prefilter design (DSP_V92_PREFILTER=0: the receiver's feed
	   forward taps instead) */
	s16 pf_ok;
	s16 plain;              /* no prefilter nor trellis decoding (a
				   retry after one with the prefilter
				   failed) */
	s16 pf;                 /* our CPd has the prefilter */
	s16 pf_on;              /* data mode: the receiver reads the levels */
	double ch[V92_CH];      /* the channel from TRN2u */
	double pf_w[V92_PF_W];  /* G times the prefilter, as sent */
	double pf_b[V92_PF_B];  /* the precoder's taps, as sent */
	s16 pf_z2[V92_PF_W]; /* CPd's z2, Q0.15 */
	s16 pf_p1[V92_PF_B]; /* CPd's p1, Q1.14 */
	double pf_g;            /* CPd's G */
	s16 pf_d;               /* decision delay, symbols */
	double pf_cd;           /* main tap of channel and prefilter */
	double pf_noise;        /* rms beyond G.711's quantization */
	double pf_echo;         /* rms of our echo before the network codec
				   (the echo canceller's, taken away after
				   it) */
	double pf_echo_res;     /* rms of what the canceller leaves of it */
	s32 pf_s0;              /* the first symbol through the prefilter */
	s32 pf_eqL;             /* eqL of the plain channel */
	s16 pf_lev[128];        /* the G.711 levels, ascending */
	s16 pf_gap[20];         /* by drn: the points' least gap, 0 none */
	double pf_mseg[20];     /* by drn: the simulated mse, grid units */
	double dmse_g;          /* data mode mse, grid units */
	/* with the prefilter, a few receiver taps around the delay: they
	   follow the analogue modem's timing as it wanders */
#define V92_PF_F        9
	double pf_f[V92_PF_F];
	/* the precoder's outputs rebuilt from the decisions, by symbol,
	   and where the analogue modem's transmitter starts anew (in a
	   fast parameter exchange) */
#define V92_PF_XR       256
	double pf_xr[V92_PF_XR];
	s32 pf_rst;
	/* with the prefilter the analogue modem takes up the new
	   constellation of a fast parameter exchange once it has our CPd
	   (SUVu', E2u and FB1u on it): the grid mse on the old one and on
	   the new, and whether we did too */
	double fpe_eo, fpe_en;
	s16 fpe_early;
	/* the analogue modem's timing slip: the receiver's copies of the
	   prefilter, the precoder and the main tap (a fast parameter
	   exchange for the slip brings new ones in at B1u), the line
	   signal rebuilt from the decisions, the channel's derivative, and
	   the residual's least squares fit to the channel's output (gain)
	   and to its derivative's (the slip), forgetting over a second */
	double pf_rw[V92_PF_W], pf_rb[V92_PF_B], pf_rcd;
	double pf_ln[V92_PF_XR];
	double pf_dch[V92_CH];
	double sl_uu, sl_ud, sl_dd, sl_ue, sl_de, sl_ee;
	s32 sl_n;
	double sl_gm;           /* the grid mse, over a second */
	double sl_base;         /* it, 3 s into the design */
	double sl_g1, sl_tau1, sl_x1;   /* the fit 2 s into it, */
	s32 sl_t1;              /* at that symbol */
	s16 fpe_slip;           /* this fast parameter exchange: the design
				   for the slip, at the same rate */
	s16 pf_new;             /* its prefilter and precoder: in at B1u */
	s32 pf_new_at;          /* (that symbol, once known) */
	s16 slips;
	s16 slip_ok;            /* DSP_V92_SLIP=0: no design for the slip */

	/* the start after INFO1a; fclock and fbase as in V90Tx */
	void init(s16 alaw_, const V92Info1a *ia_, const s32 *fclock,
		  s32 fbase);
	/* a network sample in (x) and out (*y), linear; a V92_ event */
	s16 sample(s16 x, s16 *y);
	/* the echo canceller may train: we send, the analogue modem is
	   silent (Sd to Jd) */
	s16 ec_train() const
	{
		return rx == V92R_QUIET && tstep >= 2 &&
			tx.seg >= V90_SD && tx.seg <= V90_JD;
	}

	/* transmitter */
	s16 tx_step();
	void make_seq(s16 cpd_, s16 ack);
	void fs_reinit(V90Tx *t);
	const V90Cp *fs_data_cp();
	void fs_seq_next(s16 first);
	void fs_seq_frame();
	bool fs_acked();
	void fs_to_ed();
	bool fs_silence();
	void fs_data(unsigned char *b, s16 n);
	void design();
	void fit_sections(V92Cpd *c) const;
	s16 precoder_fit(double gamma, double *bg, double *fg, double *e2);
	s16 ch_fit(double *e2, double *q2);
	s16 pf_filter(double n0, double *resid, s16 dfix = -1);
	s16 pf_quant();
	void pf_rx_take();
	void pf_slip_sample();
	s16 pf_slip_fit(double *gain, double *tau, double *expl);
	s16 pf_slip_design();
	double pf_qslope(double v) const;
	s16 pf_points(s16 gap, s16 lc, s16 *pts);
	s16 pf_sim(s16 M, const s16 *pts, s16 lc, s16 nsym, double *pw,
		   double *mseg);
	s16 pf_rate(s16 drn);
	s16 pf_design(const char *fix);
	s16 pf_cpd(V92Cpd *c, s16 drn);
	void pf_margin();
	void pf_reset(s32 r);
	double pf_sample(s32 k);
	float pf_grid(const V92Cpd *c, float r) const;
	float pf_point(const V92Cpd *c, s16 eta) const;
	/* receiver */
	void rx_sample(float x);
	void tone_sample(float x);
	s16 train(s32 start, s16 search_lo, s16 search_hi);
	float equalize(s32 s);
	void adapt(s32 s, float err, float mu);
	void symbol(float y);
	void bit2(s16 d);
	void bits4(s16 s, s16 m);
	void frame_bits(s16 bit);
	void data_symbol(float y);
	void trellis_frame();
	void decided(s16 eta);
	void rn_rx_start();
	void rn_tx_start();
	void fpe_tx_start();
	void fpe_design(s16 drn, bool up = false);
	s16 fpe_frame(const V92Cpd *c);
};

} // namespace dsp

#endif
