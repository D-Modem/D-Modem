/*
 *
 *    qam.h - generic QAM transmitter and receiver, trellis coding,
 *            echo canceller: the building blocks of the V.32/V.32bis,
 *            V.17, V.29 and V.27ter data pumps
 *
 */

#ifndef __QAM_H__
#define __QAM_H__

#include "dsp.h"

#define QAM_MAX_SPAN 16         /* pulse half length, symbols */
#define QAM_PULSE_RES 64        /* pulse table points per symbol */
#define QAM_MF_RES 64           /* receive filter table points per sample */
#define QAM_MAX_EQ 64           /* T/2 spaced equalizer taps */

/*
 *    transmitter: pulse shaping and modulation of complex symbols;
 *    get_symbol() is called at the symbol rate
 */
struct qam_tx {
	int srate;
	float sps;
	float t;
	int span;
	cplx sym[2 * QAM_MAX_SPAN];
	struct osc carrier;
	float scale;
	float pulse[2 * QAM_MAX_SPAN * QAM_PULSE_RES + 2];
	cplx (*get_symbol)(void *ctx);
	void *ctx;
};

void  qam_tx_init(struct qam_tx *q, int srate, float carrier, float baud,
		  float alpha, int span, float rms_symbol, float amplitude);
float qam_tx_sample(struct qam_tx *q);
void  qam_tx_flush(struct qam_tx *q);   /* forget symbols in flight */

/*
 *    receiver: down conversion, matched filter, AGC, Gardner timing,
 *    T/2 spaced equalizer and carrier phase rotation.  The caller
 *    decides on each symbol (slicer or known training symbol) and
 *    calls qam_rx_adapt() with that decision.
 */
struct qam_rx {
	int srate;
	float sps;
	float baud;
	struct osc lo;
	float *mf;
	cplx *mf_buf;
	int mf_len, mf_pos;
	/* level */
	float power;            /* input power (band passed, tc 20 ms) */
	float power_alpha;
	float agc_pwr, agc_alpha, agc_target;
	float agc_gain;
	int agc_hold;           /* keep the gain fixed */
	/* timing */
	float t_next, t_adj;
	float kt_p, kt_i, kt_gi;  /* proportional gain, integrator range and gain */
	int timing_hold;        /* no timing corrections (but the frequency) */
	int timing_freeze;      /* none at all */
	int half;
	cplx mid, prev_sym;
	/* equalizer and carrier */
	int eq_len;
	cplx eq[QAM_MAX_EQ];
	cplx eq_buf[QAM_MAX_EQ];
	float theta, dtheta;
	/* outputs of the last symbol */
	cplx y;                 /* equalizer input (symbol sample) */
	cplx eq_out;            /* equalizer output before rotation */
	cplx z;                 /* rotated equalizer output */
	cplx rot;
	long symbols;
};

int   qam_rx_init(struct qam_rx *q, int srate, float carrier, float baud,
		  float alpha, int span, int eq_len, float target_power);
void  qam_rx_free(struct qam_rx *q);
void  qam_rx_reset(struct qam_rx *q);   /* eq to centre tap, PLL to 0 */
/* returns 1 when a new symbol is available in q->z */
int   qam_rx_sample(struct qam_rx *q, float x);
/* adapt equalizer (mu, normalized) and carrier loop toward decision d */
void  qam_rx_adapt(struct qam_rx *q, cplx d, float mu, float k1, float k2);
/* recompute the current output after the equalizer was replaced */
void  qam_rx_recompute(struct qam_rx *q);
/* rotate the carrier phase so that the current output points at d */
void  qam_rx_align_phase(struct qam_rx *q, cplx d);

/*
 *    V.17 / V.32bis trellis code (8 state, rotationally invariant)
 */
struct trellis_enc {
	int diff;
	int conv;
};
/* q: the scrambled bits of one symbol (Q1 first in bit 0); returns the
   constellation index (bit 0 Y0, bits 1-2 Y1Y2, bits 3.. Q3...) */
int trellis_encode(struct trellis_enc *t, int q, int bits);

#define VITERBI_DEPTH 16
struct viterbi {
	int nbits;              /* bits per symbol */
	const cplx *points;     /* constellation, indexed like trellis_encode */
	int npoints;
	float metric[8];
	/* survivor paths: per state, the decided constellation index */
	int path[VITERBI_DEPTH][8];
	int prev[VITERBI_DEPTH][8];
	int pos;
	int filled;
	int last_y12;           /* differential decoder state */
};
void viterbi_init(struct viterbi *v, const cplx *points, int nbits);
/* feed one received point; returns the decoded data bits (Q1 in bit 0)
   of the symbol VITERBI_DEPTH-1 periods ago, or -1 while filling */
int  viterbi_decode(struct viterbi *v, cplx z, cplx *decided);

/*
 *    line echo canceller (real pass band, NLMS)
 */
struct echo_canceller {
	int len;
	float *w;
	float *hist;
	int pos;
	float tx_power;
	float pw_rx, pw_err;    /* in and out while it adapts */
	int training, off;
};
int   echo_init(struct echo_canceller *e, int len);
/* the end of the training: what did it take away (dB)?  Below 6 dB there
   was no echo to cancel (a 4-wire network, or the remote was not silent)
   and the canceller stays off rather than add noise */
float echo_judge(struct echo_canceller *e);
void  echo_free(struct echo_canceller *e);
/* returns rx with the echo of the transmitted signal removed; adapts
   when mu > 0 */
float echo_run(struct echo_canceller *e, float tx, float rx, float mu);

/*
 *    constellations built from the original library's tables
 *    (scaled to the V.32bis grid units)
 */
extern const cplx v32_points_7200[16];
extern const cplx v32_points_9600[32];
extern const cplx v32_points_12000[64];
extern const cplx v32_points_14400[128];
extern const cplx v32_points_4800[4];
extern const cplx v32_abcd[4];

#endif /* __QAM_H__ */
