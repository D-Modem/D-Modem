/*
 *
 *    v34.h - ITU V.34 data pump: shared definitions
 *
 */

#ifndef __V34_H__
#define __V34_H__

#include <string.h>
#include "bits.h"
#include "qam.h"

namespace dsp {

/* symbol rates, in the order of the INFO and MP fields */
enum {
	V34_BAUD_2400,
	V34_BAUD_2743,
	V34_BAUD_2800,
	V34_BAUD_3000,
	V34_BAUD_3200,
	V34_BAUD_3429,
	V34_NBAUDS
};

struct v34_baud {
	int nominal;            /* rounded symbol rate */
	int a, c;               /* symbol rate = 2400 * a / c */
	int d[2], e[2];         /* carrier = symbol rate * d / e (low, high) */
	int J, P;               /* data frame: J mapping frames, P per frame */
};
extern const struct v34_baud v34_bauds[V34_NBAUDS];

/* the symbol rate of V34_BAUD_* b, in symbols/s */
static inline float v34_symbol_rate(int b)
{
	return 2400.0f * v34_bauds[b].a / v34_bauds[b].c;
}

/* the carrier of symbol rate b, low (0) or high (1), in Hz */
static inline float v34_carrier(int b, int high)
{
	return v34_symbol_rate(b) * v34_bauds[b].d[high] / v34_bauds[b].e[high];
}

struct v34_rate {
	int b, m_min, m_exp;
};
extern const struct v34_rate v34_rates[V34_NBAUDS][14];
extern const signed char v34_quarter[416][2];
extern const cplx v34_trn4[4];
cplx v34_pp(int i);

/* MP style frames coming in (bits.h): V.90's Ja and CP, V.92's Ja,
   CPt, CPu, CPus and SUVu.  After a sync of 17 ones and a zero their
   bits go into b, their length from their header, their CRC checked at
   the end (v90.cpp) */
#define SYNC_FRAME_MAX  3000
struct SyncFrames {
	enum Kind {
		DIL90,          /* V.90's Ja: its length at bit 50 */
		DIL92,          /* V.92's Ja */
		CP90,           /* V.90's CP and CPt: at bit 129 */
		CP92,           /* V.92's CPt and CPu, and CPus and SUVu of 51
				   bits */
	};
	/* add() and bit() */
	enum { NONE = 0, GOOD = 1, BAD_CRC = -1, BAD_LENGTH = -2 };

	Kind kind;
	int max;                /* the longest frame taken */
	unsigned sr;            /* bit()'s last bits, the newest in bit 0 */
	unsigned char b[SYNC_FRAME_MAX];
	int n;                  /* bits in b: 0, none (looking for a sync) */
	int len;                /* the frame's length, 0 until known */

	/* cleared, for frames of kind k of max_len bits at most */
	void init(Kind k, int max_len = SYNC_FRAME_MAX);
	/* a frame begins: its sync in b */
	void start();
	/* in a frame */
	bool busy() const { return n != 0; }
	/* the frame in progress dropped */
	void drop() { n = 0; }
	/* a bit of the frame start() began: GOOD when it ends with a good
	   CRC (len bits in b), BAD_CRC with a bad one, BAD_LENGTH when its
	   header gives no length it may have (the frame dropped), NONE
	   until then */
	int add(int x);
	/* a bit, the sync looked for in them (sr) while not busy(); as
	   add() */
	int bit(int x);
};

/* MP sequence contents (10.1.3.12/V.34) */
struct V34Mp {
	int type;
	int rate_ca, rate_ac;   /* maximum data rates, N * 2400 */
	int aux, trellis, nonlinear, expanded, ack;
	int rate_mask;          /* bit 0: 2400 .. bit 13: 33600 */
	int asym;
	int h[6];               /* precoding coefficients (type 1) */

	int build(unsigned char *b) const;
	int parse(const unsigned char *b, int len);
};

/*
 *    data mode (v34_data.cpp)
 */
#define V34_MAX_RINGS   18
#define V34_G2_LEN      (2 * V34_MAX_RINGS - 1)
#define V34_G4_LEN      (4 * V34_MAX_RINGS - 3)
#define V34_Z8_LEN      (8 * V34_MAX_RINGS - 6)
#define V34_MAX_B       80

struct V34Precoder;

struct V34Params {
	int rate;               /* bit/s */
	int b;                  /* bits in a high mapping frame */
	int K, q, M;            /* shell mapping bits, uncoded bits, rings */
	int J, P, r;            /* data frames per superframe, mapping frames
				   per data frame, high frames among them */
	float g;                /* half the point spacing, in units of the
				   training points */
	int states;             /* of the trellis code: 16, 32 or 64 */
	int h[6];               /* precoding coefficients, 14 fractional
				   bits: h(1) re, im, h(2) re, ... */
	int precode, w;         /* h is not zero; c(n) in multiples of
				   2^w */
	float theta;            /* non-linear encoder (9.7/V.34): 0 or
				   0.3125, transmitter only */
	float nl_gain;          /* after it: the power without it */
	double pw_;             /* mean power of the points (integers) */
	unsigned long long g2[V34_G2_LEN], g4[V34_G4_LEN], z8[V34_Z8_LEN];

