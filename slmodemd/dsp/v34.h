/*
 *
 *    v34.h - ITU V.34 data pump: shared definitions
 *
 */

#ifndef __V34_H__
#define __V34_H__

#include "qam.h"

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

static inline float v34_symbol_rate(int b)
{
	return 2400.0f * v34_bauds[b].a / v34_bauds[b].c;
}

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

/* MP sequence contents (10.1.3.12/V.34) */
struct v34_mp {
	int type;
	int rate_ca, rate_ac;   /* maximum data rates, N * 2400 */
	int aux, trellis, nonlinear, expanded, ack;
	int rate_mask;          /* bit 0: 2400 .. bit 13: 33600 */
	int asym;
	int h[6];               /* precoding coefficients (type 1) */
};

/*
 *    data mode (v34_data.c)
 */
#define V34_MAX_RINGS   18
#define V34_G2_LEN      (2 * V34_MAX_RINGS - 1)
#define V34_G4_LEN      (4 * V34_MAX_RINGS - 3)
#define V34_Z8_LEN      (8 * V34_MAX_RINGS - 6)
#define V34_MAX_B       80

struct v34_dparm {
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
	unsigned long long g2[V34_G2_LEN], g4[V34_G4_LEN], z8[V34_Z8_LEN];
};

/* n: the rate in units of 2400 bit/s; returns -1 when not allowed */
int v34_data_params(struct v34_dparm *p, int baud, int n, int expanded);
/* precoding with the coefficients of an MP sequence */
void v34_data_precode(struct v34_dparm *p, const int h[6]);

/* the precoder's tap delay line (9.6.2/V.34) */
struct v34_precoder {
	int x[3][2];            /* x(n - 1) .. x(n - 3), units of 2^-7 */
	int p[2], c[2];         /* p(n) (units of 2^-7) and c(n) */
};

struct v34_dtx {
	struct v34_dparm p;
	void (*get_bits)(void *ctx, unsigned char *bits, int n);
	void *ctx;
	int sbc;                /* high/low frame switching */
	int z, state, v0;       /* differential, trellis and sync state */
	int half, pat;          /* 4D symbols in the half data frame, sync
				   pattern position */
	struct v34_precoder pc;
	cplx sym[8];
	int pos;
};

void v34_dtx_init(struct v34_dtx *d, const struct v34_dparm *p);
cplx v34_dtx_symbol(struct v34_dtx *d, unsigned *scr, int tap);

#define V34_STATES      64      /* at most */
#define V34_TB          32      /* Viterbi decoding delay, 4D symbols */

struct v34_drx {
	struct v34_dparm p;
	void (*put_bits)(void *ctx, const unsigned char *bits, int n);
	void *ctx;
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
	struct v34_precoder pc; /* the receiver's copy of the precoder */
};

void v34_drx_init(struct v34_drx *d, const struct v34_dparm *p);
cplx v34_drx_symbol(struct v34_drx *d, cplx z, unsigned *scr, int tap);

/*
 *    primary channel transmitter (v34_modem.c)
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

struct v34_tx {
	struct qam_tx qam;
	int baud;
	unsigned scr;
	int tap;
	enum v34_txseg seg;
	long n;                 /* symbols sent in this segment */
	int idx;                /* last training point, for differential coding */
	unsigned char bits[200];
	int nbits, pos;
	struct v34_mp mp;       /* the MP we send */
	int mp_sent;            /* complete MP / MP' sequences sent */
	int ack_mp;             /* set the acknowledge bit from the next MP on */
	struct v34_dtx dtx;     /* data mode, when data_on */
	int data_on;
};

void v34_tx_init(struct v34_tx *t, int baud, int high, int caller);
void v34_tx_segment(struct v34_tx *t, enum v34_txseg seg);
float v34_tx_sample(struct v34_tx *t);
/* the data mode parameters, before the end of E */
void v34_tx_data(struct v34_tx *t, const struct v34_dparm *p,
		 void (*get_bits)(void *, unsigned char *, int), void *ctx);

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
struct v90_dil {
	int n, lsp, ltp;
	unsigned char sp[128], tp[128];
	int h[8], ref[8];
	unsigned char ucode[255];
};

typedef struct { double re, im; } lscplx;

struct v34_rx {
	struct qam_rx qam;
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
	int jword;
	unsigned char mpbits[200];
	/* V.90 digital modem: Ja carries the DIL descriptor */
	int v90, ev_ja;
	int sbar_count;         /* S to S-bar transitions seen */
	int s_in_trn;           /* V.90: S may follow TRN directly (the
				   analogue modem's TRN1a during DIL) */
	/* and CP in phase 4 */
	int ev_cpt, ev_cp, ev_cpack;
	unsigned char cpraw[1800];      /* the last good CP frame */
	unsigned char *jabits;
	int ja_n, ja_len;
	struct v90_dil dil;
	int mp_n, mp_len;       /* collecting an MP frame */
	int ones;               /* ones since the end of the last MP frame */
	int after_mp;
	int ev_mp;
	struct v34_mp mp;       /* the remote's MP */
	long mp_end;            /* symbol of the end of the last MP frame */
	struct v34_drx drx;     /* data mode, when data_on */
	cplx last_d[3];         /* the last symbols before data mode */
	double err_r[8];        /* error autocorrelation, lags 0 to 3 */
	cplx err_h[3];
	long err_n;
	int data_on;
};

int  v34_rx_init(struct v34_rx *r, int baud, int high, int caller);
void v34_rx_free(struct v34_rx *r);
void v34_rx_sample(struct v34_rx *r, float x);
float v34_rx_precoder(const struct v34_rx *r, int h[6]);
int   v34_rx_v90(struct v34_rx *r);
void v34_rx_data(struct v34_rx *r, const struct v34_dparm *p,
		 void (*put_bits)(void *, const unsigned char *, int), void *ctx);

/* CRC-16 (x^16 + x^12 + x^5 + 1, preset to ones) over bits in time order,
   as used by the INFO and MP sequences */
unsigned v34_crc_bits(const unsigned char *bits, int n, unsigned crc);

#endif /* __V34_H__ */
