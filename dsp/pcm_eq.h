/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    pcm_eq.h - what the PCM modems' analogue receivers share (V90Rx,
 *               K56Rx): the detector of the digital modem's first
 *               signal and its reversal (ReversalSums); the T/2
 *               equaliser, solved by least squares on the known
 *               training and followed by NLMS, and the timing loop on
 *               its error, which moves the interpolator (PcmEqualizer)
 *
 *    A mixin on the receiver H (CRTP), which has
 *
 *        float xin(s32 kk, s16 i, s16 d) const   tap i's input for
 *                                                symbol kk, d samples
 *                                                (16 kHz) later
 *        V90Interp ip                            the line at 16 kHz
 *        float level                             the training's level
 *        void timing_locked(double loop_ppm)     the log line when the
 *                                                clock's offset is
 *                                                fitted (ppm() is it)
 *
 *    and makes this a friend if xin() is private.
 *
 */

#ifndef DSP_PCM_EQ_H
#define DSP_PCM_EQ_H

#include "dm_types.h"
#include "lsq.h"

namespace dsp {

/* the digital modem's first signal at 8000 Hz, of period 2 LAG
   samples (V.90's Sd, + + + - - -: LAG 6; K56flex's R, + + - -: LAG 4),
   then its reversal: running sums over the last WIN samples of x(n)^2,
   x(n) x(n - LAG) and x(n) x(n - LAG / 2), and over the last LAG of
   the first two (where the reversal is: the lag LAG correlation over
   LAG samples goes to -1 there) */
template <s16 LAG, s16 WIN>
struct ReversalSums {
	double e_win, a_win, h_win;     /* over WIN */
	double e_lag, a_lag;            /* over LAG */

	/* sample i, v, in; at(j) the sample j: whether the sums are
	   over WIN samples yet */
	template <class At>
	bool add(float v, s32 i, At at)
	{
		e_win += static_cast<double>(v) * v;
		e_lag += static_cast<double>(v) * v;
		if (i >= LAG) {
			a_win += static_cast<double>(v) * at(i - LAG);
			a_lag += static_cast<double>(v) * at(i - LAG);
		}
		if (i >= LAG / 2)
			h_win += static_cast<double>(v) * at(i - LAG / 2);
		if (i >= WIN + LAG) {
			float o = at(i - WIN);
			e_win -= static_cast<double>(o) * o;
			a_win -= static_cast<double>(o) * at(i - WIN - LAG);
			h_win -= static_cast<double>(o) * at(i - WIN - LAG / 2);
		}
		if (i >= 2 * LAG) {
			float o = at(i - LAG);
			e_lag -= static_cast<double>(o) * o;
			a_lag -= static_cast<double>(o) * at(i - 2 * LAG);
		}
		return i >= WIN + LAG;
	}
	/* the signal is there, at min_rms or more: correlated at LAG,
	   anticorrelated at LAG / 2 */
	bool there(double min_rms) const
	{
		return e_win > WIN * min_rms * min_rms && a_win > 0.7 * e_win &&
			h_win < -0.5 * e_win;
	}
	/* the lag LAG correlation over LAG samples (the reversal's dip) */
	double r() const { return e_lag > 0 ? a_lag / e_lag : 0; }
	/* and over WIN */
	double q() const { return e_win > 0 ? a_win / e_win : 1; }
};

template <class H, s16 TAPS>
struct PcmEqualizer {
	/* NLMS's step */
	static constexpr float EQ_MU = 0.005f;
	/* the timing loop's gains on the error (normalised to the
	   signal's power) for the phase and the frequency: to lock (the
	   codec's clock found within some 1000 symbols), then TIM_LOCK
	   symbols after the equaliser is solved, with the clock's offset
	   fitted, a fifth of the phase gain (the loop's jitter costs the
	   dense constellations their margin) */
	static constexpr double TIM_KP = 0.02;
	static constexpr double TIM_KI = 2e-5;
	static constexpr s32 TIM_LOCK = 6000;
	static constexpr double TIM_KP2 = 0.004;
	static constexpr double TIM_KI2 = 2e-7;

	float c[TAPS];
	/* least squares: the normal equations, R's Cholesky factor (some
	   250 KB: the receiver is on the heap) */
	double R[TAPS][TAPS], p[TAPS];
	double L[TAPS][TAPS];
	/* the timing loop, from the equaliser's error times its slope
	   (its output a sample either side): the frequency term (16 kHz
	   samples a symbol) */
	s16 t_on;
	s32 t_n;                /* symbols it has followed */
	double t_freq, t_pow;   /* t_pow: the targets' power */
	double t_phase;         /* the phase it has moved (16 kHz samples) */
	double t_sx, t_sy, t_sxx, t_sxy;        /* the line fitted to it */
	s32 t_sn;

