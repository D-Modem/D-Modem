/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    qam.cpp - generic QAM transmitter and receiver, V.17/V.32bis trellis
 *            coding with a Viterbi decoder, line echo canceller
 *
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "qam.h"

namespace dsp {


/*
 *    transmitter
 */

/* pulse table and carrier; scale so that the symbols' rms rms_symbol
   gives a pass band rms of amplitude / sqrt(2) */
void QamTx::init(int32_t srate_, float carrier_, float baud,
		 float alpha, int span_, float rms_symbol, float amplitude)
{
	int i, n;

	reinit(*this);
	srate = srate_;
	sps = srate_ / baud;
	span = span_ > QAM_MAX_SPAN ? QAM_MAX_SPAN : span_;
	n = 2 * span * QAM_PULSE_RES;
	for (i = 0; i <= n + 1; i++)
		pulse[i] = rrc_pulse(
			static_cast<float>(i) / QAM_PULSE_RES - span, alpha);
	/* unit energy pulses: the pass band rms is amplitude / sqrt(2) */
	scale = amplitude / rms_symbol;
	carrier.set_freq(carrier_, srate_);
}

/* the symbols still in the pulse shaping filter are dropped */
void QamTx::flush()
{
	zero(sym);
}

/* the root raised cosine pulse at t_ symbol periods from its centre,
   linear interpolation in the table, 0 outside +-span */
float QamTx::pulse_at(float t_)
{
	float p = (t_ + span) * QAM_PULSE_RES;
	int i;
	if (p < 0 || p >= 2 * span * QAM_PULSE_RES)
		return 0;
	i = static_cast<int>(p);
	p -= i;
	return pulse[i] + p * (pulse[i + 1] - pulse[i]);
}

/* the pass band sample at t: the sum of the 2 * span symbols in flight
   (sym[k] went out k periods ago, delayed by span), up converted (real
   part) */
float QamTx::out()
{
	cplx s = { 0, 0 }, c;
	int k;

	for (k = 0; k < 2 * span; k++) {
		float g = pulse_at(t + k - span);
		s.re += g * sym[k].re;
		s.im += g * sym[k].im;
	}
	c = carrier.next_cplx();
	return scale * (s.re * c.re - s.im * c.im);
}


/*
 *    receiver
 */

/* allocates the matched filter table (QAM_MF_RES points per sample, unit
   energy) and its delay line; sets the loop gains and the AGC target
   (target_power less the roll off's share); 0, or -1 when out of memory */
int QamRx::init(int32_t srate_, float carrier, float baud_,
		float alpha, int span, int eq_len_, float target_power)
{
	int32_t i;
	float sum;

	reinit(*this);
	srate = srate_;
	baud = baud_;
	sps = srate_ / baud_;
	lo.set_freq(-carrier, srate_);
	mf_len = static_cast<int>(2 * span * sps) | 1;
	/* the matched filter is evaluated at the T/2 instants themselves,
	   from a table of QAM_MF_RES points per sample */
	/* the last mf_len + 1 samples, twice */
	if (!alloc_zeroed(mf, (static_cast<int32_t>(mf_len) + 2) *
				      QAM_MF_RES + 2) ||
	    !alloc_zeroed(mf_buf, 2 * (mf_len + 1))) {
		release();
		return -1;
	}
	for (i = 0, sum = 0; i < mf_len; i++) {
		float h = rrc_pulse((i - mf_len / 2) / sps, alpha);
		sum += h * h;
	}
	for (i = 0; i < (static_cast<int32_t>(mf_len) + 2) * QAM_MF_RES + 2;
	     i++)
		mf[i] = rrc_pulse((static_cast<float>(i) / QAM_MF_RES - 1 -
				      mf_len / 2) / sps, alpha) / sqrtf(sum);
	power_alpha = 1.0f / (0.02f * srate_);
	agc_alpha = 1.0f / (0.03f * 2 * baud_);
	/* the mean power of the matched filter output is lower than at the
	   symbol instants by the roll off */
	agc_target = target_power * (1 - alpha / 4);
	kt_p = 0.03f * sps;
	kt_i = 0.001f * sps;
	kt_gi = 0.03f;
	eq_len = eq_len_ > QAM_MAX_EQ ? QAM_MAX_EQ : eq_len_;
	t_next = sps / 2;
	reset();
	return 0;
}

/* frees the matched filter buffers (also after a failed init) */
void QamRx::release()
{
	free_buffer(mf);
	free_buffer(mf_buf);
}

/* equalizer back to a single (even, centre) tap, carrier phase and
   frequency and the timing integrator to 0; AGC and buffers are kept */
void QamRx::reset()
{
	zero(eq);
	eq[eq_len / 2 & ~1].re = 1;
	theta = dtheta = 0;
	t_adj = 0;
}

/* one line sample x: power meter, down conversion, then at each T/2
   instant the interpolated matched filter output, AGC, the equalizer
   delay line; every second instant the Gardner timing update, the
   equalizer output and its rotation by the carrier phase (y, eq_out, z,
   rot).  Returns 1 when a new symbol is in z. */
int QamRx::sample(float x)
{
	cplx b;
	int i, ret = 0;

	power += power_alpha * (x * x - power);

	b = cplx_scale(lo.next_cplx(), x);
	/* newest sample at mf_buf[mf_pos], kept twice for a contiguous run */
	if (--mf_pos < 0)
		mf_pos = mf_len;
	mf_buf[mf_pos] = mf_buf[mf_pos + mf_len + 1] = b;

	t_next -= 1;
	while (t_next <= 0) {
		/* the matched filter output at t_next (<= 0) samples before
		   the previous sample (the latency the handshake timings
		   were tuned with) */
		const cplx *buf = mf_buf.data() + mf_pos + 1;
		float p = (1 + t_next) * QAM_MF_RES, f;
		int32_t k;
		cplx s = { 0, 0 };
		if (p < 0)
			p = 0;
		k = static_cast<int32_t>(p);
		f = p - k;
		for (i = 0; i < mf_len; i++, k += QAM_MF_RES) {
			float h = mf[k] + f * (mf[k + 1] - mf[k]);
			s.re += h * buf[i].re;
			s.im += h * buf[i].im;
		}
		if (!agc_hold) {
			agc_pwr += agc_alpha * (cplx_pow(s) - agc_pwr);
			agc_gain = agc_pwr > 1e-3f ?
				sqrtf(agc_target / agc_pwr) : 0;
		}
		s = cplx_scale(s, agc_gain);

		std::copy_backward(eq_buf, eq_buf + eq_len - 1,
				   eq_buf + eq_len);
		eq_buf[0] = s;
		t_next += sps / 2;

		if (half) {
			cplx out = { 0, 0 };
			if (!timing_hold && !timing_freeze) {
				/* Gardner timing error detector */
				float e = (mid.re * (prev_sym.re - s.re) +
					   mid.im * (prev_sym.im - s.im)) /
					(agc_target + 1e-3f);
				if (e > 1)
					e = 1;
				else if (e < -1)
					e = -1;
				t_adj += kt_gi * kt_i * e;
				if (t_adj > kt_i)
					t_adj = kt_i;
				else if (t_adj < -kt_i)
					t_adj = -kt_i;
				t_next += kt_p * e;
			}
			if (!timing_freeze)
				t_next += t_adj;
			prev_sym = s;
			y = s;
			for (i = 0; i < eq_len; i++)
				out = cplx_add(out, cplx_mul(eq[i], eq_buf[i]));
			eq_out = out;
			rot = cplx_make(cosf(theta), -sinf(theta));
			z = cplx_mul(out, rot);
			theta += dtheta;
			theta = remainderf(theta, 2 * static_cast<float>(M_PI));
			symbols++;
			ret = 1;
		} else
			mid = s;
		half ^= 1;
	}
	return ret;
}

/* decision directed update toward d: carrier phase (k1) and frequency
   (k2, clamped to +-0.05 rad per symbol) from the phase error of z, and
   normalized LMS on the equalizer (mu) with the error rotated back to
   the equalizer output */
void QamRx::adapt(cplx d, float mu, float k1, float k2)
{
	float p = cplx_pow(d), perr;
	int i;

	if (p > 0 && (k1 > 0 || k2 > 0)) {
		perr = (z.im * d.re - z.re * d.im) / p;
		if (perr > 0.5f)
			perr = 0.5f;
		else if (perr < -0.5f)
			perr = -0.5f;
		theta += k1 * perr;
		dtheta += k2 * perr;
		if (dtheta > 0.05f)
			dtheta = 0.05f;
		else if (dtheta < -0.05f)
			dtheta = -0.05f;
	}
	if (mu > 0) {
		float norm = 1e-3f;
		cplx e = cplx_mulc(cplx_sub(d, z), rot);
		for (i = 0; i < eq_len; i++)
			norm += cplx_pow(eq_buf[i]);
		mu /= norm;
		for (i = 0; i < eq_len; i++) {
			cplx g = cplx_mulc(e, eq_buf[i]);
			eq[i].re += mu * g.re;
			eq[i].im += mu * g.im;
		}
	}
}

/* turns theta so that the last output z points the way d does */
void QamRx::align_phase(cplx d)
{
	float a = atan2f(z.im, z.re) - atan2f(d.im, d.re);
	theta = remainderf(theta + a, 2 * static_cast<float>(M_PI));
}


/*
 *    trellis code: differential encoding of Y1Y2 (addition mod 4) and
 *    the 8 state convolutional encoder producing Y0
 */

static const unsigned char conv_next[8][4] = {
	{ 0, 2, 3, 1 }, { 4, 7, 5, 6 }, { 1, 3, 2, 0 }, { 7, 4, 6, 5 },
	{ 2, 0, 1, 3 }, { 6, 5, 7, 4 }, { 3, 1, 0, 2 }, { 5, 6, 4, 7 },
};

/* one symbol: Q1Q2 added to the differential state, which drives the
   convolutional encoder; returns Y0 | Y1Y2 << 1 | Q3.. << 3 (bits data
   bits per symbol) */
int TrellisEnc::encode(int q, int bits)
{
	diff = (diff + (q & 3)) & 3;
	conv = conv_next[conv][diff];
	return ((q << 1) & 0x78 & ((1 << (bits + 1)) - 1)) |
		(diff << 1) | ((conv >> 2) & 1);
}

/* a new decoder for the 2^(nbits_ + 1) points of points_ */
void Viterbi::init(const cplx *points_, int nbits_)
{
	reinit(*this);
	points = points_;
	nbits = nbits_;
	npoints = 1 << (nbits_ + 1);
}

/* one received point z: branch metrics from the nearest point of each
   of the 8 subsets, add compare select over the 8 states, then a trace
   back of VITERBI_DEPTH - 1 symbols from the best state.  Returns the
   data bits of that old symbol (Q1Q2 differentially decoded, Q3.. in
   bits 2..), its point in *decided (if not NULL), or -1 while the paths
   fill */
int Viterbi::decode(cplx z, cplx *decided)
{
	float best_d[8], m[8];
	int best_i[8], i, s, k, pos_, ret = -1;
	float bm;

	/* nearest point of each of the 8 subsets (index bits 0-2) */
	for (k = 0; k < 8; k++)
		best_d[k] = 1e30f;
	for (i = 0; i < npoints; i++) {
		float d = cplx_pow(cplx_sub(z, points[i]));
		k = i & 7;
		if (d < best_d[k]) {
			best_d[k] = d;
			best_i[k] = i;
		}
	}

	pos_ = pos;
	for (s = 0; s < 8; s++)
		m[s] = 1e30f;
	for (s = 0; s < 8; s++) {
		for (k = 0; k < 4; k++) {
			int ns = conv_next[s][k];
			int sub = (k << 1) | ((ns >> 2) & 1);
			float metric_ = metric[s] + best_d[sub];
			if (metric_ < m[ns]) {
				m[ns] = metric_;
				prev[pos_][ns] = s;
				path[pos_][ns] = best_i[sub];
			}
		}
	}
	bm = m[0];
	for (s = 1; s < 8; s++)
		if (m[s] < bm)
			bm = m[s];
	for (s = 0; s < 8; s++)
		metric[s] = m[s] - bm;

	if (++filled >= VITERBI_DEPTH) {
		int idx, y12, q;
		/* trace back from the best state */
		for (s = 0, k = 0; k < 8; k++)
			if (metric[k] == 0)
				s = k;
		for (i = 0, k = pos_; i < VITERBI_DEPTH - 1; i++) {
			s = prev[k][s];
			k = k ? k - 1 : VITERBI_DEPTH - 1;
		}
		idx = path[k][s];
		if (decided)
			*decided = points[idx];
		y12 = (idx >> 1) & 3;
		q = (y12 - last_y12) & 3;
		last_y12 = y12;
		ret = q | ((idx >> 3) << 2);
		filled = VITERBI_DEPTH;
	}
	pos = (pos_ + 1) % VITERBI_DEPTH;
	return ret;
}


/*
 *    echo canceller
 */

/* len_ taps, all 0; 0, or -1 when out of memory */
int EchoCanceller::init(int len_)
{
	reinit(*this);
	len = len_;
	if (!alloc_zeroed(w, len_) || !alloc_zeroed(hist, 2 * len_)) {
		release();
		return -1;
	}
	return 0;
}

/* frees the taps and the history (also after a failed init) */
void EchoCanceller::release()
{
	free_buffer(w);
	free_buffer(hist);
}

/* tx the sample we send, rx the one received at the same time: returns
   rx less the echo estimate (rx itself while off); with mu > 0 it starts
   or goes on with a training (NLMS normalized by the tx power) and
   measures the power before and after for judge() */
float EchoCanceller::run(float tx, float rx, float mu)
{
	float est = 0, err, *h;
	int i;

	if (mu > 0 && !training) {
		training = 1;
		off = 0;
		pw_rx = pw_err = 0;
	}
	if (off)
		return rx;
	/* history kept twice so that it can be read linearly */
	if (--pos < 0)
		pos = len - 1;
	hist[pos] = hist[pos + len] = tx;
	tx_power += 0.001f * (tx * tx - tx_power);
	h = hist.data() + pos;
	for (i = 0; i < len; i++)
		est += w[i] * h[i];
	err = rx - est;
	if (mu > 0) {
		pw_rx += 0.001f * (rx * rx - pw_rx);
		pw_err += 0.001f * (err * err - pw_err);
	}
	if (mu > 0 &&tx_power > 1) {
		float g = mu * err / (len * tx_power);
		for (i = 0; i < len; i++)
			w[i] += g * h[i];
	}
	return err;
}

/* the training is over: the echo return loss enhancement in dB; below
   6 dB the taps are cleared and the canceller turned off */
float EchoCanceller::judge()
{
	float erle = 10 * log10f((pw_rx + 1) / (pw_err + 1));
	training = 0;
	if (erle < 6) {
		off = 1;
		std::fill(w.begin(), w.end(), 0.0f);
	}
	return erle;
}

/* eq_out and z again from the current eq_buf, after eq was changed */
void QamRx::recompute()
{
	cplx out = { 0, 0 };
	int i;
	for (i = 0; i < eq_len; i++)
		out = cplx_add(out, cplx_mul(eq[i], eq_buf[i]));
	eq_out = out;
	z = cplx_mul(out, rot);
}

} // namespace dsp
