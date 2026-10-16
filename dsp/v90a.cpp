/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v90a.cpp - V.90 analogue modem: the PCM receiver of phase 3
 *               (v90a.h, docs/dsp/v90a.md)
 *
 *    Sd (8.4.4/V.90) repeats +W +0 +W -W -0 -W: the line has x(n) close
 *    to x(n - 6) and to -x(n - 3).  Its reversal to Sd-bar is where, for
 *    6 samples, x(n) is -x(n - 6): symbol 0 of S-bar d, in data frame
 *    interval 0 (Sd's 384 symbols are whole frames).  TRN1d follows 48
 *    symbols later, UINFO's codeword with the signs of the GPC
 *    scrambler on ones from zero (8.4.5/V.90): known, so a T-spaced
 *    equaliser is solved on its first symbols by least squares (after
 *    its delay is centred on the correlation with them), then follows
 *    its decisions.  Jd and Jd' are the decisions' signs, differentially
 *    decoded and descrambled (8.4.2, 8.4.3/V.90).
 *
 */

#include <math.h>
#include <string.h>

#include "bits.h"
#include "dsp.h"
#include "v90.h"
#include "v90a.h"

namespace dsp {

/* Sd: the window of its sums, how long it is there before we look for
   its reversal */
#define SD_WIN          24
#define SD_MIN_RUN      96
/* below this level (16 bit scale) nothing is there */
#define SD_MIN_RMS      30.0
/* the equaliser's centre: symbol k's sample b + k at tap D */
#define EQ_D            (V90RX_TAPS / 2)
/* TRN1d: its first symbol, the symbols the delay is centred on, how
   far it is looked for, and its least length (9.3.2.5/V.90) */
#define TRN1D_K0        48
#define REFINE_N        96
#define REFINE_D        10
#define TRN1D_MIN       2040
/* the equaliser's tracking (NLMS) step */
#define EQ_MU           0.005f

/* see v90a.h */
int V90Jd::parse(const unsigned char *b)
{
	int i;
	for (i = 0; i < 17; i++)
		if (!b[i])
			return -1;
	if (b[17] || b[34] || b[51] || b[68] || b[69] || b[70] || b[71] ||
	    !frame_crc_ok(b, 52))
		return -1;
	rate_mask = (unsigned)field(b, 18, 16) |
		((unsigned)field(b, 35, 7) << 16);
	cp16_train = b[47];
	cp16_reneg = b[48];
	lookahead = field(b, 49, 2);
	return 0;
}

/* see v90a.h */
void V90Rx::init(int alaw_, int uinfo_)
{
	int i;
	memset(this, 0, sizeof(*this));
	alaw = alaw_;
	uinfo = uinfo_;
	for (i = 0; i < 128; i++)
		lin[i] = v90_ucode_linear(alaw_, i);
	level = (float)lin[uinfo & 127];
	stage = WAIT_SD;
	dip = 1;
}

/* the sums of Sd's detector with the newest sample v (x[n - 1]); Sd
   there for SD_MIN_RUN samples, then its reversal: the least of
   a6_6 / e6 once it has gone below -0.5 and come back */
void V90Rx::sd_detect(float v)
{
	long i = n - 1;
	double r;
	int there;

	e24 += (double)v * v;
	e6 += (double)v * v;
	if (i >= 6) {
		a6_24 += (double)v * at(i - 6);
		a6_6 += (double)v * at(i - 6);
	}
	if (i >= 3)
		a3_24 += (double)v * at(i - 3);
	if (i >= SD_WIN + 6) {
		float o = at(i - SD_WIN);
		e24 -= (double)o * o;
		a6_24 -= (double)o * at(i - SD_WIN - 6);
		a3_24 -= (double)o * at(i - SD_WIN - 3);
	}
	if (i >= 12) {
		float o = at(i - 6);
		e6 -= (double)o * o;
		a6_6 -= (double)o * at(i - 12);
	}
	if (i < SD_WIN + 6)
		return;

	there = e24 > SD_WIN * SD_MIN_RMS * SD_MIN_RMS &&
		a6_24 > 0.7 * e24 && a3_24 < -0.5 * e24;
	if (stage == WAIT_SD) {
		sd_run = there ? sd_run + 1 : 0;
		if (sd_run >= SD_MIN_RUN) {
			stage = SD;
			ev_sd = 1;
			dip = 1;
			DSP_DBG("v90a: Sd at sample %ld\n", i);
		}
		return;
	}
	r = e6 > 0 ? a6_6 / e6 : 0;
	if (r < dip) {
		dip = r;
		dip_at = i;
	}
	if (dip < -0.5 && r > -0.2) {
		/* the 6 samples after the reversal end at dip_at */
		b = dip_at - 5;
		k = 0;
		stage = TRAIN;
		ev_sdbar = 1;
		DSP_DBG("v90a: Sd-bar at sample %ld (%.2f)\n", b, dip);
		return;
	}
	/* Sd gone without its reversal */
	sd_run = there ? sd_run + 1 : sd_run - 1;
	if (sd_run < 0) {
		stage = WAIT_SD;
		sd_run = 0;
		DSP_DBG("v90a: Sd lost\n");
	}
	if (sd_run > SD_MIN_RUN)
		sd_run = SD_MIN_RUN;
}

/* the equaliser's output for symbol kk */
float V90Rx::equalize(long kk) const
{
	float y = 0;
	int i;
	for (i = 0; i < V90RX_TAPS; i++)
		y += c[i] * at(b + kk + EQ_D - i);
	return y;
}

/* the delay of symbol k's samples centred: the shift d of b that
   correlates best with TRN1d's first REFINE_N signs (either polarity) */
void V90Rx::refine()
{
	unsigned scr = 0;
	signed char a[REFINE_N];
	double best = -1;
	int d, j, bd = 0;

	for (j = 0; j < REFINE_N; j++)
		a[j] = scramble(&scr, SCR_GPC, 1) ? 1 : -1;
	for (d = -REFINE_D; d <= REFINE_D; d++) {
		double cc = 0;
		for (j = 0; j < REFINE_N; j++)
			cc += a[j] * at(b + TRN1D_K0 + j + d);
		if (fabs(cc) > best) {
			best = fabs(cc);
			bd = d;
		}
	}
	b += bd;
	refined = 1;
	DSP_DBG("v90a: TRN1d's delay %+d samples\n", bd);
}

/* the equaliser from the normal equations R c = p (Cholesky, with a
   little added to R's diagonal) */
void V90Rx::solve()
{
	double z[V90RX_TAPS], tr = 0, lam;
	int i, j, m;

	for (i = 0; i < V90RX_TAPS; i++)
		tr += R[i][i];
	lam = 1e-4 * tr / V90RX_TAPS + 1e-9;
	for (i = 0; i < V90RX_TAPS; i++) {
		for (j = 0; j <= i; j++) {
			double s = (j < i ? R[j][i] : R[i][i] + lam);
			for (m = 0; m < j; m++)
				s -= L[i][m] * L[j][m];
			if (i == j)
				L[i][i] = sqrt(s > 1e-12 ? s : 1e-12);
			else
				L[i][j] = s / L[j][j];
		}
	}
	for (i = 0; i < V90RX_TAPS; i++) {
		double s = p[i];
		for (m = 0; m < i; m++)
			s -= L[i][m] * z[m];
		z[i] = s / L[i][i];
	}
	for (i = V90RX_TAPS - 1; i >= 0; i--) {
		double s = z[i];
		for (m = i + 1; m < V90RX_TAPS; m++)
			s -= L[m][i] * z[m];
		z[i] = s / L[i][i];
	}
	for (i = 0; i < V90RX_TAPS; i++)
		c[i] = (float)z[i];
}

/* a Jd bit: the last 72 a Jd frame?  After one, Jd' is 12 zeros where
   the next frame's sync would be ones */
void V90Rx::jd_bit(int bit)
{
	unsigned char f[72];
	int i, ones = 0;

	jb[jpos] = (unsigned char)bit;
	jpos = (jpos + 1) % 72;
	jbits++;
	if (ev_jd && !ev_jdp && jbits - jd_end == 8) {
		for (i = 0; i < 8; i++)
			ones += jb[(jpos + 64 + i) % 72];
		if (ones <= 1) {
			ev_jdp = 1;
			DSP_DBG("v90a: Jd' after %d Jd\n", ev_jd);
		}
	}
	if (jbits < 72)
		return;
	for (i = 0; i < 72; i++)
		f[i] = jb[(jpos + i) % 72];
	if (jd.parse(f) == 0) {
		if (!ev_jd)
			DSP_DBG("v90a: Jd: rates %06x, %s points in training, "
				"%s in renegotiation, look-ahead %d\n",
				jd.rate_mask, jd.cp16_train ? "16" : "4",
				jd.cp16_reneg ? "16" : "4", jd.lookahead);
		ev_jd++;
		jd_end = jbits;
	}
}

/* a decision's sign into Jd's bits (differential, then descrambled) */
void V90Rx::decide(float y)
{
	int s = y > 0;
	int bit = descramble(&dscr, SCR_GPC, s ^ last_sign);
	last_sign = s;
	jd_bit(bit);
}

/* see v90a.h */
void V90Rx::sample(float v)
{
	x[n & (V90RX_HIST - 1)] = v;
	n++;
	if (stage == WAIT_SD || stage == SD) {
		sd_detect(v);
		return;
	}
	/* symbol k's samples are in */
	while (b + k + EQ_D <= n - 1) {
		long t = k - TRN1D_K0;
		float a = 0, y;
		int i;

		if (t >= 0)
			a = scramble(&trn_scr, SCR_GPC, 1) ? level : -level;
		if (t == REFINE_N && !refined)
			refine();
		if (stage == TRAIN && t > REFINE_N) {
			/* the normal equations on TRN1d */
			float xv[V90RX_TAPS];
			int j;
			for (i = 0; i < V90RX_TAPS; i++)
				xv[i] = at(b + k + EQ_D - i);
			for (i = 0; i < V90RX_TAPS; i++) {
				p[i] += (double)a * xv[i];
				for (j = i; j < V90RX_TAPS; j++)
					R[i][j] += (double)xv[i] * xv[j];
			}
			if (t == REFINE_N + V90RX_LS) {
				solve();
				stage = DECIDE;
				ev_trained = 1;
				err_sum = 0;
				err_n = 0;
			}
			k++;
			continue;
		}
		if (stage != DECIDE) {
			k++;
			continue;
		}
		y = equalize(k);
		/* TRN1d known up to its least length; after it our
		   decisions */
		if (t >= TRN1D_MIN)
			a = y > 0 ? level : -level;
		{
			float e = a - y, pw = 0;
			for (i = 0; i < V90RX_TAPS; i++) {
				float xi = at(b + k + EQ_D - i);
				pw += xi * xi;
			}
			if (pw > 0)
				for (i = 0; i < V90RX_TAPS; i++)
					c[i] += EQ_MU * e / pw *
						at(b + k + EQ_D - i);
			err_sum += (double)e * e;
			err_n++;
			if (err_n == 512) {
				mse = (float)(err_sum / err_n /
					      ((double)level * level));
				if (t < TRN1D_MIN || !ev_jd)
					DSP_DBG("v90a: equaliser mse %.4f "
						"(%.1f dB) at symbol %ld\n",
						mse, -10 * log10(mse + 1e-12),
						k);
				err_sum = 0;
				err_n = 0;
			}
		}
		decide(y);
		k++;
	}
}

} // namespace dsp
