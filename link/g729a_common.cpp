/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    g729a_common.cpp - what G.729 Annex A's encoder and decoder share
 *                       (g729a_int.h): the 32 bit divisions and the
 *                       power, logarithm and inverse square root
 *                       (g729_ops.h), the LP filters, the LSP
 *                       quantiser's decoding, the gain prediction and
 *                       the frame's packing (g729a.h)
 *
 */

#include "g729a.h"
#include "g729a_int.h"

namespace dlink {
namespace g729 {

/* (see g729_ops.h): Newton's 1/den, then num times it */
i32 Div_32(i32 num, i16 den_hi, i16 den_lo)
{
	const i16 approx = div_s(0x3fff, den_hi);
	i32 t = Mpy_32_16(den_hi, den_lo, approx);
	t = L_sub(MAX32, t);
	i16 hi, lo, n_hi, n_lo;
	L_Extract(t, &hi, &lo);
	t = Mpy_32_16(hi, lo, approx);
	L_Extract(t, &hi, &lo);
	L_Extract(num, &n_hi, &n_lo);
	t = Mpy_32(n_hi, n_lo, hi, lo);
	return L_shl(t, 2);
}

/* (see g729_ops.h): tabpow interpolated */
i32 Pow2(i16 exponent, i16 fraction)
{
	i32 x = L_mult(fraction, 32);
	const i16 i = extract_h(x);
	x = L_shr(x, 1);
	const i16 a = static_cast<i16>(extract_l(x) & 0x7fff);
	x = L_deposit_h(tabpow[i]);
	const i16 tmp = sub(tabpow[i], tabpow[i + 1]);
	x = L_msu(x, tmp, a);
	return L_shr_r(x, sub(30, exponent));
}

/* (see g729_ops.h): tablog interpolated; 0 for x <= 0 */
void Log2(i32 x, i16 *exponent, i16 *fraction)
{
	if (x <= 0) {
		*exponent = 0;
		*fraction = 0;
		return;
	}
	const i16 e = norm_l(x);
	x = L_shl(x, e);
	*exponent = sub(30, e);
	x = L_shr(x, 9);
	i16 i = extract_h(x);
	x = L_shr(x, 1);
	const i16 a = static_cast<i16>(extract_l(x) & 0x7fff);
	i = sub(i, 32);
	i32 y = L_deposit_h(tablog[i]);
	const i16 tmp = sub(tablog[i], tablog[i + 1]);
	y = L_msu(y, tmp, a);
	*fraction = extract_h(y);
}

/* (see g729_ops.h): tabsqr interpolated */
i32 Inv_sqrt(i32 x)
{
	if (x <= 0)
		return 0x3fffffff;
	i16 e = norm_l(x);
	x = L_shl(x, e);
	e = sub(30, e);
	if ((e & 1) == 0)
		x = L_shr(x, 1);
	e = shr(e, 1);
	e = add(e, 1);
	x = L_shr(x, 9);
	i16 i = extract_h(x);
	x = L_shr(x, 1);
	const i16 a = static_cast<i16>(extract_l(x) & 0x7fff);
	i = sub(i, 16);
	i32 y = L_deposit_h(tabsqr[i]);
	const i16 tmp = sub(tabsqr[i], tabsqr[i + 1]);
	y = L_msu(y, tmp, a);
	return L_shr(y, e);
}

/* (see g729a_int.h) */
void HighPass::pre(i16 *sig, int n)
{
	for (int i = 0; i < n; i++) {
		const i16 x2 = x1;
		x1 = x0;
		x0 = sig[i];
		i32 t = Mpy_32_16(y1_hi, y1_lo, a140[1]);
		t = L_add(t, Mpy_32_16(y2_hi, y2_lo, a140[2]));
		t = L_mac(t, x0, b140[0]);
		t = L_mac(t, x1, b140[1]);
		t = L_mac(t, x2, b140[2]);
		t = L_shl(t, 3);
		sig[i] = round16(t);
		y2_hi = y1_hi;
		y2_lo = y1_lo;
		L_Extract(t, &y1_hi, &y1_lo);
	}
}

/* (see g729a_int.h) */
void HighPass::post(i16 *sig, int n)
{
	for (int i = 0; i < n; i++) {
		const i16 x2 = x1;
		x1 = x0;
		x0 = sig[i];
		i32 t = Mpy_32_16(y1_hi, y1_lo, a100[1]);
		t = L_add(t, Mpy_32_16(y2_hi, y2_lo, a100[2]));
		t = L_mac(t, x0, b100[0]);
		t = L_mac(t, x1, b100[1]);
		t = L_mac(t, x2, b100[2]);
		t = L_shl(t, 2);
		sig[i] = round16(L_shl(t, 1));
		y2_hi = y1_hi;
		y2_lo = y1_lo;
		L_Extract(t, &y1_hi, &y1_lo);
	}
}

/* F1(z) or F2(z) from every other LSP (3.2.6/G.729), in Q24 */
static void lsp_pol(const i16 *lsp, i32 *f)
{
	f[0] = L_mult(4096, 2048);
	f[1] = L_msu(0, lsp[0], 512);
	for (int i = 2; i <= 5; i++) {
		const i16 l = lsp[2 * (i - 1)];
		f[i] = f[i - 2];
		for (int j = i; j > 1; j--) {
			i16 hi, lo;
			L_Extract(f[j - 1], &hi, &lo);
			i32 t = Mpy_32_16(hi, lo, l);
			t = L_shl(t, 1);
			f[j] = L_add(f[j], f[j - 2]);
			f[j] = L_sub(f[j], t);
		}
		f[1] = L_msu(f[1], l, 512);
	}
}

/* (see g729a_int.h) */
void lsp_az(const i16 *lsp, i16 *a)
{
	i32 f1[6], f2[6];
	lsp_pol(lsp, f1);
	lsp_pol(lsp + 1, f2);
	for (int i = 5; i > 0; i--) {
		f1[i] = L_add(f1[i], f1[i - 1]);
		f2[i] = L_sub(f2[i], f2[i - 1]);
	}
	a[0] = 4096;
	for (int i = 1, j = 10; i <= 5; i++, j--) {
		a[i] = extract_l(L_shr_r(L_add(f1[i], f2[i]), 13));
		a[j] = extract_l(L_shr_r(L_sub(f1[i], f2[i]), 13));
	}
}

/* (see g729a_int.h) */
void lsf_lsp2(const i16 *lsf, i16 *lsp)
{
	for (int i = 0; i < M; i++) {
		const i16 freq = mult(lsf[i], 20861);
		i16 ind = shr(freq, 8);
		const i16 offset = static_cast<i16>(freq & 0xff);
		if (sub(ind, 63) > 0)
			ind = 63;
		const i32 t = L_mult(slope_cos[ind], offset);
		lsp[i] = add(table2[ind], extract_l(L_shr(t, 13)));
	}
}

/* (see g729a_int.h) */
void lsp_lsf2(const i16 *lsp, i16 *lsf)
{
	i16 ind = 63;
	for (int i = M - 1; i >= 0; i--) {
		while (sub(table2[ind], lsp[i]) < 0) {
			ind = sub(ind, 1);
			if (ind <= 0)
				break;
		}
		const i16 offset = sub(lsp[i], table2[ind]);
		const i32 t = L_mult(slope_acos[ind], offset);
		const i16 freq = add(shl(ind, 9), extract_l(L_shr(t, 12)));
		lsf[i] = mult(freq, 25736);
	}
}

/* (see g729a_int.h) */
void lsp_lsf(const i16 *lsp, i16 *lsf)
{
	i16 ind = 63;
	for (int i = M - 1; i >= 0; i--) {
		while (sub(table[ind], lsp[i]) < 0)
			ind = sub(ind, 1);
		const i32 t = L_mult(sub(lsp[i], table[ind]), slope[ind]);
		lsf[i] = add(round16(L_shl(t, 3)), shl(ind, 8));
	}
}

/* (see g729a_int.h) */
i16 random16(i16 &seed)
{
	seed = extract_l(L_add(L_shr(L_mult(seed, 31821), 1), 13849));
	return seed;
}

/* (see g729a_int.h) */
void int_qlpc(const i16 *lsp_old, const i16 *lsp_new, i16 *az)
{
	i16 lsp[M];
	for (int i = 0; i < M; i++)
		lsp[i] = add(shr(lsp_new[i], 1), shr(lsp_old[i], 1));
	lsp_az(lsp, az);
	lsp_az(lsp_new, az + MP1);
}

/* (see g729a_int.h) */
void weight_az(const i16 *a, i16 gamma, i16 *ap)
{
	ap[0] = a[0];
	i16 fac = gamma;
	for (int i = 1; i < M; i++) {
		ap[i] = round16(L_mult(a[i], fac));
		fac = round16(L_mult(fac, gamma));
	}
	ap[M] = round16(L_mult(a[M], fac));
}

/* (see g729a_int.h) */
void residu(const i16 *a, const i16 *x, i16 *y, int n)
{
	for (int i = 0; i < n; i++) {
		i32 s = L_mult(x[i], a[0]);
		for (int j = 1; j <= M; j++)
			s = L_mac(s, a[j], x[i - j]);
		y[i] = round16(L_shl(s, 3));
	}
}

/* (see g729a_int.h) */
void syn_filt(const i16 *a, const i16 *x, i16 *y, int n, i16 *mem,
	      bool update)
{
	i16 tmp[M + 100];
	for (int i = 0; i < M; i++)
		tmp[i] = mem[i];
	i16 *yy = tmp + M;
	for (int i = 0; i < n; i++) {
		i32 s = L_mult(x[i], a[0]);
		for (int j = 1; j <= M; j++)
			s = L_msu(s, a[j], yy[i - j]);
		yy[i] = round16(L_shl(s, 3));
	}
	for (int i = 0; i < n; i++)
		y[i] = yy[i];
	if (update)
		for (int i = 0; i < M; i++)
			mem[i] = y[n - M + i];
}

/* (see g729a_int.h): the 1/3 resolution interpolation filter */
void pred_lt_3(i16 *exc, i16 t0, i16 frac, int n)
{
	const i16 *x0 = exc - t0;
	frac = negate(frac);
	if (frac < 0) {
		frac = add(frac, UP_SAMP);
		x0--;
	}
	const i16 *c1 = &inter_3l[frac];
	const i16 *c2 = &inter_3l[sub(UP_SAMP, frac)];
	for (int j = 0; j < n; j++) {
		const i16 *x1 = x0++;
		const i16 *x2 = x0;
		i32 s = 0;
		for (int i = 0, k = 0; i < L_INTER10; i++, k += UP_SAMP) {
			s = L_mac(s, x1[-i], c1[k]);
			s = L_mac(s, x2[i], c2[k]);
		}
		exc[j] = round16(s);
	}
}

/* (see g729a_int.h) */
i16 parity_pitch(i16 index)
{
	i16 temp = shr(index, 1);
	i16 sum = 1;
	for (int i = 0; i <= 5; i++) {
		temp = shr(temp, 1);
		sum = add(sum, static_cast<i16>(temp & 1));
	}
	return static_cast<i16>(sum & 1);
}

/* (see g729a_int.h) */
void LspPast::reset()
{
	static const i16 init[M] = { 2339, 4679, 7018, 9358, 11698, 14037,
				     16377, 18717, 21056, 23396 };
	for (auto &f : freq_prev)
		for (int j = 0; j < M; j++)
			f[j] = init[j];
}

/* (see g729a_int.h) */
void lsp_expand(i16 *buf, i16 gap, int lo, int hi)
{
	for (int j = lo; j < hi; j++) {
		const i16 diff = sub(buf[j - 1], buf[j]);
		const i16 tmp = shr(add(diff, gap), 1);
		if (tmp > 0) {
			buf[j - 1] = sub(buf[j - 1], tmp);
			buf[j] = add(buf[j], tmp);
		}
	}
}

/* (see g729a_int.h) */
void lsp_prev_extract(const i16 *lsp, i16 *lsp_ele, const i16 (*fgm)[M],
		      const i16 *fg_sum_inv_m, const LspPast &past)
{
	for (int j = 0; j < M; j++) {
		i32 t = L_deposit_h(lsp[j]);
		for (int k = 0; k < MA_NP; k++)
			t = L_msu(t, past.freq_prev[k][j], fgm[k][j]);
		const i16 tmp = extract_h(t);
		t = L_mult(tmp, fg_sum_inv_m[j]);
		lsp_ele[j] = extract_h(L_shl(t, 3));
	}
}

/* (see g729a_int.h) */
void lsp_prev_extract(const i16 *lsp, i16 *lsp_ele, i16 mode,
		      const LspPast &past)
{
	lsp_prev_extract(lsp, lsp_ele, fg[mode], fg_sum_inv[mode], past);
}

/* (see g729a_int.h) */
void lsp_prev_compose(const i16 *lsp_ele, i16 *lsp, const i16 (*fgm)[M],
		      const i16 *fg_sum_m, const LspPast &past)
{
	for (int j = 0; j < M; j++) {
		i32 t = L_mult(lsp_ele[j], fg_sum_m[j]);
		for (int k = 0; k < MA_NP; k++)
			t = L_mac(t, past.freq_prev[k][j], fgm[k][j]);
		lsp[j] = extract_h(t);
	}
}

/* (see g729a_int.h) */
void lsp_prev_update(const i16 *lsp_ele, LspPast &past)
{
	for (int k = MA_NP - 1; k > 0; k--)
		for (int j = 0; j < M; j++)
			past.freq_prev[k][j] = past.freq_prev[k - 1][j];
	for (int j = 0; j < M; j++)
		past.freq_prev[0][j] = lsp_ele[j];
}

/* (see g729a_int.h) */
void lsp_stability(i16 *buf)
{
	for (int j = 0; j < M - 1; j++)
		if (L_sub(buf[j + 1], buf[j]) < 0) {
			const i16 tmp = buf[j + 1];
			buf[j + 1] = buf[j];
			buf[j] = tmp;
		}
	if (sub(buf[0], L_LIMIT) < 0)
		buf[0] = L_LIMIT;
	for (int j = 0; j < M - 1; j++)
		if (L_sub(L_sub(buf[j + 1], buf[j]), GAP3) < 0)
			buf[j + 1] = add(buf[j], GAP3);
	if (sub(buf[M - 1], M_LIMIT) > 0)
		buf[M - 1] = M_LIMIT;
}

/* (see g729a_int.h) */
void lsp_get_quant(i16 code0, i16 code1, i16 code2, i16 mode,
		   LspPast &past, i16 *lspq)
{
	i16 buf[M];
	for (int j = 0; j < NC; j++)
		buf[j] = add(lspcb1[code0][j], lspcb2[code1][j]);
	for (int j = NC; j < M; j++)
		buf[j] = add(lspcb1[code0][j], lspcb2[code2][j]);
	lsp_expand(buf, GAP1, 1, M);
	lsp_expand(buf, GAP2, 1, M);
	lsp_prev_compose(buf, lspq, fg[mode], fg_sum[mode], past);
	lsp_prev_update(buf, past);
	lsp_stability(lspq);
}

/* (see g729a_int.h) */
void gain_predict(const i16 *past_qua_en, const i16 *code, i16 *gcode0,
		  i16 *exp_gcode0)
{
	i32 t = 0;
	for (int i = 0; i < L_SUBFR; i++)
		t = L_mac(t, code[i], code[i]);
	i16 exp, frac;
	Log2(t, &exp, &frac);
	t = Mpy_32_16(exp, frac, -24660);
	t = L_mac(t, 32588, 32);
	t = L_shl(t, 10);
	for (int i = 0; i < 4; i++)
		t = L_mac(t, pred[i], past_qua_en[i]);
	*gcode0 = extract_h(t);
	t = L_mult(*gcode0, 5439);
	t = L_shr(t, 8);
	L_Extract(t, &exp, &frac);
	*gcode0 = extract_l(Pow2(14, frac));
	*exp_gcode0 = sub(14, exp);
}

/* (see g729a_int.h) */
void gain_update(i16 *past_qua_en, i32 L_gbk12)
{
	for (int i = 3; i > 0; i--)
		past_qua_en[i] = past_qua_en[i - 1];
	i16 exp, frac;
	Log2(L_gbk12, &exp, &frac);
	const i32 t = L_Comp(sub(exp, 13), frac);
	const i16 tmp = extract_h(L_shl(t, 13));
	past_qua_en[0] = mult(tmp, 24660);
}

/* (see g729a_int.h) */
void gain_update_erasure(i16 *past_qua_en)
{
	i32 t = 0;
	for (int i = 0; i < 4; i++)
		t = L_add(t, L_deposit_l(past_qua_en[i]));
	i16 av = extract_l(L_shr(t, 2));
	av = sub(av, 4096);
	if (sub(av, -14336) < 0)
		av = -14336;
	for (int i = 3; i > 0; i--)
		past_qua_en[i] = past_qua_en[i - 1];
	past_qua_en[0] = av;
}

/* each parameter's bits (Table 8/G.729) */
static const int BITS[PRMS] = { 8, 10, 8, 1, 13, 4, 7, 5, 13, 4, 7 };

/* (see g729a.h) */
void pack(const int16_t prm[PRMS], uint8_t out[FRAME_OCTETS])
{
	for (int i = 0; i < FRAME_OCTETS; i++)
		out[i] = 0;
	int pos = 0;
	for (int i = 0; i < PRMS; i++)
		for (int b = BITS[i] - 1; b >= 0; b--, pos++)
			if (prm[i] >> b & 1)
				out[pos >> 3] |= static_cast<uint8_t>(0x80 >> (pos & 7));
}

/* (see g729a.h) */
void unpack(const uint8_t in[FRAME_OCTETS], int16_t prm[PRMS])
{
	int pos = 0;
	for (int i = 0; i < PRMS; i++) {
		int v = 0;
		for (int b = 0; b < BITS[i]; b++, pos++)
			v = v << 1 | (in[pos >> 3] >> (7 - (pos & 7)) & 1);
		prm[i] = static_cast<int16_t>(v);
	}
}

/* each SID parameter's bits (Table B.2/G.729) */
static const int SID_BITS[SID_PRMS] = { 1, 5, 4, 5 };

/* (see g729a.h) */
void pack_sid(const int16_t prm[SID_PRMS], uint8_t out[SID_OCTETS])
{
	unsigned v = 0;
	for (int i = 0; i < SID_PRMS; i++)
		v = v << SID_BITS[i] |
			(static_cast<unsigned>(prm[i]) & ((1u << SID_BITS[i]) - 1));
	v <<= 1;
	out[0] = static_cast<uint8_t>(v >> 8);
	out[1] = static_cast<uint8_t>(v);
}

/* (see g729a.h) */
void unpack_sid(const uint8_t in[SID_OCTETS], int16_t prm[SID_PRMS])
{
	unsigned v = (static_cast<unsigned>(in[0]) << 8 | in[1]) >> 1;
	for (int i = SID_PRMS - 1; i >= 0; i--) {
		prm[i] = static_cast<int16_t>(v & ((1u << SID_BITS[i]) - 1));
		v >>= SID_BITS[i];
	}
}

} // namespace g729
} // namespace dlink
