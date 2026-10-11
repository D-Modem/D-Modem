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
	unsigned char seq[3072];        /* SUVd or CPd, in whole frames */
	int seq_len, seq_pos, seq_ack, seq_cpd;
	int cpd_sent, ack_sent;
	long cpd_at;            /* frames after our last CPd */
	V92Cpd cpd;             /* ours */

	/* receiver */
	enum v92_rx rx;
	float buf[V92_BUF];
	/* the period 6 tones (Ru, Su) and their reversals */
	float th[16];
	cplx tbh[32];
	unsigned char tcoh[32];
	int tone_n, tone_miss, rev_n, rev_hold;
	float rev_min;
	long rev_cand, rev_at;
	Goertzel ta_g;          /* tone A */
	float ta_e;
	int ta_blocks;
	int revs;               /* reversals of Su so far */
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
	long fpe_b1;            /* B1u's first symbol (data symbols) */
	long dsn, ddn;          /* data symbols in, decided */
	V92Cpd cpd_fpe;         /* the new CPd */
	V92Cpd cpd_old;         /* the decisions' until B1u */
	int fpes;
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
