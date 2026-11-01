/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    g729a_enc.cpp - G.729 Annex A's encoder (g729a.h): the LP
 *                    analysis and the LSP quantiser, the open and closed
 *                    loop pitch searches, Annex A's algebraic codebook
 *                    search, the gain quantiser and taming
 *
 */

#include "g729a.h"
#include "g729a_int.h"

namespace dlink {
namespace g729 {

/* the encoder's memory from frame to frame */
struct EncState {
	i16 old_speech[L_TOTAL];
	i16 old_wsp[L_FRAME + PIT_MAX];
	i16 old_exc[L_FRAME + PIT_MAX + L_INTERPOL];
	i16 lsp_old[M], lsp_old_q[M];
	i16 mem_w0[M], mem_w[M], mem_zero[M];
	i16 sharp;
	i16 old_A[MP1], old_rc[2];      /* Levinson's last good filter */
	LspPast lsp;
	i16 past_qua_en[4];
	i32 exc_err[4];                 /* taming (3.10/G.729) */
	HighPass hp;
};

/* the windowed autocorrelations r[0..M] in halves, r[0] normalised */
static void autocorr(const i16 *x, i16 *r_h, i16 *r_l)
{
	i16 y[L_WINDOW];
	for (int i = 0; i < L_WINDOW; i++)
		y[i] = mult_r(x[i], hamwindow[i]);
	i32 sum;
	do {
		overflow() = false;
		sum = 1;
		for (int i = 0; i < L_WINDOW; i++)
			sum = L_mac(sum, y[i], y[i]);
		if (overflow())
			for (int i = 0; i < L_WINDOW; i++)
				y[i] = shr(y[i], 2);
	} while (overflow());
	const i16 norm = norm_l(sum);
	sum = L_shl(sum, norm);
	L_Extract(sum, &r_h[0], &r_l[0]);
	for (int i = 1; i <= M; i++) {
		sum = 0;
		for (int j = 0; j < L_WINDOW - i; j++)
			sum = L_mac(sum, y[j], y[j + i]);
		sum = L_shl(sum, norm);
		L_Extract(sum, &r_h[i], &r_l[i]);
	}
}

/* the lag window on r[1..M] */
static void lag_window(i16 *r_h, i16 *r_l)
{
	for (int i = 1; i <= M; i++) {
		const i32 x = Mpy_32(r_h[i], r_l[i], lag_h[i - 1], lag_l[i - 1]);
		L_Extract(x, &r_h[i], &r_l[i]);
	}
}

/* Levinson-Durbin: r to the LP filter a (Q12) and the reflection
   coefficients; an unstable filter gives the last one again */
static void levinson(EncState &s, const i16 *Rh, const i16 *Rl,
		     i16 *A, i16 *rc)
{
	i16 hi, lo, Kh, Kl, alp_h, alp_l, alp_exp;
	i16 Ah[MP1], Al[MP1], Anh[MP1], Anl[MP1];

	i32 t1 = L_Comp(Rh[1], Rl[1]);
	i32 t2 = L_abs(t1);
	i32 t0 = Div_32(t2, Rh[0], Rl[0]);
	if (t1 > 0)
		t0 = L_negate(t0);
	L_Extract(t0, &Kh, &Kl);
	rc[0] = Kh;
	t0 = L_shr(t0, 4);
	L_Extract(t0, &Ah[1], &Al[1]);
	t0 = Mpy_32(Kh, Kl, Kh, Kl);
	t0 = L_abs(t0);
	t0 = L_sub(MAX32, t0);
	L_Extract(t0, &hi, &lo);
	t0 = Mpy_32(Rh[0], Rl[0], hi, lo);
	alp_exp = norm_l(t0);
	t0 = L_shl(t0, alp_exp);
	L_Extract(t0, &alp_h, &alp_l);

	for (int i = 2; i <= M; i++) {
		t0 = 0;
		for (int j = 1; j < i; j++)
			t0 = L_add(t0, Mpy_32(Rh[j], Rl[j], Ah[i - j], Al[i - j]));
		t0 = L_shl(t0, 4);
		t1 = L_Comp(Rh[i], Rl[i]);
		t0 = L_add(t0, t1);
		t1 = L_abs(t0);
		t2 = Div_32(t1, alp_h, alp_l);
		if (t0 > 0)
			t2 = L_negate(t2);
		t2 = L_shl(t2, alp_exp);
		L_Extract(t2, &Kh, &Kl);
		rc[i - 1] = Kh;
		if (sub(abs_s(Kh), 32750) > 0) {
			for (int j = 0; j <= M; j++)
				A[j] = s.old_A[j];
			rc[0] = s.old_rc[0];
			rc[1] = s.old_rc[1];
			return;
		}
		for (int j = 1; j < i; j++) {
			t0 = Mpy_32(Kh, Kl, Ah[i - j], Al[i - j]);
			t0 = L_add(t0, L_Comp(Ah[j], Al[j]));
			L_Extract(t0, &Anh[j], &Anl[j]);
		}
		t2 = L_shr(t2, 4);
		L_Extract(t2, &Anh[i], &Anl[i]);
		t0 = Mpy_32(Kh, Kl, Kh, Kl);
		t0 = L_abs(t0);
		t0 = L_sub(MAX32, t0);
		L_Extract(t0, &hi, &lo);
		t0 = Mpy_32(alp_h, alp_l, hi, lo);
		const i16 n = norm_l(t0);
		t0 = L_shl(t0, n);
		L_Extract(t0, &alp_h, &alp_l);
		alp_exp = add(alp_exp, n);
		for (int j = 1; j <= i; j++) {
			Ah[j] = Anh[j];
			Al[j] = Anl[j];
		}
	}
	A[0] = 4096;
	for (int i = 1; i <= M; i++) {
		t0 = L_Comp(Ah[i], Al[i]);
		s.old_A[i] = A[i] = round16(L_shl(t0, 1));
	}
	s.old_rc[0] = rc[0];
	s.old_rc[1] = rc[1];
}

/* the Chebyshev polynomial f at x; wide: f in Q11 (else Q10, after an
   overflow) */
static i16 chebps(i16 x, const i16 *f, bool wide)
{
	i16 b0_h, b0_l, b1_h, b1_l;
	i16 b2_h = wide ? 256 : 128, b2_l = 0;
	i32 t = L_mult(x, wide ? 512 : 256);
	t = L_mac(t, f[1], 4096);
	L_Extract(t, &b1_h, &b1_l);
	int i = 2;
	for (; i < NC; i++) {
		t = Mpy_32_16(b1_h, b1_l, x);
		t = L_shl(t, 1);
		t = L_mac(t, b2_h, MIN16);
		t = L_msu(t, b2_l, 1);
		t = L_mac(t, f[i], 4096);
		L_Extract(t, &b0_h, &b0_l);
		b2_l = b1_l;
		b2_h = b1_h;
		b1_l = b0_l;
		b1_h = b0_h;
	}
	t = Mpy_32_16(b1_h, b1_l, x);
	t = L_mac(t, b2_h, MIN16);
	t = L_msu(t, b2_l, 1);
	t = L_mac(t, f[i], 2048);
	t = L_shl(t, wide ? 6 : 7);
	return extract_h(t);
}

/* the LP filter a to its LSPs: the roots of F1 and F2 on the grid; the
   old LSPs if 10 are not found */
static void az_lsp(const i16 *a, i16 *lsp, const i16 *old_lsp)
{
	i16 f1[NC + 1], f2[NC + 1];
	bool ovf = false;
	f1[0] = 2048;
	f2[0] = 2048;
	for (int i = 0; i < NC; i++) {
		overflow() = false;
		i32 t = L_mult(a[i + 1], 16384);
		t = L_mac(t, a[M - i], 16384);
		i16 x = extract_h(t);
		ovf = ovf || overflow();
		overflow() = false;
		f1[i + 1] = sub(x, f1[i]);
		ovf = ovf || overflow();
		overflow() = false;
		t = L_mult(a[i + 1], 16384);
		t = L_msu(t, a[M - i], 16384);
		x = extract_h(t);
		ovf = ovf || overflow();
		overflow() = false;
		f2[i + 1] = add(x, f2[i]);
		ovf = ovf || overflow();
	}
	const bool wide = !ovf;
	if (ovf) {
		f1[0] = 1024;
		f2[0] = 1024;
		for (int i = 0; i < NC; i++) {
			i32 t = L_mult(a[i + 1], 8192);
			t = L_mac(t, a[M - i], 8192);
			f1[i + 1] = sub(extract_h(t), f1[i]);
			t = L_mult(a[i + 1], 8192);
			t = L_msu(t, a[M - i], 8192);
			f2[i + 1] = add(extract_h(t), f2[i]);
		}
	}

	i16 nf = 0;
	bool second = false;
	const i16 *fpol = f1;
	i16 xlow = grid[0];
	i16 ylow = chebps(xlow, fpol, wide);
	i16 j = 0;
	while (nf < M && j < GRID_POINTS) {
		j = add(j, 1);
		i16 xhigh = xlow, yhigh = ylow;
		xlow = grid[j];
		ylow = chebps(xlow, fpol, wide);
		if (L_mult(ylow, yhigh) > 0)
			continue;
		/* a sign change: two halvings, then the linear one */
		for (int i = 0; i < 2; i++) {
			const i16 xmid = add(shr(xlow, 1), shr(xhigh, 1));
			const i16 ymid = chebps(xmid, fpol, wide);
			if (L_mult(ylow, ymid) <= 0) {
				yhigh = ymid;
				xhigh = xmid;
			} else {
				ylow = ymid;
				xlow = xmid;
			}
		}
		const i16 x = sub(xhigh, xlow);
		i16 y = sub(yhigh, ylow);
		i16 xint;
		if (y == 0) {
			xint = xlow;
		} else {
			const i16 sign = y;
			y = abs_s(y);
			const i16 e = norm_s(y);
			y = shl(y, e);
			y = div_s(16383, y);
			i32 t = L_mult(x, y);
			t = L_shr(t, sub(20, e));
			y = extract_l(t);
			if (sign < 0)
				y = negate(y);
			t = L_mult(ylow, y);
			t = L_shr(t, 11);
			xint = sub(xlow, extract_l(t));
		}
		lsp[nf] = xint;
		xlow = xint;
		nf = add(nf, 1);
		second = !second;
		fpol = second ? f2 : f1;
		ylow = chebps(xlow, fpol, wide);
	}
	if (sub(nf, M) < 0)
		for (int i = 0; i < M; i++)
			lsp[i] = old_lsp[i];
}

/* the LSFs' weights for the quantiser's distance (3.2.4/G.729) */
static void get_wegt(const i16 *flsp, i16 *wegt)
{
	i16 buf[M];
	buf[0] = sub(flsp[1], PI04 + 8192);
	for (int i = 1; i < M - 1; i++)
		buf[i] = sub(sub(flsp[i + 1], flsp[i - 1]), 8192);
	buf[M - 1] = sub(PI92 - 8192, flsp[M - 2]);
	for (int i = 0; i < M; i++) {
		if (buf[i] > 0) {
			wegt[i] = 2048;
		} else {
			i32 t = L_mult(buf[i], buf[i]);
			i16 tmp = extract_h(L_shl(t, 2));
			t = L_mult(tmp, CONST10);
			tmp = extract_h(L_shl(t, 2));
			wegt[i] = add(tmp, 2048);
		}
	}
	wegt[4] = extract_h(L_shl(L_mult(wegt[4], CONST12), 1));
	wegt[5] = extract_h(L_shl(L_mult(wegt[5], CONST12), 1));
	i16 tmp = 0;
	for (int i = 0; i < M; i++)
		if (sub(wegt[i], tmp) > 0)
			tmp = wegt[i];
	const i16 sft = norm_s(tmp);
	for (int i = 0; i < M; i++)
		wegt[i] = shl(wegt[i], sft);
}

/* the first stage's codevector nearest rbuf */
static i16 lsp_pre_select(const i16 *rbuf)
{
	i16 cand = 0;
	i32 dmin = MAX32;
	for (int i = 0; i < NC0; i++) {
		i32 d = 0;
		for (int j = 0; j < M; j++) {
			const i16 tmp = sub(rbuf[j], lspcb1[i][j]);
			d = L_mac(d, tmp, tmp);
		}
		if (L_sub(d, dmin) < 0) {
			dmin = d;
			cand = static_cast<i16>(i);
		}
	}
	return cand;
}

/* the second stage's codevector nearest rbuf - cb1 over [lo, hi), by
   the weighted distance */
static i16 lsp_select(const i16 *rbuf, const i16 *cb1, const i16 *wegt,
		      int lo, int hi)
{
	i16 buf[M];
	for (int j = lo; j < hi; j++)
		buf[j] = sub(rbuf[j], cb1[j]);
	i16 index = 0;
	i32 dmin = MAX32;
	for (int k = 0; k < NC1; k++) {
		i32 d = 0;
		for (int j = lo; j < hi; j++) {
			const i16 tmp = sub(buf[j], lspcb2[k][j]);
			const i16 tmp2 = mult(wegt[j], tmp);
			d = L_mac(d, tmp2, tmp);
		}
		if (L_sub(d, dmin) < 0) {
			dmin = d;
			index = static_cast<i16>(k);
		}
	}
	return index;
}

/* the weighted distance of buf from rbuf, in mode's terms */
static i32 lsp_get_tdist(const i16 *wegt, const i16 *buf, const i16 *rbuf,
			 const i16 *sum)
{
	i32 d = 0;
	for (int j = 0; j < M; j++) {
		i16 tmp = sub(buf[j], rbuf[j]);
		tmp = mult(tmp, sum[j]);
		const i32 t = L_mult(wegt[j], tmp);
		const i16 tmp2 = extract_h(L_shl(t, 4));
		d = L_mac(d, tmp2, tmp);
	}
	return d;
}

/* the LSPs quantised: the two parameters L0..L3, the quantised LSPs */
static void qua_lsp(LspPast &past, const i16 *lsp, i16 *lsp_q, i16 *ana)
{
	i16 lsf[M], lsf_q[M], wegt[M];
	lsp_lsf2(lsp, lsf);
	get_wegt(lsf, wegt);
	i16 cand[MODE], tindex1[MODE], tindex2[MODE];
	i32 tdist[MODE];
	for (i16 mode = 0; mode < MODE; mode++) {
		i16 rbuf[M], buf[M];
		lsp_prev_extract(lsf, rbuf, mode, past);
		const i16 c = lsp_pre_select(rbuf);
		cand[mode] = c;
		tindex1[mode] = lsp_select(rbuf, lspcb1[c], wegt, 0, NC);
		for (int j = 0; j < NC; j++)
			buf[j] = add(lspcb1[c][j], lspcb2[tindex1[mode]][j]);
		lsp_expand(buf, GAP1, 1, NC);
		tindex2[mode] = lsp_select(rbuf, lspcb1[c], wegt, NC, M);
		for (int j = NC; j < M; j++)
			buf[j] = add(lspcb1[c][j], lspcb2[tindex2[mode]][j]);
		lsp_expand(buf, GAP1, NC, M);
		lsp_expand(buf, GAP2, 1, M);
		tdist[mode] = lsp_get_tdist(wegt, buf, rbuf, fg_sum[mode]);
	}
	const i16 mode = L_sub(tdist[1], tdist[0]) < 0 ? 1 : 0;
	ana[0] = static_cast<i16>(shl(mode, NC0_B) | cand[mode]);
	ana[1] = static_cast<i16>(shl(tindex1[mode], NC1_B) | tindex2[mode]);
	lsp_get_quant(cand[mode], tindex1[mode], tindex2[mode], mode, past,
		      lsf_q);
	lsf_lsp2(lsf_q, lsp_q);
}

/* the open loop pitch: the best normalised correlation of the
   weighted speech in each of three lag ranges, favouring the shorter
   (A.3.4/G.729) */
static i16 pitch_ol_fast(const i16 *signal)
{
	i16 scaled[L_FRAME + PIT_MAX];
	i16 *sig = scaled + PIT_MAX;
	overflow() = false;
	i32 sum = 0;
	for (int i = -PIT_MAX; i < L_FRAME; i += 2)
		sum = L_mac(sum, signal[i], signal[i]);
	if (overflow()) {
		for (int i = -PIT_MAX; i < L_FRAME; i++)
			sig[i] = shr(signal[i], 3);
	} else if (L_sub(sum, 1048576) < 0) {
		for (int i = -PIT_MAX; i < L_FRAME; i++)
			sig[i] = shl(signal[i], 3);
	} else {
		for (int i = -PIT_MAX; i < L_FRAME; i++)
			sig[i] = signal[i];
	}

	/* the correlation at lag t over every other sample */
	auto corr = [sig](int t) {
		i32 s = 0;
		for (int j = 0; j < L_FRAME; j += 2)
			s = L_mac(s, sig[j], sig[j - t]);
		return s;
	};
	/* max normalised by the energy at lag t */
	auto norm = [sig](i32 max, int t) {
		i32 s = 1;
		for (int i = 0; i < L_FRAME; i += 2)
			s = L_mac(s, sig[i - t], sig[i - t]);
		s = Inv_sqrt(s);
		i16 max_h, max_l, en_h, en_l;
		L_Extract(max, &max_h, &max_l);
		L_Extract(s, &en_h, &en_l);
		return extract_l(Mpy_32(max_h, max_l, en_h, en_l));
	};

	i32 max = MIN32;
	i16 T1 = 20;
	for (int i = 20; i < 40; i++) {
		const i32 c = corr(i);
		if (L_sub(c, max) > 0) {
			max = c;
			T1 = static_cast<i16>(i);
		}
	}
	i16 max1 = norm(max, T1);

	max = MIN32;
	i16 T2 = 40;
	for (int i = 40; i < 80; i++) {
		const i32 c = corr(i);
		if (L_sub(c, max) > 0) {
			max = c;
			T2 = static_cast<i16>(i);
		}
	}
	i16 max2 = norm(max, T2);

	max = MIN32;
	i16 T3 = 80;
	for (int i = 80; i < 143; i += 2) {
		const i32 c = corr(i);
		if (L_sub(c, max) > 0) {
			max = c;
			T3 = static_cast<i16>(i);
		}
	}
	const i16 t3 = T3;
	i32 c = corr(t3 + 1);
	if (L_sub(c, max) > 0) {
		max = c;
		T3 = static_cast<i16>(t3 + 1);
	}
	c = corr(t3 - 1);
	if (L_sub(c, max) > 0) {
		max = c;
		T3 = static_cast<i16>(t3 - 1);
	}
	const i16 max3 = norm(max, T3);

	/* multiples */
	i16 i = sub(shl(T2, 1), T3);
	if (sub(abs_s(i), 5) < 0)
		max2 = add(max2, shr(max3, 2));
	i = add(i, T2);
	if (sub(abs_s(i), 7) < 0)
		max2 = add(max2, shr(max3, 2));
	i = sub(shl(T1, 1), T2);
	if (sub(abs_s(i), 5) < 0)
		max1 = add(max1, mult(max2, 6554));
	i = add(i, T1);
	if (sub(abs_s(i), 7) < 0)
		max1 = add(max1, mult(max2, 6554));
	if (sub(max1, max2) < 0) {
		max1 = max2;
		T1 = T2;
	}
	if (sub(max1, max3) < 0)
		T1 = T3;
	return T1;
}

/* x and y's dot product */
static i32 dot_product(const i16 *x, const i16 *y, int n)
{
	i32 s = 0;
	for (int i = 0; i < n; i++)
		s = L_mac(s, x[i], y[i]);
	return s;
}

/* the backward filtered target: h's correlation with x, scaled to 13
   bits (3.8.1/G.729) */
static void cor_h_x(const i16 *h, const i16 *x, i16 *d)
{
	i32 y32[L_SUBFR];
	i32 max = 0;
	for (int i = 0; i < L_SUBFR; i++) {
		i32 s = 0;
		for (int j = i; j < L_SUBFR; j++)
			s = L_mac(s, x[j], h[j - i]);
		y32[i] = s;
		s = L_abs(s);
		if (L_sub(s, max) > 0)
			max = s;
	}
	i16 j = norm_l(max);
	if (sub(j, 16) > 0)
		j = 16;
	j = sub(18, j);
	for (int i = 0; i < L_SUBFR; i++)
		d[i] = extract_l(L_shr(y32[i], j));
}

/* the closed loop pitch (A.3.7/G.729): the integer lag in
   [t0_min, t0_max] best correlated with the target, then its
   fractions -1/3 and 1/3 (not above 84 in the first subframe); exc
   gets the chosen vector */
static i16 pitch_fr3_fast(i16 *exc, const i16 *xn, const i16 *h,
			  i16 t0_min, i16 t0_max, int i_subfr, i16 *pit_frac)
{
	i16 Dn[L_SUBFR], exc_tmp[L_SUBFR];
	cor_h_x(h, xn, Dn);
	i32 max = MIN32;
	i16 t0 = t0_min;
	for (i16 t = t0_min; t <= t0_max; t++) {
		const i32 corr = dot_product(Dn, exc - t, L_SUBFR);
		if (L_sub(corr, max) > 0) {
			max = corr;
			t0 = t;
		}
	}
	pred_lt_3(exc, t0, 0, L_SUBFR);
	max = dot_product(Dn, exc, L_SUBFR);
	*pit_frac = 0;
	if (i_subfr == 0 && sub(t0, 84) > 0)
		return t0;
	for (int i = 0; i < L_SUBFR; i++)
		exc_tmp[i] = exc[i];
	pred_lt_3(exc, t0, -1, L_SUBFR);
	i32 corr = dot_product(Dn, exc, L_SUBFR);
	if (L_sub(corr, max) > 0) {
		max = corr;
		*pit_frac = -1;
		for (int i = 0; i < L_SUBFR; i++)
			exc_tmp[i] = exc[i];
	}
	pred_lt_3(exc, t0, 1, L_SUBFR);
	corr = dot_product(Dn, exc, L_SUBFR);
	if (L_sub(corr, max) > 0) {
		*pit_frac = 1;
	} else {
		for (int i = 0; i < L_SUBFR; i++)
			exc[i] = exc_tmp[i];
	}
	return t0;
}

/* the adaptive codebook gain (Q14, at most 1.2), and the correlations
   the gain quantiser needs */
static i16 g_pitch(const i16 *xn, const i16 *y1, i16 *g_coeff)
{
	i16 scaled[L_SUBFR];
	for (int i = 0; i < L_SUBFR; i++)
		scaled[i] = shr(y1[i], 2);

	overflow() = false;
	i32 s = 1;
	for (int i = 0; i < L_SUBFR; i++)
		s = L_mac(s, y1[i], y1[i]);
	i16 exp_yy, yy;
	if (!overflow()) {
		exp_yy = norm_l(s);
		yy = round16(L_shl(s, exp_yy));
	} else {
		s = 1;
		for (int i = 0; i < L_SUBFR; i++)
			s = L_mac(s, scaled[i], scaled[i]);
		exp_yy = norm_l(s);
		yy = round16(L_shl(s, exp_yy));
		exp_yy = sub(exp_yy, 4);
	}

	overflow() = false;
	s = 0;
	for (int i = 0; i < L_SUBFR; i++)
		s = L_mac(s, xn[i], y1[i]);
	i16 exp_xy, xy;
	if (!overflow()) {
		exp_xy = norm_l(s);
		xy = round16(L_shl(s, exp_xy));
	} else {
		s = 0;
		for (int i = 0; i < L_SUBFR; i++)
			s = L_mac(s, xn[i], scaled[i]);
		exp_xy = norm_l(s);
		xy = round16(L_shl(s, exp_xy));
		exp_xy = sub(exp_xy, 2);
	}

	g_coeff[0] = yy;
	g_coeff[1] = sub(15, exp_yy);
	g_coeff[2] = xy;
	g_coeff[3] = sub(15, exp_xy);
	if (xy <= 0) {
		g_coeff[3] = -15;
		return 0;
	}
	xy = shr(xy, 1);
	i16 gain = div_s(xy, yy);
	gain = shr(gain, sub(exp_xy, exp_yy));
	if (sub(gain, 19661) > 0)
		gain = 19661;
	return gain;
}

/* the pitch lag's index (3.7.1/G.729); the first subframe's sets the
   second's range */
static i16 enc_lag3(i16 T0, i16 T0_frac, i16 *T0_min, i16 *T0_max,
		    int i_subfr)
{
	i16 index;
	if (i_subfr == 0) {
		if (sub(T0, 85) <= 0) {
			const i16 i = add(add(T0, T0), T0);
			index = add(sub(i, 58), T0_frac);
		} else {
			index = add(T0, 112);
		}
		*T0_min = sub(T0, 5);
		if (sub(*T0_min, PIT_MIN) < 0)
			*T0_min = PIT_MIN;
		*T0_max = add(*T0_min, 9);
		if (sub(*T0_max, PIT_MAX) > 0) {
			*T0_max = PIT_MAX;
			*T0_min = sub(*T0_max, 9);
		}
	} else {
		i16 i = sub(T0, *T0_min);
		i = add(add(i, i), i);
		index = add(add(i, 2), T0_frac);
	}
	return index;
}

/* taming (3.10/G.729): whether the excitation's error at this lag is
   above the threshold */
static bool test_err(const i32 *exc_err, i16 T0, i16 T0_frac)
{
	const i16 t1 = T0_frac > 0 ? add(T0, 1) : T0;
	i16 i = sub(t1, L_SUBFR + L_INTER10);
	if (i < 0)
		i = 0;
	const i16 zone1 = tab_zone[i];
	const i16 zone2 = tab_zone[add(t1, L_INTER10 - 2)];
	i32 maxloc = -1;
	for (int z = zone2; z >= zone1; z--)
		if (L_sub(exc_err[z], maxloc) > 0)
			maxloc = exc_err[z];
	return L_sub(maxloc, L_THRESH_ERR) > 0;
}

/* (see test_err) the error moved on with the subframe's pitch gain */
static void update_exc_err(i32 *exc_err, i16 gain_pit, i16 T0)
{
	i32 worst = -1;
	/* one zone's error through the gain */
	auto step = [gain_pit](i32 e) {
		i16 hi, lo;
		L_Extract(e, &hi, &lo);
		i32 t = Mpy_32_16(hi, lo, gain_pit);
		t = L_shl(t, 1);
		return L_add(0x00004000, t);
	};
	const i16 n = sub(T0, L_SUBFR);
	if (n < 0) {
		i32 t = step(exc_err[0]);
		if (L_sub(t, worst) > 0)
			worst = t;
		t = step(t);
		if (L_sub(t, worst) > 0)
			worst = t;
	} else {
		const i16 zone1 = tab_zone[n];
		const i16 zone2 = tab_zone[sub(T0, 1)];
		for (int i = zone1; i <= zone2; i++) {
			const i32 t = step(exc_err[i]);
			if (L_sub(t, worst) > 0)
				worst = t;
		}
	}
	for (int i = 3; i >= 1; i--)
		exc_err[i] = exc_err[i - 1];
	exc_err[0] = worst;
}

/* the correlations of h (the matrix Phi) the codebook search needs, in
   rr's 616 entries (A.3.8/G.729) */
static void cor_h(const i16 *H, i16 *rr)
{
	i16 h[L_SUBFR];
	i32 cor = 0;
	for (int i = 0; i < L_SUBFR; i++)
		cor = L_mac(cor, H[i], H[i]);
	if (sub(extract_h(cor), 32000) > 0) {
		for (int i = 0; i < L_SUBFR; i++)
			h[i] = shr(H[i], 1);
	} else {
		const i16 k = shr(norm_l(cor), 1);
		for (int i = 0; i < L_SUBFR; i++)
			h[i] = shl(H[i], k);
	}

	i16 *rri0i0 = rr, *rri1i1 = rri0i0 + NB_POS, *rri2i2 = rri1i1 + NB_POS,
	    *rri3i3 = rri2i2 + NB_POS, *rri4i4 = rri3i3 + NB_POS;
	i16 *rri0i1 = rri4i4 + NB_POS, *rri0i2 = rri0i1 + MSIZE,
	    *rri0i3 = rri0i2 + MSIZE, *rri0i4 = rri0i3 + MSIZE,
	    *rri1i2 = rri0i4 + MSIZE, *rri1i3 = rri1i2 + MSIZE,
	    *rri1i4 = rri1i3 + MSIZE, *rri2i3 = rri1i4 + MSIZE,
	    *rri2i4 = rri2i3 + MSIZE;

	/* the diagonals */
	i16 *p0 = rri0i0 + NB_POS - 1, *p1 = rri1i1 + NB_POS - 1,
	    *p2 = rri2i2 + NB_POS - 1, *p3 = rri3i3 + NB_POS - 1,
	    *p4 = rri4i4 + NB_POS - 1;
	const i16 *h1 = h;
	cor = 0;
	for (int i = 0; i < NB_POS; i++) {
		cor = L_mac(cor, *h1, *h1); h1++;
		*p4-- = extract_h(cor);
		cor = L_mac(cor, *h1, *h1); h1++;
		*p3-- = extract_h(cor);
		cor = L_mac(cor, *h1, *h1); h1++;
		*p2-- = extract_h(cor);
		cor = L_mac(cor, *h1, *h1); h1++;
		*p1-- = extract_h(cor);
		cor = L_mac(cor, *h1, *h1); h1++;
		*p0-- = extract_h(cor);
	}

	const int ldec = NB_POS + 1;
	const i16 *hd = h;
	/* pulses one position apart */
	const i16 *hf = hd + 1;
	int sup = MSIZE - 1, inf = sup - 1;
	for (int k = 0; k < NB_POS; k++) {
		p3 = rri2i3 + sup;
		p2 = rri1i2 + sup;
		p1 = rri0i1 + sup;
		p0 = rri0i4 + inf;
		cor = 0;
		const i16 *a = hd, *b = hf;
		for (int i = k + 1; i < NB_POS; i++) {
			cor = L_mac(cor, *a++, *b++);
			cor = L_mac(cor, *a++, *b++);
			*p3 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p2 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p1 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p0 = extract_h(cor);
			p3 -= ldec;
			p2 -= ldec;
			p1 -= ldec;
			p0 -= ldec;
		}
		cor = L_mac(cor, *a++, *b++);
		cor = L_mac(cor, *a++, *b++);
		*p3 = extract_h(cor);
		cor = L_mac(cor, *a++, *b++);
		*p2 = extract_h(cor);
		cor = L_mac(cor, *a++, *b++);
		*p1 = extract_h(cor);
		sup -= NB_POS;
		inf--;
		hf += STEP;
	}

	/* two apart */
	hf = hd + 2;
	sup = MSIZE - 1;
	inf = sup - 1;
	for (int k = 0; k < NB_POS; k++) {
		p4 = rri2i4 + sup;
		p3 = rri1i3 + sup;
		p2 = rri0i2 + sup;
		p1 = rri1i4 + inf;
		p0 = rri0i3 + inf;
		cor = 0;
		const i16 *a = hd, *b = hf;
		for (int i = k + 1; i < NB_POS; i++) {
			cor = L_mac(cor, *a++, *b++);
			*p4 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p3 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p2 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p1 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p0 = extract_h(cor);
			p4 -= ldec;
			p3 -= ldec;
			p2 -= ldec;
			p1 -= ldec;
			p0 -= ldec;
		}
		cor = L_mac(cor, *a++, *b++);
		*p4 = extract_h(cor);
		cor = L_mac(cor, *a++, *b++);
		*p3 = extract_h(cor);
		cor = L_mac(cor, *a++, *b++);
		*p2 = extract_h(cor);
		sup -= NB_POS;
		inf--;
		hf += STEP;
	}

	/* three apart */
	hf = hd + 3;
	sup = MSIZE - 1;
	inf = sup - 1;
	for (int k = 0; k < NB_POS; k++) {
		p4 = rri1i4 + sup;
		p3 = rri0i3 + sup;
		p2 = rri2i4 + inf;
		p1 = rri1i3 + inf;
		p0 = rri0i2 + inf;
		cor = 0;
		const i16 *a = hd, *b = hf;
		for (int i = k + 1; i < NB_POS; i++) {
			cor = L_mac(cor, *a++, *b++);
			*p4 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p3 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p2 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p1 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p0 = extract_h(cor);
			p4 -= ldec;
			p3 -= ldec;
			p2 -= ldec;
			p1 -= ldec;
			p0 -= ldec;
		}
		cor = L_mac(cor, *a++, *b++);
		*p4 = extract_h(cor);
		cor = L_mac(cor, *a++, *b++);
		*p3 = extract_h(cor);
		sup -= NB_POS;
		inf--;
		hf += STEP;
	}

	/* four apart */
	hf = hd + 4;
	sup = MSIZE - 1;
	inf = sup - 1;
	for (int k = 0; k < NB_POS; k++) {
		p3 = rri0i4 + sup;
		p2 = rri2i3 + inf;
		p1 = rri1i2 + inf;
		p0 = rri0i1 + inf;
		cor = 0;
		const i16 *a = hd, *b = hf;
		for (int i = k + 1; i < NB_POS; i++) {
			cor = L_mac(cor, *a++, *b++);
			*p3 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			cor = L_mac(cor, *a++, *b++);
			*p2 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p1 = extract_h(cor);
			cor = L_mac(cor, *a++, *b++);
			*p0 = extract_h(cor);
			p3 -= ldec;
			p2 -= ldec;
			p1 -= ldec;
			p0 -= ldec;
		}
		cor = L_mac(cor, *a++, *b++);
		*p3 = extract_h(cor);
		sup -= NB_POS;
		inf--;
		hf += STEP;
	}
}

constexpr i16 Q_1_2 = 16384, Q_1_4 = 8192, Q_1_8 = 4096, Q_1_16 = 2048;

/* the 17 bit algebraic codebook's depth-first search (A.3.8/G.729):
   the four pulses' positions (the index) and signs, the codevector
   and its filtered version */
static i16 d4i40_17_fast(i16 *dn, i16 *rr, const i16 *h, i16 *cod, i16 *y,
			 i16 *sign)
{
	i16 *rri0i0 = rr, *rri1i1 = rri0i0 + NB_POS, *rri2i2 = rri1i1 + NB_POS,
	    *rri3i3 = rri2i2 + NB_POS, *rri4i4 = rri3i3 + NB_POS;
	i16 *rri0i1 = rri4i4 + NB_POS, *rri0i2 = rri0i1 + MSIZE,
	    *rri0i3 = rri0i2 + MSIZE, *rri0i4 = rri0i3 + MSIZE,
	    *rri1i2 = rri0i4 + MSIZE, *rri1i3 = rri1i2 + MSIZE,
	    *rri1i4 = rri1i3 + MSIZE, *rri2i3 = rri1i4 + MSIZE,
	    *rri2i4 = rri2i3 + MSIZE;
	i16 sign_dn[L_SUBFR], sign_dn_inv[L_SUBFR], tmp_vect[NB_POS];

	/* the signs from dn, folded into dn and rr */
	for (int i = 0; i < L_SUBFR; i++) {
		if (dn[i] >= 0) {
			sign_dn[i] = MAX16;
			sign_dn_inv[i] = MIN16;
		} else {
			sign_dn[i] = MIN16;
			sign_dn_inv[i] = MAX16;
			dn[i] = negate(dn[i]);
		}
	}
	i16 *p0 = rri0i1, *p1 = rri0i2, *p2 = rri0i3, *p3 = rri0i4, *p4;
	for (int i0 = 0; i0 < L_SUBFR; i0 += STEP) {
		const i16 *ps = sign_dn[i0] < 0 ? sign_dn_inv : sign_dn;
		for (int i1 = 1; i1 < L_SUBFR; i1 += STEP) {
			*p0 = mult(*p0, ps[i1]); p0++;
			*p1 = mult(*p1, ps[i1 + 1]); p1++;
			*p2 = mult(*p2, ps[i1 + 2]); p2++;
			*p3 = mult(*p3, ps[i1 + 3]); p3++;
		}
	}
	p0 = rri1i2;
	p1 = rri1i3;
	p2 = rri1i4;
	for (int i1 = 1; i1 < L_SUBFR; i1 += STEP) {
		const i16 *ps = sign_dn[i1] < 0 ? sign_dn_inv : sign_dn;
		for (int i2 = 2; i2 < L_SUBFR; i2 += STEP) {
			*p0 = mult(*p0, ps[i2]); p0++;
			*p1 = mult(*p1, ps[i2 + 1]); p1++;
			*p2 = mult(*p2, ps[i2 + 2]); p2++;
		}
	}
	p0 = rri2i3;
	p1 = rri2i4;
	for (int i2 = 2; i2 < L_SUBFR; i2 += STEP) {
		const i16 *ps = sign_dn[i2] < 0 ? sign_dn_inv : sign_dn;
		for (int i3 = 3; i3 < L_SUBFR; i3 += STEP) {
			*p0 = mult(*p0, ps[i3]); p0++;
			*p1 = mult(*p1, ps[i3 + 1]); p1++;
		}
	}

	i16 psk = -1, alpk = 1;
	i16 *rri0i3_i4 = rri0i3, *rri1i3_i4 = rri1i3, *rri2i3_i4 = rri2i3,
	    *rri3i3_i4 = rri3i3;
	i16 ip0 = 0, ip1 = 1, ip2 = 2, ip3 = 3, ix = 0, iy = 0, ps = 0;
	i16 i0 = 0, i1 = 0;

	for (i16 track = 3; track < 5; track++) {
		/* i2 then i3 (track 3 or 4), then i0 and i1 */
		i16 sq = -1, alp = 1, prev_i0 = -1;
		for (int n = 0; n < 2; n++) {
			i16 max = -1;
			for (i16 j = 2; j < L_SUBFR; j += STEP)
				if (sub(dn[j], max) > 0 && sub(prev_i0, j) != 0) {
					max = dn[j];
					i0 = j;
				}
			prev_i0 = i0;
			const i16 j = mult(i0, 6554);
			p0 = rri2i2 + j;
			const i16 ps1 = dn[i0];
			const i32 alp1 = L_mult(*p0, Q_1_4);
			p0 = rri2i3_i4 + shl(j, 3);
			p1 = rri3i3_i4;
			for (i16 k = track; k < L_SUBFR; k += STEP) {
				const i16 ps2 = add(ps1, dn[k]);
				i32 alp2 = L_mac(alp1, *p0++, Q_1_2);
				alp2 = L_mac(alp2, *p1++, Q_1_4);
				const i16 sq2 = mult(ps2, ps2);
				const i16 alp_16 = round16(alp2);
				const i32 s = L_msu(L_mult(alp, sq2), sq, alp_16);
				if (s > 0) {
					sq = sq2;
					ps = ps2;
					alp = alp_16;
					ix = i0;
					iy = k;
				}
			}
		}
		i0 = ix;
		i1 = iy;
		i16 i1_offset = shl(mult(i1, 6554), 3);
		i16 ps0 = ps;
		i32 alp0 = L_mult(alp, Q_1_4);
		sq = -1;
		alp = 1;
		p0 = rri1i2 + mult(i0, 6554);
		p1 = rri1i3_i4 + mult(i1, 6554);
		p2 = rri1i1;
		p3 = tmp_vect;
		for (int k = 1; k < L_SUBFR; k += STEP) {
			i32 s = L_mult(*p0, Q_1_4); p0 += NB_POS;
			s = L_mac(s, *p1, Q_1_4); p1 += NB_POS;
			s = L_mac(s, *p2++, Q_1_8);
			*p3++ = round16(s);
		}
		p0 = rri0i2 + mult(i0, 6554);
		p1 = rri0i3_i4 + mult(i1, 6554);
		p2 = rri0i0;
		p3 = rri0i1;
		for (i16 a = 0; a < L_SUBFR; a += STEP) {
			const i16 ps1 = add(ps0, dn[a]);
			i32 alp1 = L_mac(alp0, *p0, Q_1_8); p0 += NB_POS;
			alp1 = L_mac(alp1, *p1, Q_1_8); p1 += NB_POS;
			alp1 = L_mac(alp1, *p2++, Q_1_16);
			p4 = tmp_vect;
			for (i16 b = 1; b < L_SUBFR; b += STEP) {
				const i16 ps2 = add(ps1, dn[b]);
				i32 alp2 = L_mac(alp1, *p3++, Q_1_8);
				alp2 = L_mac(alp2, *p4++, Q_1_2);
				const i16 sq2 = mult(ps2, ps2);
				const i16 alp_16 = round16(alp2);
				const i32 s = L_msu(L_mult(alp, sq2), sq, alp_16);
				if (s > 0) {
					sq = sq2;
					alp = alp_16;
					ix = a;
					iy = b;
				}
			}
		}
		if (L_msu(L_mult(alpk, sq), psk, alp) > 0) {
			psk = sq;
			alpk = alp;
			ip2 = i0;
			ip3 = i1;
			ip0 = ix;
			ip1 = iy;
		}

		/* i3 (track 3 or 4) then i0, then i1 and i2 */
		sq = -1;
		alp = 1;
		prev_i0 = -1;
		for (int n = 0; n < 2; n++) {
			i16 max = -1;
			for (i16 j = track; j < L_SUBFR; j += STEP)
				if (sub(dn[j], max) > 0 && sub(prev_i0, j) != 0) {
					max = dn[j];
					i0 = j;
				}
			prev_i0 = i0;
			const i16 j = mult(i0, 6554);
			p0 = rri3i3_i4 + j;
			const i16 ps1 = dn[i0];
			const i32 alp1 = L_mult(*p0, Q_1_4);
			p0 = rri0i3_i4 + j;
			p1 = rri0i0;
			for (i16 k = 0; k < L_SUBFR; k += STEP) {
				const i16 ps2 = add(ps1, dn[k]);
				i32 alp2 = L_mac(alp1, *p0, Q_1_2); p0 += NB_POS;
				alp2 = L_mac(alp2, *p1++, Q_1_4);
				const i16 sq2 = mult(ps2, ps2);
				const i16 alp_16 = round16(alp2);
				const i32 s = L_msu(L_mult(alp, sq2), sq, alp_16);
				if (s > 0) {
					sq = sq2;
					ps = ps2;
					alp = alp_16;
					ix = i0;
					iy = k;
				}
			}
		}
		i0 = ix;
		i1 = iy;
		i1_offset = shl(mult(i1, 6554), 3);
		ps0 = ps;
		alp0 = L_mult(alp, Q_1_4);
		sq = -1;
		alp = 1;
		p0 = rri2i3_i4 + mult(i0, 6554);
		p1 = rri0i2 + i1_offset;
		p2 = rri2i2;
		p3 = tmp_vect;
		for (int k = 2; k < L_SUBFR; k += STEP) {
			i32 s = L_mult(*p0, Q_1_4); p0 += NB_POS;
			s = L_mac(s, *p1++, Q_1_4);
			s = L_mac(s, *p2++, Q_1_8);
			*p3++ = round16(s);
		}
		p0 = rri1i3_i4 + mult(i0, 6554);
		p1 = rri0i1 + i1_offset;
		p2 = rri1i1;
		p3 = rri1i2;
		for (i16 a = 1; a < L_SUBFR; a += STEP) {
			const i16 ps1 = add(ps0, dn[a]);
			i32 alp1 = L_mac(alp0, *p0, Q_1_8); p0 += NB_POS;
			alp1 = L_mac(alp1, *p1++, Q_1_8);
			alp1 = L_mac(alp1, *p2++, Q_1_16);
			p4 = tmp_vect;
			for (i16 b = 2; b < L_SUBFR; b += STEP) {
				const i16 ps2 = add(ps1, dn[b]);
				i32 alp2 = L_mac(alp1, *p3++, Q_1_8);
				alp2 = L_mac(alp2, *p4++, Q_1_2);
				const i16 sq2 = mult(ps2, ps2);
				const i16 alp_16 = round16(alp2);
				const i32 s = L_msu(L_mult(alp, sq2), sq, alp_16);
				if (s > 0) {
					sq = sq2;
					alp = alp_16;
					ix = a;
					iy = b;
				}
			}
		}
		if (L_msu(L_mult(alpk, sq), psk, alp) > 0) {
			psk = sq;
			alpk = alp;
			ip3 = i0;
			ip0 = i1;
			ip1 = ix;
			ip2 = iy;
		}

		rri0i3_i4 = rri0i4;
		rri1i3_i4 = rri1i4;
		rri2i3_i4 = rri2i4;
		rri3i3_i4 = rri4i4;
	}

	/* the codevector and its filtered version */
	const i16 s0 = sign_dn[ip0], s1 = sign_dn[ip1], s2 = sign_dn[ip2],
		s3 = sign_dn[ip3];
	for (int i = 0; i < L_SUBFR; i++)
		cod[i] = 0;
	cod[ip0] = shr(s0, 2);
	cod[ip1] = shr(s1, 2);
	cod[ip2] = shr(s2, 2);
	cod[ip3] = shr(s3, 2);
	for (int i = 0; i < ip0; i++)
		y[i] = 0;
	for (int i = ip0, j = 0; i < L_SUBFR; i++, j++)
		y[i] = s0 > 0 ? h[j] : negate(h[j]);
	const i16 ip[3] = { ip1, ip2, ip3 }, sg[3] = { s1, s2, s3 };
	for (int n = 0; n < 3; n++)
		for (int i = ip[n], j = 0; i < L_SUBFR; i++, j++)
			y[i] = sg[n] > 0 ? add(y[i], h[j]) : sub(y[i], h[j]);

	i16 i = 0;
	if (s0 > 0)
		i = add(i, 1);
	if (s1 > 0)
		i = add(i, 2);
	if (s2 > 0)
		i = add(i, 4);
	if (s3 > 0)
		i = add(i, 8);
	*sign = i;

	ip0 = mult(ip0, 6554);
	ip1 = mult(ip1, 6554);
	ip2 = mult(ip2, 6554);
	i = mult(ip3, 6554);
	i16 j = add(i, shl(i, 2));
	j = sub(ip3, add(j, 3));
	ip3 = add(shl(i, 1), j);
	i = add(ip0, shl(ip1, 3));
	i = add(i, shl(ip2, 6));
	i = add(i, shl(ip3, 9));
	return i;
}

/* the fixed codebook (Annex A's search) with the pitch sharpening:
   the positions' index, the signs, the codevector, its filtered
   version (h is sharpened in place) */
static i16 acelp_code_a(const i16 *x, i16 *h, i16 T0, i16 pitch_sharp,
			i16 *code, i16 *y, i16 *sign)
{
	i16 Dn[L_SUBFR], rr[DIM_RR];
	const i16 sharp = shl(pitch_sharp, 1);
	if (T0 < L_SUBFR)
		for (int i = T0; i < L_SUBFR; i++)
			h[i] = add(h[i], mult(h[i - T0], sharp));
	cor_h(h, rr);
	cor_h_x(h, x, Dn);
	const i16 index = d4i40_17_fast(Dn, rr, h, code, y, sign);
	if (T0 < L_SUBFR)
		for (int i = T0; i < L_SUBFR; i++)
			code[i] = add(code[i], mult(code[i - T0], sharp));
	return index;
}

/* the correlations with the filtered codevector y2 the gain quantiser
   needs, beside g_pitch()'s */
static void corr_xy2(const i16 *xn, const i16 *y1, const i16 *y2,
		     i16 *g_coeff, i16 *exp_g_coeff)
{
	i16 scaled[L_SUBFR];
	for (int i = 0; i < L_SUBFR; i++)
		scaled[i] = shr(y2[i], 3);

	i32 acc = 1;
	for (int i = 0; i < L_SUBFR; i++)
		acc = L_mac(acc, scaled[i], scaled[i]);
	i16 e = norm_l(acc);
	g_coeff[2] = round16(L_shl(acc, e));
	exp_g_coeff[2] = add(e, 19 - 16);

	acc = 1;
	for (int i = 0; i < L_SUBFR; i++)
		acc = L_mac(acc, xn[i], scaled[i]);
	e = norm_l(acc);
	g_coeff[3] = negate(round16(L_shl(acc, e)));
	exp_g_coeff[3] = sub(add(e, 10 - 16), 1);

	acc = 1;
	for (int i = 0; i < L_SUBFR; i++)
		acc = L_mac(acc, y1[i], scaled[i]);
	e = norm_l(acc);
	g_coeff[4] = round16(L_shl(acc, e));
	exp_g_coeff[4] = sub(add(e, 10 - 16), 1);
}

/* the gain codebooks' preselection about the best unquantised gains */
static void gbk_presel(const i16 *best_gain, i16 *cand1, i16 *cand2,
		       i16 gcode0)
{
	i32 cfbg = L_mult(coef[0][0], best_gain[0]);
	i32 acc = L_shr(L_coef[1][1], 15);
	acc = L_add(cfbg, acc);
	i16 acc_h = extract_h(acc);
	i32 preg = L_mult(acc_h, gcode0);
	acc = L_shl(L_deposit_l(best_gain[1]), 7);
	acc = L_sub(acc, preg);
	acc_h = extract_h(L_shl(acc, 2));
	const i32 tmp_x = L_mult(acc_h, INV_COEF);

	acc = L_shr(L_coef[0][1], 10);
	acc = L_sub(cfbg, acc);
	acc_h = extract_h(acc);
	acc_h = mult(acc_h, gcode0);
	const i32 tmp = L_mult(acc_h, coef[1][0]);
	preg = L_mult(coef[0][0], best_gain[1]);
	acc = L_sub(tmp, L_shr(preg, 3));
	acc_h = extract_h(L_shl(acc, 2));
	const i32 tmp_y = L_mult(acc_h, INV_COEF);

	const i16 sft_y = (14 + 4 + 1) - 16, sft_x = (15 + 4 + 1) - 15;
	*cand1 = 0;
	*cand2 = 0;
	if (gcode0 > 0) {
		do {
			if (L_sub(tmp_y, L_shr(L_mult(thr1[*cand1], gcode0),
					       sft_y)) > 0)
				*cand1 = add(*cand1, 1);
			else
				break;
		} while (sub(*cand1, NCODE1 - NCAN1) < 0);
		do {
			if (L_sub(tmp_x, L_shr(L_mult(thr2[*cand2], gcode0),
					       sft_x)) > 0)
				*cand2 = add(*cand2, 1);
			else
				break;
		} while (sub(*cand2, NCODE2 - NCAN2) < 0);
	} else {
		do {
			if (L_sub(tmp_y, L_shr(L_mult(thr1[*cand1], gcode0),
					       sft_y)) < 0)
				*cand1 = add(*cand1, 1);
			else
				break;
		} while (sub(*cand1, NCODE1 - NCAN1) != 0);
		do {
			if (L_sub(tmp_x, L_shr(L_mult(thr2[*cand2], gcode0),
					       sft_x)) < 0)
				*cand2 = add(*cand2, 1);
			else
				break;
		} while (sub(*cand2, NCODE2 - NCAN2) != 0);
	}
}

/* the gains quantised (A.3.9/G.729): their index; the quantised pitch
   and code gains; with taming, a pitch gain below 1 */
static i16 qua_gain(i16 *past_qua_en, const i16 *code, const i16 *g_coeff,
		    const i16 *exp_coeff, i16 *gain_pit, i16 *gain_cod,
		    bool taming)
{
	i16 gcode0, exp_gcode0;
	gain_predict(past_qua_en, code, &gcode0, &exp_gcode0);

	/* the best gains unquantised (A.3.9.1/G.729) */
	i32 tmp1 = L_mult(g_coeff[0], g_coeff[2]);
	i16 exp1 = add(add(exp_coeff[0], exp_coeff[2]), 1 - 2);
	i32 tmp2 = L_mult(g_coeff[4], g_coeff[4]);
	i16 exp2 = add(add(exp_coeff[4], exp_coeff[4]), 1);
	i32 t;
	i16 exp;
	if (sub(exp1, exp2) > 0) {
		t = L_sub(L_shr(tmp1, sub(exp1, exp2)), tmp2);
		exp = exp2;
	} else {
		t = L_sub(tmp1, L_shr(tmp2, sub(exp2, exp1)));
		exp = exp1;
	}
	i16 sft = norm_l(t);
	const i16 denom = extract_h(L_shl(t, sft));
	const i16 exp_denom = sub(add(exp, sft), 16);
	const i16 inv_denom = negate(div_s(16384, denom));
	const i16 exp_inv_denom = sub(14 + 15, exp_denom);

	/* numerator a * d - b * c (or the other), shifted down one */
	auto numer = [&](i16 c1, i16 c2, i16 c3, i16 c4, i16 *exp_nume) {
		i32 a = L_mult(g_coeff[c1], g_coeff[c2]);
		const i16 ea = add(exp_coeff[c1], exp_coeff[c2]);
		i32 b = L_mult(g_coeff[c3], g_coeff[c4]);
		const i16 eb = add(add(exp_coeff[c3], exp_coeff[c4]), 1);
		i32 r;
		i16 e;
		if (sub(ea, eb) > 0) {
			r = L_sub(L_shr(a, add(sub(ea, eb), 1)), L_shr(b, 1));
			e = sub(eb, 1);
		} else {
			r = L_sub(L_shr(a, 1), L_shr(b, add(sub(eb, ea), 1)));
			e = sub(ea, 1);
		}
		const i16 s = norm_l(r);
		*exp_nume = sub(add(e, s), 16);
		return extract_h(L_shl(r, s));
	};
	i16 best_gain[2], exp_nume;
	i16 nume = numer(2, 1, 3, 4, &exp_nume);
	sft = sub(add(exp_nume, exp_inv_denom), 9 + 16 - 1);
	best_gain[0] = extract_h(L_shr(L_mult(nume, inv_denom), sft));
	if (taming && sub(best_gain[0], GPCLIP2) > 0)
		best_gain[0] = GPCLIP2;
	nume = numer(0, 3, 1, 4, &exp_nume);
	sft = sub(add(exp_nume, exp_inv_denom), 2 + 16 - 1);
	best_gain[1] = extract_h(L_shr(L_mult(nume, inv_denom), sft));

	i16 gcode0_org;
	if (sub(exp_gcode0, 4) >= 0) {
		gcode0_org = shr(gcode0, sub(exp_gcode0, 4));
	} else {
		const i32 a = L_shl(L_deposit_l(gcode0),
				    sub(4 + 16, exp_gcode0));
		gcode0_org = extract_h(a);
	}
	i16 cand1, cand2;
	gbk_presel(best_gain, &cand1, &cand2, gcode0_org);

	/* the candidates by the error's energy (A.3.9.2/G.729) */
	i16 exp_min[5], coeff[5], coeff_lsf[5];
	exp_min[0] = add(exp_coeff[0], 13);
	exp_min[1] = add(exp_coeff[1], 14);
	exp_min[2] = add(exp_coeff[2], sub(shl(exp_gcode0, 1), 21));
	exp_min[3] = add(exp_coeff[3], sub(exp_gcode0, 3));
	exp_min[4] = add(exp_coeff[4], sub(exp_gcode0, 4));
	i16 e_min = exp_min[0];
	for (int i = 1; i < 5; i++)
		if (sub(exp_min[i], e_min) < 0)
			e_min = exp_min[i];
	for (int i = 0; i < 5; i++) {
		const i16 j = sub(exp_min[i], e_min);
		i32 a = L_deposit_h(g_coeff[i]);
		a = L_shr(a, j);
		L_Extract(a, &coeff[i], &coeff_lsf[i]);
	}

	i32 dist_min = MAX32;
	i16 index1 = cand1, index2 = cand2;
	for (int i = 0; i < NCAN1; i++)
		for (int j = 0; j < NCAN2; j++) {
			const i16 g_pitch = add(gbk1[cand1 + i][0],
						gbk2[cand2 + j][0]);
			if (taming && g_pitch >= GP0999)
				continue;
			i32 a = L_add(L_deposit_l(gbk1[cand1 + i][1]),
				      L_deposit_l(gbk2[cand2 + j][1]));
			const i16 tmp = extract_l(L_shr(a, 1));
			const i16 g_code = mult(gcode0, tmp);
			const i16 g2_pitch = mult(g_pitch, g_pitch);
			const i16 g2_code = mult(g_code, g_code);
			const i16 g_pit_cod = mult(g_code, g_pitch);
			a = Mpy_32_16(coeff[0], coeff_lsf[0], g2_pitch);
			a = L_add(a, Mpy_32_16(coeff[1], coeff_lsf[1], g_pitch));
			a = L_add(a, Mpy_32_16(coeff[2], coeff_lsf[2], g2_code));
			a = L_add(a, Mpy_32_16(coeff[3], coeff_lsf[3], g_code));
			a = L_add(a, Mpy_32_16(coeff[4], coeff_lsf[4], g_pit_cod));
			if (L_sub(a, dist_min) < 0) {
				dist_min = a;
				index1 = add(cand1, static_cast<i16>(i));
				index2 = add(cand2, static_cast<i16>(j));
			}
		}

	*gain_pit = add(gbk1[index1][0], gbk2[index2][0]);
	const i32 gbk12 = L_add(L_deposit_l(gbk1[index1][1]),
				L_deposit_l(gbk2[index2][1]));
	const i16 tmp = extract_l(L_shr(gbk12, 1));
	i32 a = L_mult(tmp, gcode0);
	a = L_shl(a, add(negate(exp_gcode0), -12 - 1 + 1 + 16));
	*gain_cod = extract_h(a);
	gain_update(past_qua_en, gbk12);
	return add(static_cast<i16>(map1[index1] * NCODE2), map2[index2]);
}

/* (see g729a.h) */
Encoder::Encoder() : st_(new EncState)
{
	reset();
}

/* (see g729a.h) */
Encoder::~Encoder() = default;

/* (see g729a.h) */
void Encoder::reset()
{
	static const i16 lsp_init[M] = { 30000, 26000, 21000, 15000, 8000, 0,
					 -8000, -15000, -21000, -26000 };
	EncState &s = *st_;
	s = EncState();
	for (int i = 0; i < M; i++)
		s.lsp_old[i] = s.lsp_old_q[i] = lsp_init[i];
	s.sharp = SHARPMIN;
	s.old_A[0] = 4096;
	s.lsp.reset();
	for (i16 &e : s.past_qua_en)
		e = -14336;
	for (i32 &e : s.exc_err)
		e = 0x00004000;
	s.hp.reset();
}

/* (see g729a.h): clause 3 with Annex A's simplifications */
void Encoder::encode(const int16_t pcm[FRAME], int16_t prm[PRMS])
{
	EncState &s = *st_;
	i16 *new_speech = s.old_speech + L_TOTAL - L_FRAME;
	i16 *speech = new_speech - L_NEXT;
	i16 *p_window = s.old_speech + L_TOTAL - L_WINDOW;
	i16 *wsp = s.old_wsp + PIT_MAX;
	i16 *exc = s.old_exc + PIT_MAX + L_INTERPOL;
	for (int i = 0; i < L_FRAME; i++)
		new_speech[i] = pcm[i];
	s.hp.pre(new_speech, L_FRAME);

	i16 Aq_t[MP1 * 2], Ap_t[MP1 * 2];
	i16 *ana = prm;

	/* LP analysis, the LSPs quantised and interpolated */
	{
		i16 r_l[MP1], r_h[MP1], rc[M], lsp_new[M], lsp_new_q[M];
		autocorr(p_window, r_h, r_l);
		lag_window(r_h, r_l);
		levinson(s, r_h, r_l, Ap_t, rc);
		az_lsp(Ap_t, lsp_new, s.lsp_old);
		qua_lsp(s.lsp, lsp_new, lsp_new_q, ana);
		ana += 2;
		int_qlpc(s.lsp_old_q, lsp_new_q, Aq_t);
		weight_az(&Aq_t[0], GAMMA1, &Ap_t[0]);
		weight_az(&Aq_t[MP1], GAMMA1, &Ap_t[MP1]);
		for (int i = 0; i < M; i++) {
			s.lsp_old[i] = lsp_new[i];
			s.lsp_old_q[i] = lsp_new_q[i];
		}
	}

	/* the weighted speech: the residual through A(z/g)/(1 - 0.7z^-1)
	   (A.3.3/G.729) */
	residu(&Aq_t[0], &speech[0], &exc[0], L_SUBFR);
	residu(&Aq_t[MP1], &speech[L_SUBFR], &exc[L_SUBFR], L_SUBFR);
	{
		i16 Ap1[MP1];
		const i16 *Ap = Ap_t;
		Ap1[0] = 4096;
		for (int i = 1; i <= M; i++)
			Ap1[i] = sub(Ap[i], mult(Ap[i - 1], 22938));
		syn_filt(Ap1, &exc[0], &wsp[0], L_SUBFR, s.mem_w, true);
		Ap += MP1;
		for (int i = 1; i <= M; i++)
			Ap1[i] = sub(Ap[i], mult(Ap[i - 1], 22938));
		syn_filt(Ap1, &exc[L_SUBFR], &wsp[L_SUBFR], L_SUBFR, s.mem_w,
			 true);
	}

	/* the open loop pitch and the first subframe's search range */
	const i16 T_op = pitch_ol_fast(wsp);
	i16 T0_min = sub(T_op, 3);
	if (sub(T0_min, PIT_MIN) < 0)
		T0_min = PIT_MIN;
	i16 T0_max = add(T0_min, 6);
	if (sub(T0_max, PIT_MAX) > 0) {
		T0_max = PIT_MAX;
		T0_min = sub(T0_max, 6);
	}

	const i16 *Ap = Ap_t;
	for (int i_subfr = 0; i_subfr < L_FRAME; i_subfr += L_SUBFR) {
		i16 h1[L_SUBFR], xn[L_SUBFR], xn2[L_SUBFR], code[L_SUBFR],
			y1[L_SUBFR], y2[L_SUBFR], g_coeff[4], g_coeff_cs[5],
			exp_g_coeff_cs[5];

		/* the weighted synthesis filter's impulse response, the
		   target */
		h1[0] = 4096;
		for (int i = 1; i < L_SUBFR; i++)
			h1[i] = 0;
		syn_filt(Ap, h1, h1, L_SUBFR, &h1[1], false);
		syn_filt(Ap, &exc[i_subfr], xn, L_SUBFR, s.mem_w0, false);

		/* the adaptive codebook */
		i16 T0_frac;
		const i16 T0 = pitch_fr3_fast(&exc[i_subfr], xn, h1, T0_min,
					      T0_max, i_subfr, &T0_frac);
		i16 index = enc_lag3(T0, T0_frac, &T0_min, &T0_max, i_subfr);
		*ana++ = index;
		if (i_subfr == 0)
			*ana++ = parity_pitch(index);
		syn_filt(Ap, &exc[i_subfr], y1, L_SUBFR, s.mem_zero, false);
		i16 gain_pit = g_pitch(xn, y1, g_coeff);
		const bool taming = test_err(s.exc_err, T0, T0_frac);
		if (taming && sub(gain_pit, GPCLIP) > 0)
			gain_pit = GPCLIP;
		for (int i = 0; i < L_SUBFR; i++) {
			const i32 t = L_shl(L_mult(y1[i], gain_pit), 1);
			xn2[i] = sub(xn[i], extract_h(t));
		}

		/* the fixed codebook */
		i16 sign;
		index = acelp_code_a(xn2, h1, T0, s.sharp, code, y2, &sign);
		*ana++ = index;
		*ana++ = sign;

		/* the gains */
		g_coeff_cs[0] = g_coeff[0];
		exp_g_coeff_cs[0] = negate(g_coeff[1]);
		g_coeff_cs[1] = negate(g_coeff[2]);
		exp_g_coeff_cs[1] = negate(add(g_coeff[3], 1));
		corr_xy2(xn, y1, y2, g_coeff_cs, exp_g_coeff_cs);
		i16 gain_code;
		*ana++ = qua_gain(s.past_qua_en, code, g_coeff_cs,
				  exp_g_coeff_cs, &gain_pit, &gain_code, taming);

		s.sharp = gain_pit;
		if (sub(s.sharp, SHARPMAX) > 0)
			s.sharp = SHARPMAX;
		if (sub(s.sharp, SHARPMIN) < 0)
			s.sharp = SHARPMIN;

		/* the excitation and the filters' memories */
		for (int i = 0; i < L_SUBFR; i++) {
			i32 t = L_mult(exc[i + i_subfr], gain_pit);
			t = L_mac(t, code[i], gain_code);
			t = L_shl(t, 1);
			exc[i + i_subfr] = round16(t);
		}
		update_exc_err(s.exc_err, gain_pit, T0);
		for (int i = L_SUBFR - M, j = 0; i < L_SUBFR; i++, j++) {
			const i16 temp = extract_h(L_shl(L_mult(y1[i], gain_pit), 1));
			const i16 k = extract_h(L_shl(L_mult(y2[i], gain_code), 2));
			s.mem_w0[j] = sub(xn[i], add(temp, k));
		}
		Ap += MP1;
	}

	for (int i = 0; i < L_TOTAL - L_FRAME; i++)
		s.old_speech[i] = s.old_speech[i + L_FRAME];
	for (int i = 0; i < PIT_MAX; i++)
		s.old_wsp[i] = s.old_wsp[i + L_FRAME];
	for (int i = 0; i < PIT_MAX + L_INTERPOL; i++)
		s.old_exc[i] = s.old_exc[i + L_FRAME];
}

/* (see g729a.h) */
void Encoder::encode(const int16_t pcm[FRAME], uint8_t out[FRAME_OCTETS])
{
	int16_t prm[PRMS];
	encode(pcm, prm);
	pack(prm, out);
}

} // namespace g729
} // namespace dlink
