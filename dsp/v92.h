/*
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

#include "v90.h"

namespace dsp {

/* INFO1a when the analogue modem asks for PCM upstream (Table 18/V.92) */
struct V92Info1a {
	int sections;           /* 3: z1, p1, p2 and z2 */
	int ltot, lmax;         /* coefficients at most: in all, per section */
	int md;                 /* MD, in 276 symbols */
	int uinfo;

	void parse(const unsigned char *b);
};

/* CPd (Table 30/V.92): the analogue modem's data mode parameters */
#define V92_LC_MAX      128
#define V92_PMAX        64      /* taps of a precoder or prefilter section */
struct V92Cpd {
	int drn;                /* (drn + 17) * 8000 / 6 bit/s */
	int ack;
	int trellis;            /* 0: 16 states, 1: 32, 2: 64 */
	double g;               /* the gain after the prefilter */
	/* LZ1, LP1, LZ2, LP2 and their coefficients: z1 and z2 in Q0.15,
	   p1 and p2 in Q1.14 */
	int len[4];
	short coef[4][V92_PMAX];
	int M[12];
	int ncon;
	int con[6];             /* constellation of intervals i and i + 6 */
	int lc[6];              /* positive points of each constellation */
	int pts[6][V92_LC_MAX]; /* their linear values, smallest first */

	/* the bits, filled with zeros to whole frames of fill bits;
	   returns their number */
	int build(unsigned char *b, int fill) const;
};

/* SUVd (Table 31/V.92) */
int  v92_suv_build(unsigned char *b, int ack, int fill);

/* the analogue modem's DIL descriptor in Ja (Table 20/V.92): V.90's,
   with its data signalling rate mask before the CRC; the length up to
   the end of the CRC once 50 bits are in, -1 before */
int  v92_dil_length(const unsigned char *b, int n);

/* CPt and CPu (Table 23/V.92) into V.90's CP; -1 for a bad CRC */
int  v92_cp_parse(V90Cp *cp, const unsigned char *b, int len);

/* frames of 17 bit blocks after a sync of 17 ones, in the decoded bits
   of the analogue modem */
#define V92_FR_MAX      3000
enum { V92F_DIL, V92F_CP };
struct V92Frames {
	int mode;
	unsigned sr;
	unsigned char b[V92_FR_MAX];
	int n, len;

	void init(int mode_);
	/* 1 when a frame with a good CRC is in b (len bits) */
	int bit(int x);
};

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

enum v92_fmode { V92F_TRN, V92F_SEQ, V92F_ED, V92F_B1, V92F_DATA,
		 V92F_SIL };     /* silence in a renegotiation */

struct V92Up {
	int alaw;
	V92Info1a ia;
	void (*get_bits)(void *ctx, unsigned char *bits, int n);
	void (*put_bits)(void *ctx, const unsigned char *bits, int n);
	void *ctx;
	long n;                 /* samples since the start */
	int connected;

	/* transmitter */
	V90Tx tx;
	int tstep;
	long tstep_at;
	V90Cp cpt, cpu;         /* the analogue modem's CPt and CPu */
	int D;                  /* bits per data frame of ours */
	enum v92_fmode fmode;
	long fcount;
	unsigned char seq[8192];        /* SUVd or CPd (one bit each), in whole
					   frames */
	int seq_len, seq_pos, seq_ack, seq_cpd;
	int cpd_sent, ack_sent;
	long cpd_at;            /* frames after our last CPd */
	V92Cpd cpd;             /* ours */

