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
 *    Phase 4: Ri, + + + - - - of UINFO in each data frame, until our
 *    CPt is in, Ri-bar (24 symbols), then TRN2d, MP sequences and Ed
 *    from the data mode encoder on CPt's constellations, B1d on CP's
 *    (8.6/V.90): decoded here as the encoder makes them for Sr 0, no
 *    spectral shaping.
 *
 */

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dm_types.h"
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
#define EQ_D            V90RX_EQ_D
/* the symbols of UINFO's spread in each interval (noise()), after the
   second solution */
#define NOISE_N         512
/* TRN1d: its first symbol, the symbols the delay is centred on, how
   far it is looked for, and its least length (9.3.2.5/V.90) */
#define TRN1D_K0        48
#define REFINE_N        96
#define REFINE_D        20      /* (16 kHz samples) */
#define TRN1D_MIN       2040
/* data mode: a point's level a step towards each value decided as it */
#define LEV_MU          0.01f
/* the equaliser's tracking (NLMS) step */
#define EQ_MU           0.005f
/* the timing loop's gains on the error (normalised to the signal's
   power) for the phase and the frequency: to lock (the codec's clock
   found within some 1000 symbols), then TIM_LOCK symbols after the
   equaliser is solved, with the clock's offset fitted, a fifth of the
   phase gain (the loop's jitter costs the dense constellations their
   margin) */
#define TIM_KP          0.02
#define TIM_KI          2e-5
#define TIM_LOCK        6000
#define TIM_KP2         0.004
#define TIM_KI2         2e-7

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
	rate_mask = static_cast<u32>(field(b, 18, 16)) |
		(static_cast<u32>(field(b, 35, 7)) << 16);
	cp16_train = b[47];
	cp16_reneg = b[48];
	lookahead = field(b, 49, 2);
	return 0;
}

/* see v90a.h: a Blackman windowed sinc over +-V90_INTERP_T samples,
   at V90_INTERP_P phases between two samples */
void V90Interp::init(int rate)
{
	int p, j;
	reinit(*this);
	step = rate == 8000 ? 0.5 : 0.6;
	for (p = 0; p <= V90_INTERP_P; p++)
		for (j = 0; j < 2 * V90_INTERP_T; j++) {
			/* the output's time less input j's (from the
			   oldest of those it takes) */
			double u = static_cast<double>(p) / V90_INTERP_P +
				   V90_INTERP_T - 1 - j,
			       x = M_PI * u / V90_INTERP_T;
			double h = fabs(u) < 1e-12 ? 1 : sin(M_PI * u) /
				(M_PI * u);
			g[p][j] = fabs(u) >= V90_INTERP_T ?
					  0 :
					  static_cast<float>(
						  h * (0.42 + 0.5 * cos(x) +
						       0.08 * cos(2 * x)));
		}
	/* (the inputs it needs before the first output) */
	t = V90_INTERP_T;
}

/* see v90a.h: outputs step input samples apart, each from the inputs
   either side of its time */
int V90Interp::run(float x, float *out)
{
	const int L = 2 * V90_INTERP_T;
	int m = 0;
	hist[nin % L] = hist[nin % L + L] = x;
	nin++;
	while (t + V90_INTERP_T <= nin - 1 && m < 3) {
		s32 n0 = static_cast<s32>(floor(t));
		double fp = (t - n0) * V90_INTERP_P;
		int p = static_cast<int>(fp), j;
		float w = static_cast<float>(fp - p), y = 0;
		const float *bp = hist + (n0 - V90_INTERP_T + 1) % L;
		const float *g0 = g[p], *g1 = g[p + 1];
		for (j = 0; j < L; j++)
			y += (g0[j] + w * (g1[j] - g0[j])) * bp[j];
		out[m++] = y;
		t += step;
	}
	return m;
}

/* see v90a.h */
void V90Rx::init(int alaw_, int uinfo_, int rate)
{
	int i;
	reinit(*this);
	alaw = alaw_;
	uinfo = uinfo_;
	for (i = 0; i < 128; i++)
		lin[i] = v90_ucode_linear(alaw_, i);
	level = static_cast<float>(lin[uinfo & 127]);
	for (i = 0; i < 6; i++) {
		tlev[i] = level;
		tmag[i] = level;
	}
	ip.init(rate);
	stage = WAIT_SD;
	dip = 1;
}

/* the sums of Sd's detector with the newest sample v (x8[n8 - 1], at
   8000 Hz); Sd
   there for SD_MIN_RUN samples, then its reversal: the least of
   a6_6 / e6 once it has gone below -0.5 and come back */
void V90Rx::sd_detect(float v)
{
	s32 i = n8 - 1;
	double r, q;
	int there;

	e24 += static_cast<double>(v) * v;
	e6 += static_cast<double>(v) * v;
	if (i >= 6) {
		a6_24 += static_cast<double>(v) * at8(i - 6);
		a6_6 += static_cast<double>(v) * at8(i - 6);
	}
	if (i >= 3)
		a3_24 += static_cast<double>(v) * at8(i - 3);
	if (i >= SD_WIN + 6) {
		float o = at8(i - SD_WIN);
		e24 -= static_cast<double>(o) * o;
		a6_24 -= static_cast<double>(o) * at8(i - SD_WIN - 6);
		a3_24 -= static_cast<double>(o) * at8(i - SD_WIN - 3);
	}
	if (i >= 12) {
		float o = at8(i - 6);
		e6 -= static_cast<double>(o) * o;
		a6_6 -= static_cast<double>(o) * at8(i - 12);
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
			DSP_DBG("v90a: Sd at sample %" PRId32 "\n", i);
		}
		return;
	}
	/* the reversal: over 24 samples the lag 6 correlation falls to
	   about 0.5 (the 6 after the reversal against the 6 before) at any
	   phase; within that dip, where over 6 it is least (sampled off
	   the symbols' instants the reversal spreads, and that least is
	   -1 only on them) */
	r = e6 > 0 ? a6_6 / e6 : 0;
	q = e24 > 0 ? a6_24 / e24 : 1;
	if (!sd_dip && q < 0.8) {
		sd_dip = 1;
		dip = 1;
	}
	if (sd_dip && r < dip) {
		dip = r;
		dip_at = i;
	}
	if (sd_dip && q > 0.9 && dip > -0.2) {
		/* the lag 6 correlation never went the other way: not the
		   reversal (an echo of ours over Sd, as after a hold), on
		   to the next dip */
		DSP_DBG("v90a: not Sd-bar at sample %" PRId32 " (%.2f)\n",
			2 * (dip_at - 5), dip);
		sd_dip = 0;
		dip = 1;
		return;
	}
	if (sd_dip && q > 0.9) {
		/* the 6 samples after the reversal end at dip_at (8000
		   Hz: every other sample) */
		b = 2 * (dip_at - 5);
		k = 0;
		stage = TRAIN;
		ev_sdbar = 1;
		DSP_DBG("v90a: Sd-bar at sample %" PRId32 " (%.2f)\n", b, dip);
		return;
	}
	if (sd_dip)
		return;
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

