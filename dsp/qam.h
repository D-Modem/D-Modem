/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    qam.h - generic QAM transmitter and receiver, trellis coding,
 *            echo canceller: the building blocks of the V.32/V.32bis,
 *            V.17, V.29 and V.27ter data pumps
 *
 */

#ifndef __QAM_H__
#define __QAM_H__

#include <cstring>
#include <vector>

#include "dsp.h"

namespace dsp {

#define QAM_MAX_SPAN 16         /* pulse half length, symbols */
#define QAM_PULSE_RES 64        /* pulse table points per symbol */
#define QAM_MF_RES 64           /* receive filter table points per sample */
#define QAM_MAX_EQ 64           /* T/2 spaced equalizer taps */

/*
 *    transmitter: pulse shaping and modulation of complex symbols, each
 *    from the source sample() is given, at the symbol rate
 */
struct QamTx {
	int srate;
	float sps;
	float t;
	int span;
	cplx sym[2 * QAM_MAX_SPAN];
	Osc carrier;
	float scale;
	float pulse[2 * QAM_MAX_SPAN * QAM_PULSE_RES + 2];

	/* carrier (Hz), baud, root raised cosine roll off alpha over span
	   symbols each side; symbols of rms rms_symbol give a pass band rms
	   of amplitude / sqrt(2) (that of a sine of amplitude amplitude) */
	void  init(int srate, float carrier, float baud, float alpha, int span,
		   float rms_symbol, float amplitude);
	/* the next line sample; at a symbol boundary the next symbol from
	   next() */
	template <class Next> float sample(Next next)
	{
		t += 1.0f / sps;
		if (t >= 1) {
			t -= 1;
			std::copy_backward(sym, sym + 2 * span - 1,
					   sym + 2 * span);
			sym[0] = next();
		}
		return out();
	}
	void  flush();          /* forget symbols in flight */
private:
	/* the pulse at t symbol periods from its centre (interpolated) */
	float pulse_at(float t);
	/* the symbols in flight shaped and up converted: the sample */
	float out();
};

/*
 *    receiver: down conversion, matched filter, AGC, Gardner timing,
 *    T/2 spaced equalizer and carrier phase rotation.  The caller
 *    decides on each symbol (slicer or known training symbol) and
 *    calls adapt() with that decision.
 */
struct QamRx {
	int srate;
	float sps;
	float baud;
	Osc lo;
	std::vector<float> mf;
	std::vector<cplx> mf_buf;
	int mf_len, mf_pos;
	/* level */
	float power;            /* input power (tc 20 ms) */
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

	/* carrier (Hz), baud, roll off alpha, matched filter span symbols
	   each side, eq_len T/2 taps, AGC to target_power at the symbol
	   instants; 0, or -1 when out of memory */
	int   init(int srate, float carrier, float baud, float alpha, int span,
		   int eq_len, float target_power);
	void  release();        /* the buffers init took */
	void  reset();          /* eq to centre tap, PLL to 0 */
	/* returns 1 when a new symbol is available in z */
	int   sample(float x);
	/* adapt equalizer (mu, normalized) and carrier loop toward
	   decision d: k1 the phase gain, k2 the frequency gain (rad per
	   symbol per radian of error), 0 leaves that part alone */
	void  adapt(cplx d, float mu, float k1, float k2);
	/* recompute the current output after the equalizer was replaced */
	void  recompute();
	/* rotate the carrier phase so that the current output points at d */
	void  align_phase(cplx d);
};

/*
 *    V.17 / V.32bis trellis code (8 state, rotationally invariant)
 */
struct TrellisEnc {
	int diff;
	int conv;

	/* q: the scrambled bits of one symbol (Q1 first in bit 0); returns
	   the constellation index (bit 0 Y0, bits 1-2 Y1Y2, bits 3.. Q3...) */
	int encode(int q, int bits);
};

/* the index of the point nearest to z among pts[0], pts[step], ... below
   pts[n] (the first of equals): a slicer on a constellation's table */
inline int nearest_point(const cplx *pts, int n, cplx z, int step = 1)
{
	int best = 0;
	float bd = 1e30f;
	for (int k = 0; k < n; k += step) {
		float d = cplx_pow(cplx_sub(z, pts[k]));
		if (d < bd) {
			bd = d;
			best = k;
		}
	}
	return best;
}

#define VITERBI_DEPTH 16
struct Viterbi {
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

	/* points: the 2^(nbits + 1) point constellation; clears the paths */
	void init(const cplx *points, int nbits);
	/* feed one received point; returns the decoded data bits (Q1 in bit
	   0) of the symbol VITERBI_DEPTH-1 periods ago, or -1 while filling */
	int  decode(cplx z, cplx *decided);
};

/*
 *    line echo canceller (real pass band, NLMS)
 */
struct EchoCanceller {
	int len;
	std::vector<float> w;
	std::vector<float> hist;
	int pos;
	float tx_power;
	float pw_rx, pw_err;    /* in and out while it adapts */
	int training, off;

	/* len taps (samples); 0, or -1 when out of memory */
	int   init(int len);
	/* the end of the training: what did it take away (dB)?  Below 6 dB
	   there was no echo to cancel (a 4-wire network, or the remote was
	   not silent) and the canceller stays off rather than add noise */
	float judge();
	void  release();        /* the buffers init took */
	/* returns rx with the echo of the transmitted signal removed; adapts
	   when mu > 0 */
	float run(float tx, float rx, float mu);
};

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

} // namespace dsp

#endif /* __QAM_H__ */