	/* receiver */
	enum v92_rx rx;
	float buf[V92_BUF];
	/* the period 6 tones (Ru, Su) and their reversals */
	float th[32];
	cplx tbh[32];
	unsigned char tcoh[32];
	int tone_n, tone_miss, rev_n, rev_hold;
	float rev_min;
	long rev_cand, rev_at;
	Goertzel ta_g;          /* tone A */
	float ta_e;
	int ta_blocks;
	int revs;               /* reversals of Su so far */
	cplx su_ref;            /* Su's phase */
	int su_back;            /* samples of Su-bar back in Su's phase */
	/* the equalizer */
	double f[V92_EQ_F], b[V92_EQ_B];
	float dh[V92_EQ_B];     /* past decisions, newest first */
	float dring[V92_DR];    /* the decisions, by symbol */
	long t0;                /* the sample of TRN1u's first symbol (est.) */
	long eqL;               /* the sample of symbol 0's main tap */
	long sym;               /* the next symbol to equalize */
	int trained;
	float pr;               /* received power, for the step size */
	double mse;
	int md_done;            /* MD is over */
	float mse_trn;          /* after TRN1u's training (and, with the
				   precoder, its fit) */
	/* bits */
	int last_d;             /* differential decoding */
	unsigned dscr;          /* GPA descrambler */
	int order;              /* TRN2u: 0 sign bit first, 1 the other */
	unsigned dscr2[2];
	long ones2[2];
	V92Frames fr;
	int got_ja, got_cpt, got_e1u, got_suvu, got_cpu, got_ack, got_e2u;
	long frame_zero;        /* zero bits of the current data frame */
	V90Dil dil;
	int up_mask;            /* the analogue modem's rates, bit 0: 24 000 */
	long e2u_sym;
	/* rate renegotiation */
	int rn;                 /* one is going on */
	int rn_req;             /* we start one */
	int rn_sent;            /* our Rd has been asked for */
	int rn_tx_init;         /* the encoder starts on CPt again */
	int cpd_new;            /* a new CPd for it */
	long rn_trn;            /* the symbol of TRN2u's start */
	long rn_at;             /* its start (samples) */
	int renegs;
	int rn_silence;         /* SUVu asked for a silent period */
	int rn_wait_suv2;       /* in it: until SUVu with bit 32 clear */
	int rn_suv2;            /* that SUVu is in */
	int rn_rxd;             /* its TRN2u has come */
	/* fast parameter exchange (9.9/V.92): no training, the sequences
	   in the data mode modulation */
	int fpe;                /* one is going on */
	int fpe_req;            /* we start one */
	int fpe_rf;             /* our Rf has gone out (or is asked for) */
	int fpe_sw;             /* the receiver's front end is on cpd_fpe,
				   the decisions not yet */
	int fpe_tx_init;        /* the encoder starts again on CPu */
	int fpe_rx;             /* 1 RM, 2 RM', 3 SUVu to E2u, 4 FB1u,
				   5 B1u */
	int fpe_rm, fpe_fb;     /* frames of RM, of FB1u */
	long fpe_suv;           /* the decision where SUVu begins */
	long fpe_b1;            /* B1u's first symbol (data symbols) */
	long dsn, ddn;          /* data symbols in, decided */
	V92Cpd cpd_fpe;         /* the new CPd */
	V92Cpd cpd_old;         /* the decisions' until B1u */
	int fpes;
	long fpe_at;            /* our Rf asked for here (samples) */
	int fpe_none;           /* no RM came to it: the analogue modem has
				   no fast parameter exchange, renegotiations
				   instead */
	int fpe_drn;            /* test: the rate it asks for */
	long fpe_test_at;       /* test: ours, this far into data */
	int rn_drn;             /* test: the rate it asks for */
	long rn_test_at;        /* test: our own, this far into data */
	long connect_n;         /* data from here on (samples) */
	long rn_low, rn_high;   /* data symbols with too little, more than
				   enough margin */
	long rn_last;           /* the last one ended here (samples) */
	double margin_used;     /* the margin of our CPd's rate */
	/* data mode */
	int b_frozen;           /* b is the precoder's (in CPd) */
	int trellis_on;         /* Viterbi decoding of the trellis code */
	float tu[4];            /* the symbols of a trellis frame, in grid
				   units (2 eta + 1) */
	float vmetric[64];
	unsigned char vprev[V92_VT][64];
	int vpa[V92_VT][64], vpb[V92_VT][64];
	int vt, vn;
	int vi;                 /* data frame interval of the next decided
				   symbol */
	int dk[12];
	int dprev;              /* the modulus encoder's sign */
	long dframes, derrs, dfar;
	int b1_bad;
	unsigned char b1z[48];
	double dmse;
	/* the prefilter design (DSP_V92_PREFILTER=0: the receiver's feed
	   forward taps instead) */
	int pf_ok;
	int pf;                 /* our CPd has the prefilter */
	int pf_on;              /* data mode: the receiver reads the levels */
	double ch[V92_CH];      /* the channel from TRN2u */
	double pf_w[V92_PF_W];  /* G times the prefilter, as sent */
	double pf_b[V92_PF_B];  /* the precoder's taps, as sent */
	short pf_z2[V92_PF_W];  /* CPd's z2, Q0.15 */
	short pf_p1[V92_PF_B];  /* CPd's p1, Q1.14 */
	double pf_g;            /* CPd's G */
	int pf_d;               /* decision delay, symbols */
	double pf_cd;           /* main tap of channel and prefilter */
	double pf_noise;        /* rms beyond G.711's quantization */
	long pf_s0;             /* the first symbol through the prefilter */
	long pf_eqL;            /* eqL of the plain channel */
	short pf_lev[128];      /* the G.711 levels, ascending */
	short pf_gap[20];       /* by drn: the points' least gap, 0 none */
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
	long pf_rst;
	/* with the prefilter the analogue modem takes up the new
	   constellation of a fast parameter exchange once it has our CPd
	   (SUVu', E2u and FB1u on it): the grid mse on the old one and on
	   the new, and whether we did too */
	double fpe_eo, fpe_en;
	int fpe_early;
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
	long sl_n;
	double sl_gm;           /* the grid mse, over a second */
	double sl_base;         /* it, 3 s into the design */
	double sl_g1, sl_tau1, sl_x1;   /* the fit 2 s into it, */
	long sl_t1;             /* at that symbol */
	int fpe_slip;           /* this fast parameter exchange: the design
				   for the slip, at the same rate */
	int pf_new;             /* its prefilter and precoder: in at B1u */
	long pf_new_at;         /* (that symbol, once known) */
	int slips;
	int slip_ok;            /* DSP_V92_SLIP=0: no design for the slip */