/* the equaliser's output for symbol kk, on its input d samples later */
float V90Rx::equalize(s32 kk, int d) const
{
	float y = 0;
	int i;
	for (i = 0; i < V90RX_TAPS; i++)
		y += c[i] * xin(kk, i, d);
	return y;
}

/* the timing loop on symbol kk's output y against a: the error's
   gradient in the time of the input, (a - y) (y(+1) - y(-1)), over the
   signal's power of late (the DIL's codes and the data's are up to 64
   times UINFO's), moves the interpolator (its outputs later for a
   positive one), a phase term and a frequency term (which follows the
   codec's clock).  From TIM_LOCK / 3 to TIM_LOCK symbols the phase it
   has moved is fitted to a line, whose slope is the clock's offset far
   more closely than the frequency term (it then runs on that, with
   TIM_KP2 and TIM_KI2) */
void V90Rx::timing(s32 kk, float y, float a)
{
	double e, dt;
	if (!t_on)
		return;
	t_pow += 0.05 * (static_cast<double>(a) * a - t_pow);
	e = static_cast<double>(a - y) * (equalize(kk, 1) - equalize(kk, -1)) /
		(t_pow + 1e-3 * static_cast<double>(level) * level);
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

/* a symbol on the timing's frequency alone (no error to go by: the
   DIL's levels are what it measures) */
void V90Rx::timing_free()
{
	if (t_on)
		timing_step(t_freq);
}

/* the interpolator dt samples (16 kHz) on; the line fitted to the
   phase */
void V90Rx::timing_step(double dt)
{
	ip.shift(ip.step * dt);
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
		DSP_DBG("v90a: timing %+.2f ppm (the loop's %+.2f)\n", ppm(),
			old);
	}
}

/* the delay of symbol k's samples centred: the shift d of b that
   correlates best with TRN1d's first REFINE_N signs (either polarity) */