	int shell_tables();
	unsigned long long g2p(int i) const;
	unsigned long long g4p(int i) const;
	double frame_power(int k, double theta = 0, double e0 = 1) const;
	void set_nonlinear(double theta_);
	/* n: the rate in units of 2400 bit/s; returns -1 when not allowed */
	int init(int baud, int n, int expanded);
	/* precoding with the coefficients of an MP sequence */
	void set_precoder(const int h_[6]);
	void precoder_push(V34Precoder *pc, int yr, int yi) const;
	void shell_map(unsigned long r0, int m[8]) const;
	int pair_index(int x, int y) const;
	unsigned long shell_demap(const int m[8]) const;
	int decide(cplx z, int *px, int *py) const;
	void subset_points(float u, float v, float dist[8], int pt[8]) const;
};

/* the precoder's tap delay line (9.6.2/V.34) */
struct V34Precoder {
	int x[3][2];            /* x(n - 1) .. x(n - 3), units of 2^-7 */
	int p[2], c[2];         /* p(n) (units of 2^-7) and c(n) */

	int c_parity() const;
};

struct V34DataTx {
	V34Params p;
	CoreBits link;          /* the data */
	int sbc;                /* high/low frame switching */
	int z, state, v0;       /* differential, trellis and sync state */
	int half, pat;          /* 4D symbols in the half data frame, sync
				   pattern position */
	V34Precoder pc;
	cplx sym[8];
	int pos;

	void init(const V34Params *p_);
	void encode_frame(unsigned *scr, int tap);
	cplx symbol(unsigned *scr, int tap);
};


#define V34_STATES      64      /* at most */
#define V34_TB          32      /* Viterbi decoding delay, 4D symbols */

struct V34DataRx {
	V34Params p;
	CoreBits link;          /* the data */
	int sbc, z;
	int e[8];               /* rotation << 9 | point index */
	int pos;
	/* Viterbi decoder */
	float metric[V34_STATES];
	unsigned char prev[V34_TB][V34_STATES];
	int pa[V34_TB][V34_STATES], pb[V34_TB][V34_STATES];
	int vt, vn;
	int half, pat, v0;      /* superframe inversions, as the transmitter */
	int half2;              /* the first 2D symbol of a 4D one is in: */
	float da[8];
	int ca[8];
	V34Precoder pc; /* the receiver's copy of the precoder */

	void viterbi_init();
	void init(const V34Params *p_);
	void decode_frame(unsigned *scr, int tap);
	int viterbi_4d(const float da_[8], const int ca_[8], const float db[8], const int cb[8], int out[2]);
	void point(int pt, unsigned *scr, int tap);
	cplx symbol(cplx z_, unsigned *scr, int tap);
};


/*
 *    primary channel transmitter (v34_modem.cpp)
 */
enum v34_txseg {
	V34_TX_SILENCE,
	V34_TX_S,
	V34_TX_SBAR,
	V34_TX_PP,
	V34_TX_TRN,
	V34_TX_J,
	V34_TX_JP,
	V34_TX_MP,
	V34_TX_E,
	V34_TX_DATA,
};

struct V34Tx {
	QamTx qam;
	int baud;
	unsigned scr;
	int tap;
	enum v34_txseg seg;
	long n;                 /* symbols sent in this segment */
	int idx;                /* last training point, for differential coding */
	int trn16;              /* phase 4 TRN, MP and E of 16 points (the
				   remote's J asked for them) */
	unsigned char bits[200];
	int nbits, pos;
	V34Mp mp;       /* the MP we send */
	int mp_sent;            /* complete MP / MP' sequences sent */
	int ack_mp;             /* set the acknowledge bit from the next MP on */
	V34DataTx dtx;     /* data mode, when data_on */
	int data_on;

	int dibit(int b0, int b1);
	void init(int baud_, int high, int caller);
	void segment(enum v34_txseg seg_);
	void start_data(const V34Params *p, CoreBits link);
	float sample();
};

/*
 *    primary channel receiver
 */
enum v34_rxstage {
	V34_RX_IDLE,            /* waiting for S */
	V34_RX_S,               /* in S, waiting for S-bar */
	V34_RX_SBAR,
	V34_RX_PP,
	V34_RX_TRN,             /* trained on the 4 point signals */
	V34_RX_DATA,
};

/* V.90: the DIL descriptor of Ja (Table 12/V.90) */
struct V90Dil {
	int n, lsp, ltp;
	unsigned char sp[128], tp[128];
	int h[8], ref[8];
	unsigned char ucode[255];