	void init(int alaw_, const V92Info1a *ia_, const long *fclock,
		  long fbase);
	int sample(int x, int *y);
	/* the echo canceller may train: we send, the analogue modem is
	   silent (Sd to Jd) */
	int ec_train() const
	{
		return rx == V92R_QUIET && tstep >= 2 &&
			tx.seg >= V90_SD && tx.seg <= V90_JD;
	}

	/* transmitter */
	int tx_step();
	void make_seq(int cpd_, int ack);
	void design();
	int precoder_fit(double gamma, double *bg, double *fg, double *e2);
	int ch_fit(double *e2, double *q2);
	int pf_filter(double n0, double *resid, int dfix = -1);
	int pf_quant();
	void pf_rx_take();
	void pf_slip_sample();
	int pf_slip_fit(double *gain, double *tau, double *expl);
	int pf_slip_design();
	double pf_qslope(double v) const;
	int pf_points(int gap, int lc, short *pts);
	int pf_sim(int M, const short *pts, int lc, int nsym, double *pw,
		   double *mseg);
	int pf_rate(int drn);
	int pf_design(const char *fix);
	int pf_cpd(V92Cpd *c, int drn);
	void pf_margin();
	void pf_reset(long r);
	double pf_sample(long k);
	float pf_grid(const V92Cpd *c, float r) const;
	float pf_point(const V92Cpd *c, int eta) const;
	/* receiver */
	void rx_sample(float x);
	void tone_sample(float x);
	int train(long start, int search_lo, int search_hi);
	float equalize(long s);
	void adapt(long s, float err, float mu);
	void symbol(float y);
	void bit2(int d);
	void bits4(int s, int m);
	void frame_bits(int bit);
	void data_symbol(float y);
	void trellis_frame();
	void decided(int eta);
	void rn_rx_start();
	void rn_tx_start();
	void fpe_tx_start();
	void fpe_design(int drn);
	int fpe_frame(const V92Cpd *c);
};

} // namespace dsp

#endif