void V90Rx::refine()
{
	u32 scr = 0;
	signed char a[REFINE_N];
	double best = -1;
	int d, j, bd = 0;

	for (j = 0; j < REFINE_N; j++)
		a[j] = scramble(&scr, SCR_GPC, 1) ? 1 : -1;
	for (d = -REFINE_D; d <= REFINE_D; d++) {
		double cc = 0;
		for (j = 0; j < REFINE_N; j++)
			cc += a[j] * at(b + 2 * (TRN1D_K0 + j) + d);
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
		c[i] = static_cast<float>(z[i]);
}

/* a Jd bit: the last 72 a Jd frame?  After one, Jd' is 12 zeros where
   the next frame's sync would be ones */
void V90Rx::jd_bit(int bit)
{
	unsigned char f[72];
	int i, ones = 0;
	V90Jd jp;

	jb[jpos] = static_cast<unsigned char>(bit);
	jpos = (jpos + 1) % 72;
	jbits++;
	if (!ev_jdp && (v92 ? ev_jp && jbits - jp_end == 8 :
			ev_jd && jbits - jd_end == 8)) {
		for (i = 0; i < 8; i++)
			ones += jb[(jpos + 64 + i) % 72];
		if (ones <= 1) {
			ev_jdp = 1;
			/* this is Jd''s 8th symbol: the DIL's first is 5
			   on, in data frame interval 0 */
			dil_start = k + 5;
			if (dil_start % 6) {
				DSP_DBG("v90a: Jd' %" PRId32 " symbols off its "
					"frame\n", dil_start % 6);
				dil_start = (dil_start + 3) / 6 * 6;
			}
			DSP_DBG("v90a: %s after %d %s, DIL from symbol %"
				PRId32 "\n", v92 ? "Jp'" : "Jd'",
				v92 ? ev_jp : ev_jd, v92 ? "Jp" : "Jd",
				dil_start);
		}
	}
	if (jbits < 72)
		return;
	for (i = 0; i < 72; i++)
		f[i] = jb[(jpos + i) % 72];
	if (v92 && f[47] && ev_jd && jp.parse(f) == 0) {
		/* (its frame as Jd's, the fields its own) */
		if (!ev_jp) {
			jp_frac = field(f, 18, 16) / 65536.0;
			jp_con = f[48];
			jp_con_rn = f[49];
			DSP_DBG("v90a: Jp: Su-bar %.4f symbol longer, %d "
				"points in training, %d in renegotiation\n",
				jp_frac, jp_con ? 8 : 4, jp_con_rn ? 8 : 4);
		}
		ev_jp++;
		jp_end = jbits;
		return;
	}
	if ((!v92 || !f[47]) && jd.parse(f) == 0) {
		if (v92)
			jd.cp16_train = jd.cp16_reneg = 0;
		if (!ev_jd)
			DSP_DBG("v90a: Jd: rates %06" PRIx32 ", %s points in "
				"training, %s in renegotiation, look-ahead "
				"%d\n",
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
	int len = (d->h[c] + 1) * 6, k = r->dil_k,
	    j = static_cast<int>(r->k % 6);
	float s = d->sp[k % d->lsp] ? 1.0f : -1.0f;

	if (!d->tp[k % d->ltp])
		u = d->ref[c];
	r->dsum[j][u] += s * y;
	r->dsq[j][u] += static_cast<double>(y) * y;
	r->dcnt[j][u]++;
	if (++r->dil_k >= len) {
		r->dil_k = 0;
		if (++r->dil_seg >= d->n) {
			r->dil_seg = 0;
			r->ev_dil++;
		}
	}
}

/* the median of n values (sorted in place); 0 for none */
static float median(float *v, int n)
{
	if (!n)
		return 0;
	std::sort(v, v + n);
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
		out[u] = static_cast<float>(g711::decode(
			alaw, g711::encode(alaw, static_cast<int>(
							 lrint(lin[u] * g)))));
	ref = out[uinfo] > 0 ? lin[uinfo] / out[uinfo] : 1;
	for (u = 0; u < 128; u++)
		out[u] = static_cast<float>(out[u] * ref);
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

	zero(dil_level);
	zero(dil_usable);
	for (j = 0; j < 6; j++)
		for (u = 0; u < 128; u++) {
			s32 n_ = dcnt[j][u];
			double mu, var;
			nz[j][u] = 0;
			if (!n_)
				continue;
			mu = dsum[j][u] / n_;
			var = dsq[j][u] / n_ - mu * mu;
			dil_level[j][u] = static_cast<float>(mu);
			nz[j][u] = static_cast<float>(sqrt(var > 0 ? var : 0));
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
		std::sort(lv, lv + n_);
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
				float step =
					static_cast<float>(lin[b_] - lin[a]);
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
void V90Rx::line(float v)
{
	float o[3];
	int m = ip.run(v, o), i;
	for (i = 0; i < m; i++)
		sample(o[i]);
}

/* see v90a.h */
void V90Rx::sample(float v)
{
	x[n & (V90RX_HIST - 1)] = v;
	n++;
	if (stage == WAIT_SD || stage == SD) {
		/* Sd at 8000 Hz: it repeats every 6 symbols at any phase */
		if (n & 1) {
			x8[n8 & (V90RX_HIST8 - 1)] = v;
			n8++;
			sd_detect(v);
		}
		return;
	}
	/* symbol k's samples are in */
	while (b + 2 * k + EQ_D <= n - 1) {
		s32 t = k - TRN1D_K0;
		float a = 0, y;
		int i;

		if (t >= 0)
			a = scramble(&trn_scr, SCR_GPC, 1) ? tlev[k % 6] :
				-tlev[k % 6];
		if (t == REFINE_N && !refined)
			refine();
		if (stage == TRAIN && t > REFINE_N) {
			/* the normal equations on TRN1d */
			float xv[V90RX_TAPS];
			int j;
			for (i = 0; i < V90RX_TAPS; i++)
				xv[i] = xin(k, i);
			for (i = 0; i < V90RX_TAPS; i++) {
				p[i] += static_cast<double>(a) * xv[i];
				for (j = i; j < V90RX_TAPS; j++)
					R[i][j] += static_cast<double>(xv[i]) *
						   xv[j];
			}
			if (t == REFINE_N + V90RX_LS) {
				solve();
				stage = DECIDE;
				ev_trained = 1;
				t_on = 1;
				t_pow = static_cast<double>(level) * level;
				err_sum = 0;
				err_n = 0;
			}
			k++;
			continue;
		}
		if (stage == TRAIN) {
			k++;
			continue;
		}
		y = equalize(k);
		/* phase 4: Ri, its reversal (the equaliser follows them,
		   UINFO's), then data frames */
		if (stage == RI && ev_ribar && k >= frames_at)
			stage = FRAMES;
		if (stage == RI) {
			int j = static_cast<int>(k % 6);
			rs[j] = y > 0;
			if (ev_ri) {
				float a4 = y > 0 ? tlev[j] : -tlev[j];
				adapt(k, a4 - y);
				timing(k, y, a4);
			} else
				timing_free();
			if (j == 5)
				ri_frame(rs);
			k++;
			continue;
		}
		if (stage == RD) {
			/* Rd-bar's symbols: the timing on its frequency */
			timing_free();
			if (k + 1 >= frames_at && k % 6 == 5)
				reneg_frames();
			k++;
			continue;
		}
		if (stage == FRAMES) {
			fy[k % 6] = y;
			if (k % 6 == 5)
				frame();
			k++;
			continue;
		}
		/* after Jd': the equaliser still, the DIL measured (the
		   timing follows what it measured so far) */
		if (ev_jdp) {
			if (dil_on && k >= dil_start &&
			    ev_dil < V90RX_DIL_ROUNDS)
				dil_add(this, y);
			timing_free();
			k++;
			continue;
		}
		/* TRN1d known up to its least length; after it our
		   decisions */
		if (t >= TRN1D_MIN)
			a = y > 0 ? tlev[k % 6] : -tlev[k % 6];
		/* each position's level as the equaliser gives it: the
		   targets follow, against the loudest's (an equaliser the
		   same for all six cannot make one interval's level its
		   own, so what it gives there is the line's) */
		{
			int j = static_cast<int>(k % 6), i2;
			double mx = 0;
			tmag[j] += 0.01 * (fabsf(y) - tmag[j]);
			for (i2 = 0; i2 < 6; i2++)
				if (tmag[i2] > mx)
					mx = tmag[i2];
			for (i2 = 0; i2 < 6; i2++)
				tlev[i2] = static_cast<float>(level * tmag[i2] /
							      mx);
		}
		/* once the timing has locked: the equaliser solved again on
		   what is known (TRN1d's first solution spans a drifting
		   clock), then UINFO's spread in each interval */
		if (!ev_ready && t_n >= TIM_LOCK && ls2_n < V90RX_LS) {
			int j;
			if (!ls2_n) {
				zero(R);
				zero(p);
			}
			for (i = 0; i < V90RX_TAPS; i++) {
				float xi = xin(k, i);
				p[i] += static_cast<double>(a) * xi;
				for (j = i; j < V90RX_TAPS; j++)
					R[i][j] += static_cast<double>(xi) *
						   xin(k, j);
			}
			if (++ls2_n == V90RX_LS) {
				solve();
				DSP_DBG("v90a: equaliser solved again at symbol "
					"%" PRId32 ", timing %+.1f ppm\n", k,
					ppm());
				y = equalize(k);
			}
		} else if (!ev_ready && ls2_n == V90RX_LS) {
			int j = static_cast<int>(k % 6);
			usum[j] += fabsf(y);
			usq[j] += static_cast<double>(y) * y;
			un[j]++;
			if (un[0] >= NOISE_N / 6 && un[5] >= NOISE_N / 6) {
				ev_ready = 1;
				DSP_DBG("v90a: PCM receiver ready at symbol %"
					PRId32 ": noise %.2f\n", k, noise());
			}
		}
		{
			float e = a - y;
			adapt(k, e);
			timing(k, y, a);
			err_sum += static_cast<double>(e) * e;
			err_n++;
			if (err_n == 512) {
				mse = static_cast<float>(
					err_sum / err_n /
					(static_cast<double>(level) * level));
				if (t < TRN1D_MIN || !ev_jd)
					DSP_DBG("v90a: equaliser mse %.4f "
						"(%.1f dB) at symbol %" PRId32
						", timing %+.1f ppm\n", mse,
						-10 * log10(mse + 1e-12), k,
						ppm());
				err_sum = 0;
				err_n = 0;
			}
		}
		decide(y);
		k++;
	}
}

/*
 *    phase 4 (9.4.2/V.90)
 */

/* see v90a.h: Table 16/V.90, the CRC after bit 68 (type 0) or 170 */
int V90Mp::parse(const unsigned char *b, int len)
{
	int i, t = len == 188;

	if (len != 86 && len != 188)
		return -1;
	if (b[18] != t || !frame_crc_ok(b, t ? 171 : 69))
		return -1;
	type = t;
	rate_max = field(b, 24, 4);
	trellis = field(b, 29, 2);
	nonlinear = b[31];
	expanded = b[32];
	ack = b[33];
	rate_mask = field(b, 36, 13);
	for (i = 0; i < 6; i++)
		h[i] = t ? static_cast<s16>(field(b, 52 + 17 * i, 16)) : 0;
	return 0;
}

/* see v90a.h: Sr 0 only (no spectral shaping) */
int V90Con::init(const V90Cp &cp, int D_, const float lv[6][128])
{
	int i, u;

	reinit(*this);
	if (cp.sr || !cp.usable(D_, 6))
		return -1;
	D = D_;
	S = 6;
	K = D - S;
	for (i = 0; i < 6; i++) {
		const unsigned char *m = cp.mask[cp.con[i]];
		for (u = 127; u >= 0; u--)
			if (m[u]) {
				ucode[i][M[i]] = static_cast<unsigned char>(u);
				lev[i][M[i]++] = lv[i][u];
			}
	}
	return 0;
}

/* see v90a.h: label j of interval i comes up in n of the 2^K values of
   the modulus encoder's R, with A the product of the M before i */
double V90Con::power(const int *lin) const
{
	u64 two_k = UINT64_C(1) << K, A = 1;
	double sum = 0;
	int i, j;

	for (i = 0; i < 6; i++) {
		u64 am = A * static_cast<unsigned>(M[i]);
		u64 full = two_k / am, rem = two_k % am;
		for (j = 0; j < M[i]; j++) {
			u64 lo = static_cast<u64>(j) * A, n;
			n = full * A + (rem > lo ? (rem - lo < A ? rem - lo : A) :
					0);
			sum += static_cast<double>(lin[ucode[i][j]]) *
			       lin[ucode[i][j]] * static_cast<double>(n);
		}
		A = am;
	}
	return sum / (6.0 * static_cast<double>(two_k));
}

/* a CP's constellations from each interval's Ucodes (pts[i][0..n[i]-1]):
   one per distinct set */
static void cp_masks(V90Cp *cp, const int pts[6][128], const int *n)
{
	int i, j, m;

	cp->ncon = 0;
	zero(cp->mask);
	for (i = 0; i < 6; i++) {
		unsigned char mk[128];
		zero(mk);
		for (m = 0; m < n[i]; m++)
			mk[pts[i][m]] = 1;
		for (j = 0; j < cp->ncon; j++)
			if (!memcmp(cp->mask[j], mk, sizeof(mk)))
				break;
		if (j == cp->ncon)
			memcpy(cp->mask[cp->ncon++], mk, sizeof(mk));
		cp->con[i] = j;
	}
}

/* the parts of a CP our designs share: Sr 0 (no shaping), ld 0, every
   upstream rate */
static void cp_common(V90Cp *cp, int alaw)
{
	zero(*cp);
	cp->alaw = alaw;
	cp->rate_mask = 0x1FFF;
}

/* the power of a CP's constellations (Table 15/V.90) at D bits */
static double cp_power(const V90Cp &cp, int D, const int *lin,
		       const float lv[6][128])
{
	V90Con c;
	if (c.init(cp, D, lv))
		return -1;
	return c.power(lin);
}

/* its level in dBm0: 0 dBm0 is 3.17 dB (A-law 3.14) below the sine
   at G.711's overload */
static double dbm0(int alaw, double pw)
{
	double rms0 = (alaw ? 32768 : 32636) / M_SQRT2 /
		pow(10, (alaw ? 3.14 : 3.17) / 20);
	return 10 * log10(pw / (rms0 * rms0) + 1e-30);
}

/* see v90a.h: in each interval the Ucodes nearest L, 3 L, 5 L and 7 L
   (8 levels with the signs, evenly apart; 21 L^2 the power), L lowered
   until the power is within the limit */
int v90a_design_cpt(V90Cp *cp, int alaw, const int *lin,
		    const float lv[6][128], double limit)
{
	double L = 0.89 * limit / sqrt(21.0), pw = -1;
	int pts[6][128], n[6], i, t, u, tries;

	cp_common(cp, alaw);
	cp->drn = 10;           /* D 18: K 12, S 6, 24 000 bit/s */
	for (tries = 0; tries < 40; tries++, L *= 0.97) {
		for (i = 0; i < 6; i++) {
			n[i] = 0;
			for (t = 1; t <= 7; t += 2) {
				int best = -1;
				for (u = 1; u < 128; u++)
					if (lv[i][u] > 0 &&
					    (n[i] == 0 || u > pts[i][n[i] - 1]) &&
					    (best < 0 || fabs(lin[u] - t * L) <
					     fabs(lin[best] - t * L)))
						best = u;
				if (best < 0)
					return -1;
				pts[i][n[i]++] = best;
			}
		}
		cp_masks(cp, pts, n);
		pw = cp_power(*cp, 18, lin, lv);
		if (pw >= 0 && pw <= limit * limit)
			break;
	}
	if (pw < 0 || pw > limit * limit)
		return -1;
	DSP_DBG("v90a: CPt: Ucodes %d %d %d %d (interval 0), %d "
		"constellations, %.1f dBm0\n", pts[0][0], pts[0][1],
		pts[0][2], pts[0][3], cp->ncon, dbm0(alaw, pw));
	return 0;
}

/* see v90a.h: in each interval, from the least level of half the
   spacing up, each next point the spacing above the last; then the
   largest point of the interval whose largest is loudest dropped until
   the power is within the limits; K the most the points carry (up to
   36), as Jd allows (bit 0: K 15, 28 000 bit/s) */
int v90a_design_cp(V90Cp *cp, int alaw, const int *lin,
		   const float lv[6][128], double noise, double limit,
		   double cpt_power, u32 jd_rates)
{
	double dmin = 10 * noise, most = limit * limit, pw = -1;
	int pts[6][128], n[6], i, u, K = 0;

	if (cpt_power * 1.99 < most)
		most = cpt_power * 1.99;
	cp_common(cp, alaw);
	cp->cp = 1;
	for (i = 0; i < 6; i++) {
		float last = 0;
		n[i] = 0;
		/* (the levels rise with the Ucode) */
		for (u = 1; u < 128; u++) {
			if (lv[i][u] <= 0)
				continue;
			if (n[i] ? lv[i][u] - last < dmin : lv[i][u] < dmin / 2)
				continue;
			pts[i][n[i]++] = u;
			last = lv[i][u];
		}
		if (n[i] < 2)
			return -1;
	}
	for (;;) {
		double bits = 0;
		int j;
		for (i = 0; i < 6; i++)
			bits += log2(static_cast<double>(n[i]));
		K = static_cast<int>(floor(bits + 1e-9));
		if (K > 36)
			K = 36;
		while (K >= 15 && !(jd_rates >> (K - 15) & 1))
			K--;
		if (K < 15) {
			DSP_DBG("v90a: CP: M %d %d %d %d %d %d at spacing "
				"%.0f carry %.1f bits, 15 at least\n", n[0],
				n[1], n[2], n[3], n[4], n[5], dmin, bits);
			return -1;
		}
		cp->drn = K - 14;
		cp_masks(cp, pts, n);
		pw = cp_power(*cp, K + 6, lin, lv);
		if (pw >= 0 && pw <= most)
			break;
		/* the loudest largest point goes */
		for (i = 1, j = 0; i < 6; i++)
			if (lin[pts[i][n[i] - 1]] > lin[pts[j][n[j] - 1]])
				j = i;
		if (--n[j] < 2)
			return -1;
	}
	DSP_DBG("v90a: CP: D %d (%" PRId32 " bit/s), M %d %d %d %d %d %d, %d "
		"constellations, spacing %.0f, %.1f dBm0 (CPt %.1f)\n", K + 6,
		s32{K + 6} * 8000 / 6, n[0], n[1], n[2], n[3], n[4], n[5],
		cp->ncon, dmin, dbm0(alaw, pw), dbm0(alaw, cpt_power));
	return 0;
}

/* see v90a.h: after TRN2d, by each interval's gain it found */
void V90Rx::levels(float lv[6][128]) const
{
	int j, u;
	for (j = 0; j < 6; j++)
		for (u = 0; u < 128; u++) {
			if (dil_on)
				lv[j][u] = dil_usable[j][u] &&
					dil_level[j][u] > 0 ?
					dil_level[j][u] : 0;
			else
				lv[j][u] = static_cast<float>(lin[u]);
			if (ev_trn2)
				lv[j][u] *= t_gain[j];
		}
}

/* see v90a.h: UINFO's spread in each frame interval after TRN1d's
   least length (random signs: the data mode's intersymbol
   interference), or the DIL's noise if more */
float V90Rx::noise() const
{
	double v = 0;
	float nz;
	int j;
	for (j = 0; j < 6; j++) {
		double mu;
		if (un[j] < NOISE_N / 6)
			break;
		mu = usum[j] / un[j];
		v += usq[j] / un[j] - mu * mu;
	}
	nz = j == 6 ? static_cast<float>(sqrt(v > 0 ? v / 6 : 0)) :
		      sqrtf(mse) * level;
	return dil_on && dil_noise > nz ? dil_noise : nz;
}

/* see v90a.h: Ri next */
int V90Rx::phase4(const V90Cp &cpt)
{
	int u;
	cpt_s = cpt;
	levels(lv4);
	if (con_trn.init(cpt, cpt.drn + 8, lv4))
		return -1;
	for (u = 0; u < 128; u++)
		ref_lin[u] = u + 1;
	ref.init(&cpt, cpt.drn + 8, ref_lin);
	con = &con_trn;
	stage = RI;
	ri_run = 0;
	return 0;
}

/* see v90a.h */
int V90Rx::take(unsigned char *b, int n)
{
	int m = 0;
	while (m < n && qn) {
		b[m++] = q[qh];
		qh = (qh + 1) % V90RX_Q;
		qn--;
	}
	return m;
}

/* see v90a.h */
int V90Rx::data_cp(const V90Cp &cp)
{
	return con_data.init(cp, cp.drn + 20, lv4);
}

/* see v90a.h */
int V90Rx::data_cp_next(const V90Cp &cp)
{
	have_next = !con_next.init(cp, cp.drn + 20, lv4);
	return have_next ? 0 : -1;
}

/* the equaliser a step towards e (NLMS) for symbol kk */
void V90Rx::adapt(s32 kk, float e)
{
	float pw = 0;
	int i;
	for (i = 0; i < V90RX_TAPS; i++) {
		float xi = xin(kk, i);
		pw += xi * xi;
	}
	if (pw > 0)
		for (i = 0; i < V90RX_TAPS; i++)
			c[i] += EQ_MU * e / pw * xin(kk, i);
}

/* a frame's signs in phase 4 before TRN2d: Ri, 8 frames of + + + - - -
   in a row, then the first frame of mostly its opposite, Ri-bar;
   TRN2d 24 symbols after it */
void V90Rx::ri_frame(const int *s)
{
	static const int p[6] = { 1, 1, 1, -1, -1, -1 };
	int j, score = 0;

	if (ev_ribar)
		return;
	for (j = 0; j < 6; j++)
		score += s[j] ? p[j] : -p[j];
	if (!ev_ri) {
		ri_run = score == 6 ? ri_run + 1 : 0;
		if (ri_run >= 8) {
			ev_ri = 1;
			ri_seen = k;
			DSP_DBG("v90a: Ri at symbol %" PRId32 "\n", k - 47);
		}
		return;
	}
	if (score <= -4) {
		ev_ribar = 1;
		ribar_at = k - 5;
		frames_at = ribar_at + 24;
		DSP_DBG("v90a: Ri-bar at symbol %" PRId32 ", TRN2d from %"
			PRId32 "\n", ribar_at, frames_at);
	} else if (score < 4) {
		ri_run = 0;
		ev_ri = 0;
		DSP_DBG("v90a: Ri lost at symbol %" PRId32 "\n", k);
	}
}

/* a data frame's labels and signs in data mode: Rd, 8 frames of + + +
   - - - (or - - - + + +) of each interval's loudest point (or the one
   below it) in a row, then the first frame mostly the opposite, Rd-bar
   (9.6/V.90); TRN2d (or MP) 24 symbols after it */
void V90Rx::rd_frame(const int *s, const int *lab)
{
	static const int p[6] = { 1, 1, 1, -1, -1, -1 };
	int j, score = 0, top = 1;

	if (ev_rdbar)
		return;
	for (j = 0; j < 6; j++) {
		score += s[j] ? p[j] : -p[j];
		/* (label 0 the loudest) */
		if (lab[j] > 1)
			top = 0;
	}
	if (!ev_rd) {
		if (top && (score == 6 || score == -6) &&
		    (!rd_run || score == 6 * rd_pol)) {
			rd_pol = score > 0 ? 1 : -1;
			if (++rd_run >= 8) {
				ev_rd = 1;
				DSP_DBG("v90a: Rd at symbol %" PRId32 "\n",
					k - 47);
			}
		} else
			rd_run = 0;
		return;
	}
	if (score * rd_pol <= -4) {
		ev_rdbar = 1;
		ribar_at = k - 5;
		frames_at = ribar_at + 24;
		stage = RD;
		DSP_DBG("v90a: Rd-bar at symbol %" PRId32 ", TRN2d from %"
			PRId32 "\n", ribar_at, frames_at);
	} else if (score * rd_pol < 4) {
		/* (the frames before it were not data: Rd lost) */
		rd_run = 0;
		ev_rd = 0;
		DSP_DBG("v90a: Rd lost at symbol %" PRId32 "\n", k);
	}
}

/* a data frame's signs and labels against Rf (8.8.4/V.92): + + - - + +
   and - - + + - - in turn on the loudest points, 8 frames of it; then
   Rf-bar, the pattern a frame on (two frames alike); after its 4
   frames the SUVd of a fast parameter exchange */
void V90Rx::rf_frame(const int *s, const int *lab)
{
	static const int p[6] = { 1, 1, -1, -1, 1, 1 };
	int j, score = 0, top = 1, pat;

	if (ev_rfbar) {
		if (++rfbar_n == 4) {
			/* SUVd from cleared descrambler and signs */
			fpe_on = 1;
			fscr = 0;
			flast = 0;
			mp_n = 0;
			msr = 0;
			mp_frame = 0;
			ed_zero = 0;
			ev_suvd = ev_cpd = ev_mpack = ev_ed = ev_b1d = 0;
			ev_rf = ev_rfbar = rf_run = 0;
			DSP_DBG("v90a: FPE: SUVd from frame %" PRId32 "\n",
				frames + 1);
		}
		return;
	}
	for (j = 0; j < 6; j++) {
		score += s[j] ? p[j] : -p[j];
		if (lab[j] > 1)
			top = 0;
	}
	pat = score == 6 ? 1 : score == -6 ? -1 : 0;
	if (!ev_rf) {
		if (top && pat && (!rf_run || pat == -rf_last)) {
			rf_last = pat;
			if (++rf_run >= 8) {
				ev_rf = 1;
				DSP_DBG("v90a: Rf at frame %" PRId32 "\n",
					frames);
			}
		} else
			rf_run = 0;
		return;
	}
	if (top && pat && pat == rf_last) {
		ev_rfbar = 1;
		rfbar_n = 1;
		DSP_DBG("v90a: Rf-bar at frame %" PRId32 "\n", frames);
	} else if (top && pat == -rf_last)
		rf_last = pat;
	else {
		ev_rf = rf_run = 0;
		DSP_DBG("v90a: Rf lost at frame %" PRId32 "\n", frames);
	}
}

/* after Rd-bar: phase 4's frames again (9.6.2.2/V.90), TRN2d, MP and
   Ed on CPt's constellations from cleared descrambler and signs, then
   B1d on the new CP's; the bits queued for the caller stay */
void V90Rx::reneg_frames()
{
	renegs++;
	con = &con_trn;
	frames = 0;
	frames_b1 = 0;
	fscr = 0;
	flast = 0;
	trn_sym = trn_sym_err = 0;
	mp_n = 0;
	msr = 0;
	mp_frame = 0;
	ed_zero = 0;
	b1_err = 0;
	fe_sum = 0;
	fe_n = 0;
	ev_trn2 = ev_mp = ev_mpack = ev_ed = ev_b1d = 0;
	ev_suvd = ev_cpd = 0;
	have_next = 0;
	stage = FRAMES;
}

/* an MP bit (9.4.1.3/V.90): frame sync, then 86 bits (type 0) or 188 */
void V90Rx::mp_bit(int bit)
{
	msr = (msr << 1) | static_cast<u32>(bit);
	if (mp_n && v92) {
		/* SUVd (bit 18 set: Table 31/V.92) or CPd (Table 30) */
		mpb[mp_n++] = static_cast<unsigned char>(bit);
		if (mp_n == 19)
			mp_len = bit ? 51 : 0;
		if (!mp_len && mp_n > 19) {
			mp_len = v92_cpd_length(mpb, mp_n);
			if (mp_len > V90RX_SEQ || (mp_len && mp_len < mp_n)) {
				mp_n = 0;
				return;
			}
		}
		if (mp_len && mp_n == mp_len) {
			int ack = -1;
			mp_n = 0;
			if (mp_len == 51) {
				if (frame_crc_ok(mpb, 35)) {
					ack = mpb[33];
					if (!ev_suvd || (ack && !ev_mpack))
						DSP_DBG("v90a: SUVd%s at frame "
							"%" PRId32 "\n",
							ack ? "'" : "", frames);
					ev_suvd++;
				}
			} else {
				V92Cpd c;
				if (c.parse(mpb, mp_len) == 0) {
					ack = c.ack;
					if (!ev_cpd || (ack && !ev_mpack))
						DSP_DBG("v90a: CPd%s at frame "
							"%" PRId32 ": %" PRId32
							" bit/s, M %d, "
							"%d points, G %.5f, "
							"trellis %d, taps %d %d "
							"%d %d\n", ack ? "'" : "",
							frames,
							s32{c.drn + 17} *
							8000 / 6, c.M[0],
							2 * c.lc[0], c.g,
							16 << c.trellis,
							c.len[0], c.len[1],
							c.len[2], c.len[3]);
					cpd = c;
					ev_cpd++;
				}
			}
			if (ack < 0) {
				DSP_DBG("v90a: %s with a bad CRC at frame "
					"%" PRId32 "\n",
					mp_len == 51 ? "SUVd" : "CPd", frames);
				return;
			}
			if (ack)
				ev_mpack++;
			mp_frame = frames;
			ed_zero = 0;
		}
		return;
	}
	if (mp_n) {
		mpb[mp_n++] = static_cast<unsigned char>(bit);
		if (mp_n == 19)
			mp_len = bit ? 188 : 86;
		if (mp_n == mp_len) {
			V90Mp m;
			mp_n = 0;
			if (m.parse(mpb, mp_len) == 0) {
				if (!ev_mp || (m.ack && !ev_mpack))
					DSP_DBG("v90a: MP%s at frame %" PRId32
						": type %d, upstream at most %"
						PRId32 ", mask "
						"%04x, trellis %d\n",
						m.ack ? "'" : "", frames,
						m.type,
						s32{m.rate_max} * 2400,
						m.rate_mask, m.trellis);
				mp = m;
				ev_mp++;
				if (m.ack)
					ev_mpack++;
				mp_frame = frames;
				ed_zero = 0;
			} else
				DSP_DBG("v90a: MP with bad CRC at frame %"
					PRId32 "\n", frames);
		}
		return;
	}
	if ((msr & 0x3FFFF) == 0x3FFFE) {
		std::fill_n(mpb, 17, 1);
		mpb[17] = 0;
		mp_n = 18;
		mp_len = 0;
	}
}

/* a data frame's values fy[] decoded (5.4/V.90, Sr 0): each the
   nearest level, the labels the modulus encoder's R, the signs
   differentially, all D descrambled.  TRN2d's first 340 frames are
   known (the equaliser follows them), after them the decisions; MP
   frames, Ed after an MP', B1d on CP's constellations */
void V90Rx::frame()
{
	V90Con *cn = con;
	unsigned char d[48];
	u64 R = 0, A = 1;
	int j, i, s[6], lab[6], ones = 0;
	int known = cn == &con_trn && frames < 340 && !renegs;
	int out[6];

	if (known) {
		unsigned char one[48];
		std::fill(std::begin(one), std::end(one), 1);
		ref.push(one, 0);
		ref.pop(out);
	}
	for (j = 0; j < 6; j++) {
		float m = fabsf(fy[j]), best = 1e30f, tg;
		int l, bl = 0;
		for (l = 0; l < cn->M[j]; l++) {
			float dd = fabsf(cn->lev[j][l] - m);
			if (dd < best) {
				best = dd;
				bl = l;
			}
		}
		s[j] = fy[j] > 0;
		lab[j] = bl;
		R += static_cast<u64>(bl) * A;
		A *= static_cast<unsigned>(cn->M[j]);
		tg = s[j] ? cn->lev[j][bl] : -cn->lev[j][bl];
		if (known) {
			int u = (out[j] > 0 ? out[j] : -out[j]) - 1;
			if (u != cn->ucode[j][bl] || (out[j] > 0) != s[j])
				trn_sym_err++;
			trn_sym++;
			tg = out[j] > 0 ? lv4[j][u] : -lv4[j][u];
		}
		/* TRN2d's second half: each interval's gain against the
		   DIL's levels */
		if (known && frames >= 170) {
			t2_yr[j] += static_cast<double>(fy[j]) * tg;
			t2_rr[j] += static_cast<double>(tg) * tg;
			t2_yy[j] += static_cast<double>(fy[j]) * fy[j];
			t2_n++;
		}
		adapt(k - 5 + j, tg - fy[j]);
		timing(k - 5 + j, fy[j], tg);
		if (cn == &con_data) {
			/* each point's level follows its decisions (the
			   DIL's, by the gain TRN2d showed, are a little off
			   for some codes) */
			cn->lev[j][bl] += LEV_MU * (m - cn->lev[j][bl]);
			dpow += 0.0002f * (fy[j] * fy[j] - dpow);
			de_mse += 0.0002f * ((tg - fy[j]) * (tg - fy[j]) -
					     de_mse);
		}
		fe_sum += static_cast<double>(tg - fy[j]) * (tg - fy[j]);
		fe_n++;
	}
	for (j = 0; j < 6; j++) {
		d[j] = static_cast<unsigned char>(s[j] ^ flast);
		flast = s[j];
	}
	for (i = 0; i < cn->K; i++)
		d[6 + i] = static_cast<unsigned char>(R >> i & 1);
	for (i = 0; i < cn->D; i++) {
		d[i] = static_cast<unsigned char>(
			descramble(&fscr, SCR_GPC, d[i]));
		ones += d[i];
	}

	if (cn == &con_data) {
		/* data mode: the bits for take(), the signal's power and the
		   decisions' error */
		if (frames_b1 >= 48 && fpe_on) {
			/* a fast parameter exchange's SUVd, CPd, Ed */
			for (i = 0; i < cn->D; i++)
				mp_bit(d[i]);
			if (ev_mpack && !mp_n &&
			    frames == mp_frame + 1 + ed_zero) {
				if (ones <= 2) {
					if (++ed_zero == 2) {
						ev_ed = 1;
						fpe_on = 0;
						fscr = 0;
						flast = 0;
						frames_b1 = 0;
						b1_err = 0;
						if (have_next) {
							/* B1d on our CPu's */
							con_data = con_next;
							have_next = 0;
						}
						DSP_DBG("v90a: FPE: Ed at "
							"frame %" PRId32
							", B1d next (D %d)\n",
							frames,
							con_data.D);
					}
				} else
					ed_zero = 0;
			}
			frames++;
			return;
		}
		if (frames_b1 >= 48) {
			/* Rd: no bits from it on (circuit 104 clamped) */
			rd_frame(s, lab);
			if (ev_rd) {
				frames++;
				return;
			}
			/* Rf (V.92) likewise */
			if (v92) {
				rf_frame(s, lab);
				if (ev_rf) {
					frames++;
					return;
				}
			}
			for (i = 0; i < cn->D; i++) {
				if (qn == V90RX_Q) {
					/* (the caller is not taking them) */
					qh = (qh + 1) % V90RX_Q;
					qn--;
				}
				q[(qh + qn++) % V90RX_Q] = d[i];
			}
			frames++;
			return;
		}
		/* B1d: 48 frames of ones */
		if (frames_b1 < 48) {
			b1_err += cn->D - ones;
			if (++frames_b1 == 48) {
				ev_b1d = 1;
				DSP_DBG("v90a: B1d: %" PRId32 " bit errors in "
					"48 frames of %d bits, mse %.6f, "
					"timing %+.1f ppm\n", b1_err, cn->D,
					fe_n ? fe_sum / fe_n / ((double)level *
						level) : 0, ppm());
			}
		}
		frames++;
		return;
	}
	if (known && frames == 339) {
		/* the DIL's levels come out a little high or low in some
		   intervals (its sign pattern repeats with the segment, so
		   what the equaliser leaves of the neighbours adds the same
		   each time); TRN2d's random symbols show by how much: its
		   levels, the CP's and the decoder's, by each interval's
		   gain, the noise what is left about them */
		double rest = 0;
		int l, u;
		for (j = 0; j < 6; j++) {
			double g = t2_rr[j] > 0 ? t2_yr[j] / t2_rr[j] : 1;
			rest += t2_yy[j] - t2_yr[j] * g;
			t_gain[j] = static_cast<float>(g);
			for (u = 0; u < 128; u++)
				lv4[j][u] *= static_cast<float>(g);
			for (l = 0; l < con_trn.M[j]; l++)
				con_trn.lev[j][l] *= static_cast<float>(g);
		}
		trn2_noise = static_cast<float>(
			sqrt(rest > 0 && t2_n ? rest / t2_n : 0));
		ev_trn2 = 1;
		DSP_DBG("v90a: TRN2d: %" PRId32 " of %" PRId32 " symbols wrong, noise %.2f, "
			"interval gains %.4f %.4f %.4f %.4f %.4f %.4f, "
			"timing %+.1f ppm\n", trn_sym_err, trn_sym,
			trn2_noise, t_gain[0], t_gain[1], t_gain[2], t_gain[3],
			t_gain[4], t_gain[5], ppm());
	}
	/* a renegotiation's TRN2d (if any: the decisions, on CPt's
	   points): the noise from the error over the same frames */
	if (renegs && cn == &con_trn && frames == 339) {
		trn2_noise = static_cast<float>(sqrt(fe_n ? fe_sum / fe_n : 0));
		ev_trn2 = 1;
		DSP_DBG("v90a: renegotiation's TRN2d (or MP): noise %.2f, "
			"timing %+.1f ppm\n", trn2_noise, ppm());
	}
	/* (TRN2d's noise from its second half: the equaliser and the
	   timing settle in the first after the DIL) */
	if (frames == 169 || frames == 339) {
		fe_sum = 0;
		fe_n = 0;
	}
	for (i = 0; i < cn->D; i++)
		mp_bit(d[i]);
	/* Ed: the 2 frames after an MP' sequence's last are zeros */
	if (ev_mpack && !mp_n && frames == mp_frame + 1 + ed_zero) {
		if (ones <= 2) {
			if (++ed_zero == 2) {
				ev_ed = 1;
				con = &con_data;
				fscr = 0;
				flast = 0;
				fe_sum = 0;
				fe_n = 0;
				DSP_DBG("v90a: Ed at frame %" PRId32 ", B1d "
					"next (D %d)\n", frames, con_data.D);
			}
		} else
			ed_zero = 0;
	}
	frames++;
}

} // namespace dsp