	/* the codec's clock against the network's, parts per million */
	double ppm() const { return t_freq / 2 * 1e6; }

	/* the equaliser's output for symbol kk, on its input d samples
	   later */
	float equalize(s32 kk, s16 d = 0) const
	{
		float y = 0;
		for (s16 i = 0; i < TAPS; i++)
			y += c[i] * host().xin(kk, i, d);
		return y;
	}

	/* NLMS on symbol kk's error e */
	void adapt(s32 kk, float e)
	{
		float pw = 0;
		s16 i;
		for (i = 0; i < TAPS; i++) {
			float xi = host().xin(kk, i, 0);
			pw += xi * xi;
		}
		if (pw > 0)
			for (i = 0; i < TAPS; i++)
				c[i] += EQ_MU * e / pw * host().xin(kk, i, 0);
	}

	/* symbol kk's input and its target a into the normal equations
	   (their upper triangle) */
	void ls_add(s32 kk, float a)
	{
		float xv[TAPS];
		s16 i, j;
		for (i = 0; i < TAPS; i++)
			xv[i] = host().xin(kk, i, 0);
		for (i = 0; i < TAPS; i++) {
			p[i] += static_cast<double>(a) * xv[i];
			for (j = i; j < TAPS; j++)
				R[i][j] += static_cast<double>(xv[i]) * xv[j];
		}
	}

	/* the equaliser from R c = p (Cholesky, with a little added to R's
	   diagonal) */
	void solve()
	{
		double w[TAPS];
		lsq_solve(R, p, 1e-4, 1e-9, L, w);
		for (s16 i = 0; i < TAPS; i++)
			c[i] = static_cast<float>(w[i]);
	}

	/* the timing loop on symbol kk's output y against a: the error's
	   gradient in the time of the input, (a - y) (y(+1) - y(-1)), over
	   the signal's power of late, moves the interpolator (its outputs
	   later for a positive one), a phase term and a frequency term
	   (which follows the codec's clock).  From TIM_LOCK / 3 to
	   TIM_LOCK symbols the phase it has moved is fitted to a line,
	   whose slope is the clock's offset far more closely than the
	   frequency term (it then runs on that, with TIM_KP2 and
	   TIM_KI2) */
	void timing(s32 kk, float y, float a)
	{
		double e, dt;
		if (!t_on)
			return;
		t_pow += 0.05 * (static_cast<double>(a) * a - t_pow);
		e = static_cast<double>(a - y) *
			(equalize(kk, 1) - equalize(kk, -1)) /
			(t_pow + 1e-3 * static_cast<double>(host().level) *
					host().level);
		if (e > 0.05)
			e = 0.05;
		if (e < -0.05)
			e = -0.05;
		if (t_n < TIM_LOCK) {
			t_freq += TIM_KI * e;
			dt = TIM_KP * e + t_freq;
		} else {
			t_freq += TIM_KI2 * e;
			dt = TIM_KP2 * e + t_freq;
		}
		timing_step(dt);
	}

	/* a symbol on the timing's frequency alone (no error to go by) */
	void timing_free()
	{
		if (t_on)
			timing_step(t_freq);
	}

	/* the interpolator dt samples (16 kHz) on; the line fitted to the
	   phase */
	void timing_step(double dt)
	{
		host().ip.shift(host().ip.step * dt);
		t_phase += dt;
		t_n++;
		if (t_n > TIM_LOCK / 3 && t_n <= TIM_LOCK) {
			double x = static_cast<double>(t_n);
			t_sx += x;
			t_sy += t_phase;
			t_sxx += x * x;
			t_sxy += x * t_phase;
			t_sn++;
		}
		if (t_n == TIM_LOCK && t_sn > 1) {
			double d = t_sn * t_sxx - t_sx * t_sx;
			double old = ppm();
			if (d > 0)
				t_freq = (t_sn * t_sxy - t_sx * t_sy) / d;
			host().timing_locked(old);
		}
	}

private:
	/* the receiver this is part of */
	H &host() { return static_cast<H &>(*this); }
	/* (see host()) */
	const H &host() const { return static_cast<const H &>(*this); }
};

} // namespace dsp

#endif
