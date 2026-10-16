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
 *    decoded and descrambled (8.4.2, 8.4.3/V.90).  The DIL (8.4.1/V.90)
 *    follows Jd' in data frame interval 0; with the equaliser still,
 *    each symbol's value times its sign (SP) goes to its frame
 *    interval's and Ucode's sums.
 *
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bits.h"
#include "dsp.h"
#include "g711.h"
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
			/* this is Jd''s 8th symbol: the DIL's first is 5
			   on, in data frame interval 0 */
			dil_start = k + 5;
			if (dil_start % 6) {
				DSP_DBG("v90a: Jd' %ld symbols off its "
					"frame\n", dil_start % 6);
				dil_start = (dil_start + 3) / 6 * 6;
			}
			DSP_DBG("v90a: Jd' after %d Jd, DIL from symbol %ld\n",
				ev_jd, dil_start);
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
void V90Rx::dil_set(const V90Dil &d)
{
	int i;
	dil = d;
	dil_on = d.n > 0;
	dil_len = 0;
	for (i = 0; i < d.n; i++)
		dil_len += (d.h[d.ucode[i] >> 4] + 1) * 6;
}

/* a DIL symbol's value y: to its frame interval's and Ucode's sums,
   times its sign; the segments and rounds counted on */
static inline void dil_add(V90Rx *r, float y)
{
	const V90Dil *d = &r->dil;
	int u = d->ucode[r->dil_seg], c = u >> 4;
	int len = (d->h[c] + 1) * 6, k = r->dil_k, j = (int)(r->k % 6);
	float s = d->sp[k % d->lsp] ? 1.0f : -1.0f;

	if (!d->tp[k % d->ltp])
		u = d->ref[c];
	r->dsum[j][u] += s * y;
	r->dsq[j][u] += (double)y * y;
	r->dcnt[j][u]++;
	if (++r->dil_k >= len) {
		r->dil_k = 0;
		if (++r->dil_seg >= d->n) {
			r->dil_seg = 0;
			r->ev_dil++;
		}
	}
}

/* a value's place in a sorted array of floats (qsort()) */
static int cmp_float(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y;
}

