/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
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

#include "dm_types.h"
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
	s32 srate;
	float sps;
	float t;
	s16 span;
	cplx sym[2 * QAM_MAX_SPAN];
	Osc carrier;
	float scale;
	float pulse[2 * QAM_MAX_SPAN * QAM_PULSE_RES + 2];
	float step;             /* symbol periods a sample: 1 / (sps + dt) */

	/* carrier (Hz), baud, root raised cosine roll off alpha over span
	   symbols each side; symbols of rms rms_symbol give a pass band rms
	   of amplitude / sqrt(2) (that of a sine of amplitude amplitude) */
	void  init(s32 srate, float carrier, float baud, float alpha,
		   s16 span, float rms_symbol, float amplitude);
	/* the next line sample; at a symbol boundary the next symbol from
	   next() */
	template <class Next> float sample(Next next)
	{
		t += step;
		if (t >= 1) {
			t -= 1;
			std::copy_backward(sym, sym + 2 * span - 1,
					   sym + 2 * span);
			sym[0] = next();
		}
		return out();
	}
	void  flush();          /* forget symbols in flight */
	/* the symbol period dt samples longer than sps (loop timing: the
	   remote's clock, as our receiver measures it) */
	void  set_clock(float dt) { step = 1.0f / (sps + dt); }
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
	s32 srate;
	float sps;
	float baud;
	Osc lo;
	std::vector<float> mf;
	std::vector<cplx> mf_buf;
	s16 mf_len, mf_pos;
	/* level */
	float power;            /* input power (tc 20 ms) */
	float power_alpha;
	float agc_pwr, agc_alpha, agc_target;
	float agc_gain;
	s16 agc_hold;           /* keep the gain fixed */
	/* timing */
	float t_next, t_adj;
	float kt_p, kt_i, kt_gi;  /* proportional gain, integrator range and gain */
	s16 timing_hold;        /* no timing corrections (but the frequency) */
	s16 timing_freeze;      /* none at all */
	s16 half;
	cplx mid, prev_sym;
	/* equalizer and carrier */
	s16 eq_len;
	cplx eq[QAM_MAX_EQ];
	cplx eq_buf[QAM_MAX_EQ];
	float theta, dtheta;
	/* outputs of the last symbol */
	cplx y;                 /* equalizer input (symbol sample) */
	cplx eq_out;            /* equalizer output before rotation */
	cplx z;                 /* rotated equalizer output */
	cplx rot;
	s32 symbols;

	/* carrier (Hz), baud, roll off alpha, matched filter span symbols
	   each side, eq_len T/2 taps, AGC to target_power at the symbol
	   instants; 0, or -1 when out of memory */
	s16   init(s32 srate, float carrier, float baud, float alpha,
		   s16 span, s16 eq_len, float target_power);
	void  release();        /* the buffers init took */
	void  reset();          /* eq to centre tap, PLL to 0 */
	/* returns 1 when a new symbol is available in z */
	s16   sample(float x);
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
	s16 diff;
	s16 conv;

	/* q: the scrambled bits of one symbol (Q1 first in bit 0); returns
	   the constellation index (bit 0 Y0, bits 1-2 Y1Y2, bits 3.. Q3...) */
	s16 encode(s16 q, s16 bits);
};

/* the index of the point nearest to z among pts[0], pts[step], ... below
   pts[n] (the first of equals): a slicer on a constellation's table */
inline s16 nearest_point(const cplx *pts, s16 n, cplx z, s16 step = 1)
{
	s16 best = 0;
	float bd = 1e30f;
	for (s16 k = 0; k < n; k += step) {
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
	s16 nbits;              /* bits per symbol */
	const cplx *points;     /* constellation, indexed like trellis_encode */
	s16 npoints;
	float metric[8];
	/* survivor paths: per state, the decided constellation index */
	s16 path[VITERBI_DEPTH][8];
	s16 prev[VITERBI_DEPTH][8];
	s16 pos;
	s16 filled;
	s16 last_y12;           /* differential decoder state */

	/* points: the 2^(nbits + 1) point constellation; clears the paths */
	void init(const cplx *points, s16 nbits);
	/* feed one received point; returns the decoded data bits (Q1 in bit
	   0) of the symbol VITERBI_DEPTH-1 periods ago, or -1 while filling */
	s16  decode(cplx z, cplx *decided);
};

/*
 *    line echo canceller (real pass band, NLMS)
 */
struct EchoCanceller {
	s16 len;
	std::vector<float> w;
	std::vector<float> hist;
	s16 pos;
	float tx_power;
	float pw_rx, pw_err;    /* in and out while it adapts */
	s16 training, off;

	/* len taps (samples); 0, or -1 when out of memory */
	s16   init(s16 len);
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
/* V.32terbo's, through its non-linear encoder (constant initialized:
   ready before any static constructor) */
template <s32 N> struct V32tPoints {
	cplx p[N];
};
extern const V32tPoints<256> v32_points_16800;
extern const V32tPoints<512> v32_points_19200;
extern const cplx v32_abcd[4];

/* narrow band tone tracker for a tone and its phase reversals (V.32's
   AA / AC, V.61's tones) */
struct ToneTrk {
	Osc lo;
	cplx lp1, lp2;
	float a;
	cplx hist[256];
	u32 pos;
	s16 lag;
	s16 on;
	s32 on_count;
	s16 hold;

	void init(float freq, s32 srate);
	s16 run(float x, float in_power, s32 srate);
};

} // namespace dsp

#endif /* __QAM_H__ */