	int parse(const unsigned char *b, int len);
};

typedef struct { double re, im; } lscplx;

struct V34Rx {
	QamRx qam;
	int baud;
	int tap;                /* the remote's scrambler */
	int phase4;             /* the next S starts phase 4 (no PP) */
	enum v34_rxstage stage;
	long n;                 /* symbols in this stage */
	long sym;
	cplx z1, z2;            /* the last symbols, for S detection */
	float s_score, sbar_score;
	int s_count;
	int on;                 /* signal present */
	float mse;
	float ref_power;        /* input power when trained */
	cplx good_eq[QAM_MAX_EQ];       /* the equalizer at its best */
	cplx pp_buf[48];        /* the first PP symbols, for the alignment */
	int pp_off;
	int have_good;
	int timing_fixed;       /* the timing follows the equalizer */
	float c0, t_freq;       /* trained equalizer delay, clock offset */
	lscplx *ls_r, *ls_p;    /* least squares training */
	int phase4_ls, have_ls;
	float ls_d;             /* delay of the last solution */
	long ls_sym;            /* at this symbol */
	long ls_n;              /* TRN symbols */
	float x_ref;            /* equalizer input energy when trained */
	/* differential decoder for J, MP and E */
	int idx;
	unsigned scr;
	unsigned sr;            /* last decoded bits, newest in bit 0 */
	int j_count, ev_j, ev_jp, ev_s, ev_e;
	int j16;                /* the remote asked for 16 point TRN */
	int jword;
	unsigned char mpbits[200];
	/* V.90 digital modem: Ja carries the DIL descriptor */
	int v90, ev_ja;
	int sbar_count;         /* S to S-bar transitions seen */
	int s_in_trn;           /* V.90: S may follow TRN directly (the
				   analogue modem's TRN1a during DIL) */
	/* and CP in phase 4 */
	int ev_cpt, ev_cp, ev_cpack;
	int cp_count;           /* good CP frames (not CPt) so far */
	int cp_silence;         /* the last one asked for silence (CPs) */
	unsigned char cpraw[1800];      /* the last good CP frame */
	SyncFrames *fr;         /* Ja, then CP (set_v90()) */
	V90Dil dil;
	int mp_n, mp_len;       /* collecting an MP frame */
	int ones;               /* ones since the end of the last MP frame */
	int after_mp;
	int ev_mp;
	V34Mp mp;       /* the remote's MP */
	long mp_end;            /* symbol of the end of the last MP frame */
	V34DataRx drx;     /* data mode, when data_on */
	cplx last_d[3];         /* the last symbols before data mode */
	double err_r[8];        /* error autocorrelation, lags 0 to 3 */
	cplx err_h[3];
	long err_n;
	int data_on;
	/* rate renegotiation (11.6/V.34): the remote's S in data mode */
	int ev_reneg;
	float mse_mid, mse_old; /* data mode mse 64 and 128 symbols ago */

	int init(int baud_, int high, int caller);
	int set_v90();
	void release();
	void set_stage(enum v34_rxstage s);
	void frame_start();
	void ja_bit(int bit);
	void cp_bit(int bit);
	void got_bit(int bit);
	void diff_decode(cplx z);
	void pp_align();
	float eq_delay() const;
	void timing_fix();
	void ls_collect(cplx d);
	int ls_solve();
	void ls_window(cplx d, long start);
	void ls_train(cplx d);
	void timing_dd(cplx d);
	int level_ok();
	void precode_eq(const V34Params *p);
	void reneg_s();
	void err_collect(cplx e);
	cplx precode_start(cplx z);
	void symbol();
	float precoder(int h[6]) const;
	/* clears the error autocorrelation that precoder() works from */
	void err_reset()
	{
		memset(err_r, 0, sizeof(err_r));
		memset(err_h, 0, sizeof(err_h));
		err_n = 0;
	}
	void start_data(const V34Params *p, CoreBits link);
	void sample(float x);
};


/* the trellis code (9.6.3/V.34): the subset label of a 2D point (odd
   coordinates), the encoder input Y1 | Y2 << 1 | Y3 << 2 | Y4 << 3 of a
   4D symbol from its two labels, and the next encoder state (16, 32 or 64
   states; its low bit is Y0) */
int v34_subset_label(int x, int y);
int v34_conv_in(int s0, int s1);
int v34_trellis_next(int states, int st, int y);

} // namespace dsp

#endif /* __V34_H__ */