/* the median of n values (sorted in place); 0 for none */
static float median(float *v, int n)
{
	if (!n)
		return 0;
	qsort(v, n, sizeof(*v), cmp_float);
	return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

/* the levels Ucodes 0..127 have through a digital loss pad of db
   (each code requantized that much lower), as our equaliser scales them
   (UINFO through it to lin[UINFO]) */
static void pad_levels(int alaw, const int *lin, int uinfo, float db,
		       float *out)
{
	double g = pow(10, -db / 20), ref;
	int u;
	for (u = 0; u < 128; u++)
		out[u] = (float)g711::decode(alaw, g711::encode(alaw,
				(int)lrint(lin[u] * g)));
	ref = out[uinfo] > 0 ? lin[uinfo] / out[uinfo] : 1;
	for (u = 0; u < 128; u++)
		out[u] = (float)(out[u] * ref);
}

/* see v90a.h: the levels and their noise (each Ucode's; the median of
   them); a Ucode unusable whose noise is more than 4 times that and
   0.2% of its level (clipped).  A frame interval robbed when 7 in 10 of
   the Ucode pairs (2m, 2m + 1, or 2m + 1, 2m + 2: whichever the law's
   octet makes) that the noise lets tell apart come out alike, or it
   tells less than 3/4 as many levels apart as the best.  The pad:
   of 0 to 9 dB in 0.5 dB steps, the one whose levels fit those of the
   intervals not robbed best (their relative errors, on the levels 10
   times the noise and more) */
void V90Rx::dil_analyse()
{
	float r[6 * 128], nz[6][128], lv[128], pl[128];
	double best = -1;
	char rb[16] = "none";
	int j, u, m, nr = 0, nm, pd, most = 0;

	memset(dil_level, 0, sizeof(dil_level));
	memset(dil_usable, 0, sizeof(dil_usable));
	for (j = 0; j < 6; j++)
		for (u = 0; u < 128; u++) {
			int n_ = dcnt[j][u];
			double mu, var;
			nz[j][u] = 0;
			if (!n_)
				continue;
			mu = dsum[j][u] / n_;
			var = dsq[j][u] / n_ - mu * mu;
			dil_level[j][u] = (float)mu;
			nz[j][u] = (float)sqrt(var > 0 ? var : 0);
			r[nr++] = nz[j][u];
		}
	nm = nr;
	dil_noise = median(r, nr);
	if (dsplibs_debug_level >= 5)
		for (u = 0; u < 128; u += 4)
			DSP_DBG("v90a: DIL u %3d lin %6d: %8.1f %8.1f %8.1f %8.1f "
				"%8.1f %8.1f (noise %6.1f)\n", u, lin[u],
				dil_level[0][u], dil_level[1][u],
				dil_level[2][u], dil_level[3][u],
				dil_level[4][u], dil_level[5][u], nz[0][u]);
	for (j = 0; j < 6; j++)
		for (u = 0; u < 128; u++)
			dil_usable[j][u] = dcnt[j][u] &&
				nz[j][u] <= 4 * dil_noise + 0.002f * lin[u];

	for (j = 0; j < 6; j++) {
		int n_ = 0, i;
		for (u = 0; u < 128; u++)
			if (dil_usable[j][u])
				lv[n_++] = dil_level[j][u];
		qsort(lv, n_, sizeof(*lv), cmp_float);
		dil_distinct[j] = n_ ? 1 : 0;
		for (i = 1, m = 0; i < n_; i++)
			if (lv[i] - lv[m] > 3 * dil_noise) {
				dil_distinct[j]++;
				m = i;
			}
	}
	for (j = 0; j < 6; j++)
		if (dil_distinct[j] > most)
			most = dil_distinct[j];

	dil_robbed = 0;
	for (j = 0; j < 6; j++) {
		int off;
		/* (behind a pad the codes the robbed bit makes alike are
		   not neighbouring Ucodes: half the levels are left) */
		if (dil_distinct[j] * 4 < most * 3)
			dil_robbed |= 1 << j;
		for (off = 0; off < 2; off++) {
			int pairs = 0, alike = 0;
			for (m = 0; 2 * m + off + 1 < 128; m++) {
				int a = 2 * m + off, b_ = a + 1;
				float step = (float)(lin[b_] - lin[a]);
				if (!dil_usable[j][a] || !dil_usable[j][b_] ||
				    step < 4 * dil_noise)
					continue;
				pairs++;
				if (fabsf(dil_level[j][b_] - dil_level[j][a]) <
				    0.3f * step)
					alike++;
			}
			if (pairs >= 8 && alike * 10 >= pairs * 7)
				dil_robbed |= 1 << j;
		}
	}

	dil_pad_db = 0;
	for (pd = 0; pd <= 18; pd++) {
		double e = 0;
		pad_levels(alaw, lin, uinfo, pd * 0.5f, pl);
		for (j = 0; j < 6; j++) {
			if (dil_robbed & (1 << j))
				continue;
			for (u = 1; u < 128; u++)
				if (dil_usable[j][u] &&
				    pl[u] > 10 * dil_noise) {
					double d = (dil_level[j][u] - pl[u]) /
						pl[u];
					e += d * d;
				}
		}
		if (best < 0 || e < best) {
			best = e;
			dil_pad_db = pd * 0.5f;
		}
	}

	for (j = 0, u = 0; j < 6; j++)
		for (m = 0; m < 128; m++)
			u += dcnt[j][m] && !dil_usable[j][m];
	if (dil_robbed)
		for (j = 0, m = 0; j < 6; j++)
			if (dil_robbed & (1 << j)) {
				m += snprintf(rb + m, sizeof(rb) - m, "%s%d",
					      m ? "," : "", j);
			}
	DSP_DBG("v90a: DIL: pad %.1f dB, noise %.1f (%.1f dB below UINFO), "
		"%d of %d unusable, robbed %s, distinct levels %d "
		"%d %d %d %d %d\n", dil_pad_db, dil_noise,
		20 * log10f(level / (dil_noise + 1e-6f)), u, nm, rb,
		dil_distinct[0],
		dil_distinct[1], dil_distinct[2], dil_distinct[3],
		dil_distinct[4], dil_distinct[5]);
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
		/* after Jd': the equaliser still, the DIL measured */
		if (ev_jdp) {
			if (dil_on && k >= dil_start &&
			    ev_dil < V90RX_DIL_ROUNDS)
				dil_add(this, y);
			k++;
			continue;
		}
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
