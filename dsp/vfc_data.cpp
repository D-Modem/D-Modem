/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    vfc_data.cpp - V.FC's data mode: its frames, mapper and 2D trellis
 *                 codes, on V.34's symbol rates and shell mapping
 *
 *    Rockwell's V.FC was never published.  This is the Courier
 *    V.Everything's (IDSDL302, overlay 7 of its DSP), read out of the
 *    firmware and checked point for point against it at every rate
 *    (docs/dsp/vfc-details.md):
 *    - 8 symbol frames of b bits (b = ceil(8R/S)); where 8R/S is not a
 *      whole number some frames are one bit short, in cycles of 5 (7 at
 *      2800 baud) frames, the long ones first, the very first frame
 *      short
 *    - per symbol: 2 bits that turn the quadrant z differentially, q
 *      uncoded bits, then 2 or 3 bits for the shell mapper, whose 16 + k
 *      bits pick the 8 symbols' rings as V.34's does (9.3/V.34)
 *    - a symbol's point: index (ring << q | uncoded bits), one of two
 *      quarter-plane tables and a rotation, by the subset that the
 *      encoder state's low bit and z give
 *    - V.32's 8 state code or a 32 state one, a step every symbol
 *
 */

#include <cmath>
#include <cstring>
#include <mutex>

#include "dm_types.h"
#include "bits.h"
#include "v34.h"

namespace dsp {

/* the quarter-plane points (Courier resident 0xaecf): index i's two
   subsets' points at 2i and 2i + 1, on the lattice of x + y odd */
static const signed char vfc_points[256][2] = {
	{ 0, 1 }, { -2, 1 }, { 0, -3 }, { 2, 1 }, { -4, 1 },
	{ -2, -3 }, { 4, 1 }, { 2, -3 }, { -4, -3 }, { -2, 5 },
	{ 4, -3 }, { 2, 5 }, { 0, 5 }, { -6, 1 }, { -4, 5 },
	{ 6, 1 }, { 4, 5 }, { -6, -3 }, { 0, -7 }, { 6, -3 },
	{ -8, 1 }, { -2, -7 }, { -4, -7 }, { 2, -7 }, { 4, -7 },
	{ -6, 5 }, { 8, 1 }, { 6, 5 }, { -8, -3 }, { 2, 9 },
	{ 8, -3 }, { -2, 9 }, { 0, 9 }, { -6, -7 }, { -8, 5 },
	{ 6, -7 }, { 8, 5 }, { -10, 1 }, { -4, 9 }, { 10, 1 },
	{ 4, 9 }, { -10, -3 }, { -8, -7 }, { 10, -3 }, { 8, -7 },
	{ -6, 9 }, { 0, -11 }, { 6, 9 }, { -4, -11 }, { -10, 5 },
	{ 4, -11 }, { -2, -11 }, { -12, 1 }, { 2, -11 }, { -8, 9 },
	{ 10, 5 }, { 8, 9 }, { -10, -7 }, { 12, 1 }, { 10, -7 },
	{ -12, -3 }, { -6, -11 }, { 12, -3 }, { 6, -11 }, { -12, 5 },
	{ -2, 13 }, { 0, 13 }, { 2, 13 }, { 12, 5 }, { -10, 9 },
	{ -8, -11 }, { 10, 9 }, { -4, 13 }, { -14, 1 }, { 4, 13 },
	{ 14, 1 }, { 8, -11 }, { -14, -3 }, { -12, -7 }, { -6, 13 },
	{ 12, -7 }, { 6, 13 }, { -12, 9 }, { 14, -3 }, { 0, -15 },
	{ -14, 5 }, { 12, 9 }, { -10, -11 }, { -8, 13 }, { 10, -11 },
	{ 8, 13 }, { 14, 5 }, { -4, -15 }, { -2, -15 }, { 4, -15 },
	{ 2, -15 }, { -16, 1 }, { -14, -7 }, { 16, 1 }, { 14, -7 },
	{ -16, -3 }, { -6, -15 }, { -12, -11 }, { 6, -15 }, { 12, -11 },
	{ -10, 13 }, { 16, -3 }, { 10, 13 }, { -16, 5 }, { -14, 9 },
	{ 16, 5 }, { 14, 9 }, { -8, -15 }, { -2, 17 }, { 0, 17 },
	{ 2, 17 }, { 8, -15 }, { -14, -11 }, { -16, -7 }, { 14, -11 },
	{ -4, 17 }, { 18, 1 }, { 4, 17 }, { -18, 1 }, { 16, -7 },
	{ -6, 17 }, { 12, 13 }, { 6, 17 }, { -12, 13 }, { 10, -15 },
	{ -16, 9 }, { -10, -15 }, { 16, 9 }, { -18, -3 }, { -8, 17 },
	{ 18, -3 }, { 8, 17 }, { -18, 5 }, { 0, -19 }, { 18, 5 },
	{ -12, -15 }, { -14, 13 }, { 12, -15 }, { -2, -19 }, { -16, -11 },
	{ 2, -19 }, { -4, -19 }, { 14, 13 }, { 4, -19 }, { -18, -7 },
	{ 16, -11 }, { 18, -7 }, { -20, 1 }, { -10, 17 }, { 20, 1 },
	{ 10, 17 }, { -20, -3 }, { -6, -19 }, { 20, -3 }, { 6, -19 },
	{ -20, 5 }, { -18, 9 }, { -16, 13 }, { 18, 9 }, { -8, -19 },
	{ -14, -15 }, { 8, -19 }, { 14, -15 }, { 16, 13 }, { -18, -11 },
	{ 20, 5 }, { -2, 21 }, { -12, 17 }, { 2, 21 }, { 12, 17 },
	{ 18, -11 }, { 0, 21 }, { -10, -19 }, { -20, -7 }, { 10, -19 },
	{ 20, -7 }, { -6, 21 }, { -4, 21 }, { 6, 21 }, { 4, 21 },
	{ -22, 1 }, { -20, 9 }, { -14, 17 }, { -16, -15 }, { 14, 17 },
	{ 16, -15 }, { 22, 1 }, { 20, 9 }, { -22, -3 }, { -12, -19 },
	{ -18, 13 }, { -8, 21 }, { 18, 13 }, { 8, 21 }, { 22, -3 },
	{ 12, -19 }, { -22, 5 }, { -20, -11 }, { 22, 5 }, { 20, -11 },
	{ -22, -7 }, { 0, -23 }, { -2, -23 }, { -16, 17 }, { 2, -23 },
	{ -4, -23 }, { 22, -7 }, { 4, -23 }, { -10, 21 }, { 16, 17 },
	{ 10, 21 }, { -20, 13 }, { -18, -15 }, { 20, 13 }, { 18, -15 },
	{ -24, 1 }, { -14, -19 }, { 24, 1 }, { 14, -19 }, { -24, -3 },
	{ -22, 9 }, { -12, 21 }, { -6, -23 }, { 12, 21 }, { 6, -23 },
	{ 24, -3 }, { 22, 9 }, { -8, -23 }, { -22, -11 }, { 8, -23 },
	{ 22, -11 }, { -24, 5 }, { -18, 17 }, { 24, 5 }, { 18, 17 },
	{ -16, -19 }, { -10, -23 }, { 16, -19 }, { -2, 25 }, { -24, -7 },
	{ 2, 25 }, { -20, -15 }, { 10, -23 }, { 0, 25 }, { -14, 21 },
	{ 20, -15 }, { 14, 21 }, { 24, -7 }, { -22, 13 }, { -4, 25 },
	{ 22, 13 },
};

/* the subset c by the encoder state's low bit and z (Courier 0xcb2e):
   c >> 2 the table, c & 3 the quarter turns */
static const unsigned char vfc_label[8] = { 0, 5, 6, 1, 2, 7, 4, 3 };

/* the encoder's taps, one per bit of state | z << 5 (Courier 0xcb5f,
   V.32's 8 state code, and 0xcb58, the 32 state one) */
static const u16 vfc_taps8[7] = {
	0x0064, 0x0101, 0x0002, 0x0000, 0x0000, 0x0082, 0x0103
};
static const u16 vfc_taps32[7] = {
	0x0070, 0x0101, 0x0002, 0x0004, 0x000a, 0x008a, 0x0103
};

/* the shell mapping bits of each symbol of a frame, 2 bits a symbol
   from the low ones, by k (Courier 0xcb66) */
static const u16 vfc_shell_bits[8] = {
	0xaaaa, 0xaaab, 0xaaaf, 0xaabf, 0xaaff, 0xabff, 0xafff, 0xbfff
};

/* the most V.FC sends at each symbol rate, in 2400 bit/s (above it a
   frame's points leave the table) */
static const s16 vfc_max[V34_NBAUDS] = { 8, 10, 10, 11, 12, 12 };

/* (see v34.h) */
s16 vfc_max_n(s16 baud)
{
	return baud >= 0 && baud < V34_NBAUDS ? vfc_max[baud] : 0;
}

/* (see v34.h) */
s16 vfc_subset(s16 st, s16 z)
{
	return vfc_label[(st & 1) + 2 * (z & 3)];
}

/* (see v34.h) */
s16 vfc_next(s16 states, s16 st, s16 z)
{
	const u16 *tap = states == 32 ? vfc_taps32 : vfc_taps8;
	s16 u = static_cast<s16>((st & 0x1f) | (z & 3) << 5), i, t;
	u16 b = 0;
	for (i = 0; i < 7; i++)
		if ((u >> i) & 1)
			b ^= tap[i];
	t = static_cast<s16>(b >> 5);
	return static_cast<s16>((b ^ ((t >> 2) & t & 3)) & 0x1f);
}

/* clockwise by k quarter turns, as the Courier turns its points */
static void vfc_rotate(s16 *x, s16 *y, s16 k)
{
	s16 t;
	for (k &= 3; k; k--) {
		t = *x;
		*x = *y;
		*y = -t;
	}
}

/* the point of subset c and index i */
static void vfc_point(s16 c, s16 i, s16 *x, s16 *y)
{
	*x = vfc_points[2 * i + (c >> 2)][0];
	*y = vfc_points[2 * i + (c >> 2)][1];
	vfc_rotate(x, y, c & 3);
}

/* index << 3 | subset of each point (x, y), |x|, |y| <= VFC_EDGE */
#define VFC_EDGE        26
static s16 vfc_lookup[2 * VFC_EDGE + 1][2 * VFC_EDGE + 1];

/* vfc_lookup[] from the table and its turns */
static void vfc_make_lookup(void)
{
	s16 i, c, x, y;
	for (auto &row : vfc_lookup)
		std::fill(std::begin(row), std::end(row), -1);
	for (i = 0; i < 128; i++)
		for (c = 0; c < 8; c++) {
			vfc_point(c, i, &x, &y);
			vfc_lookup[x + VFC_EDGE][y + VFC_EDGE] =
				static_cast<s16>(i << 3 | c);
		}
}

/* (see v34.h) */
s16 V34Params::vfc_init(s16 baud, s16 n, s16 states_)
{
	s16 num, den, longs, cyc, m, i;
	double ringp[V34_MAX_RINGS], pw;

	reinit(*this);
	if (baud < 0 || baud >= V34_NBAUDS || n < 6 || n > vfc_max[baud])
		return -1;
	/* 8R/S = 8 n a / c bits a frame (S = 2400 a / c) */
	num = static_cast<s16>(8 * n * v34_bauds[baud].c);
	den = v34_bauds[baud].a;
	for (i = den; i > 1; i--)
		if (num % i == 0 && den % i == 0) {
			num = static_cast<s16>(num / i);
			den = static_cast<s16>(den / i);
			break;
		}
	b = static_cast<s16>((num + den - 1) / den);
	q = static_cast<s16>((b - 32) / 8);
	vfc_k = static_cast<s16>(b - 32 - 8 * q);
	K = static_cast<s16>(16 + vfc_k);
	for (M = 4; pow(M, 8) < ldexp(1, K); M++)
		;
	/* the short frames: num / den = b - shorts / cyc */
	vfc_pat = 0;
	if (num % den) {
		cyc = den;
		longs = static_cast<s16>(cyc - (b * den - num));
		vfc_pat = static_cast<s16>(((1 << (cyc - longs)) - 1) << longs);
	}
	rate = 2400 * n;
	vfc = 1;
	states = states_ == 32 ? 32 : 8;
	theta = 0;
	nl_gain = 1;
	if (shell_tables())
		return -1;
	for (m = 0; m < M; m++) {
		double s = 0;
		for (i = 0; i < 2 << q; i++) {
			const signed char *v = vfc_points[(m << (q + 1)) + i];
			s += v[0] * v[0] + v[1] * v[1];
		}
		ringp[m] = s / (2 << q);
	}
	pw = shell_power(ringp, K);
	if (vfc_pat) {
		s16 sh = 0, len = 0;
		for (i = vfc_pat; i; i >>= 1, len++)
			sh += i & 1;
		pw = ((len - sh) * pw + sh * shell_power(ringp, K - 1)) / len;
	}
	/* the same power as the 4 point training signals */
	g = static_cast<float>(sqrt(2 / pw));
	pw_ = pw;
	return 0;
}

/* the 8 symbols of the next frame into sym[] */
void V34DataTx::vfc_frame(u32 *scr, s16 tap)
{
	const V34Params *p_ = &p;
	unsigned char bits[V34_MAX_B];
	s16 bb = p_->b, n = 0, i, j, k, m[8], by[8], sb[8];
	u16 pat = vfc_shell_bits[p_->vfc_k];
	u32 r0 = 0;

	/* the pattern from its low bit, the first frame short */
	if (vfc_sh & 1) {
		bb--;
		pat = static_cast<u16>(pat - 0x4000);
	}
	vfc_sh >>= 1;
	if (!vfc_sh)
		vfc_sh = p_->vfc_pat;
	link.get(bits, bb);
	for (i = 0; i < bb; i++)
		bits[i] = static_cast<unsigned char>(
			scramble(scr, tap, bits[i]));
	for (j = 0, k = 0; j < 8; j++) {
		by[j] = 0;
		for (i = 0; i < p_->q + 2; i++)
			by[j] |= static_cast<s16>(bits[n++] << i);
		sb[j] = pat & 3;
		pat >>= 2;
		for (i = 0; i < sb[j]; i++)
			r0 |= static_cast<u32>(bits[n++]) << k++;
	}
	p_->shell_map(r0, m);
	for (j = 0; j < 8; j++) {
		s16 c, x, y, idx;
		z = (z + by[j]) & 3;
		idx = static_cast<s16>(((by[j] >> 2) & ((1 << p_->q) - 1)) |
				       m[j] << p_->q);
		c = vfc_subset(state, z);
		vfc_point(c, idx, &x, &y);
		state = vfc_next(p_->states, state, z);
		sym[j] = cplx_make(p_->g * x, p_->g * y);
	}
}

/* the nearest point of each subset to (u, v) among those in use */
void V34Params::vfc_subsets(float u, float v, float dist[8], s16 pt[8]) const
{
	s16 i, lim = M << q, edge = VFC_EDGE - 4;
	s32 x0 = static_cast<s32>(lrintf(u)), y0 = static_cast<s32>(lrintf(v)),
	    dx, dy;

	if (x0 > edge) x0 = edge;
	if (x0 < -edge) x0 = -edge;
	if (y0 > edge) y0 = edge;
	if (y0 < -edge) y0 = -edge;
	for (i = 0; i < 8; i++) {
		dist[i] = 1e30f;
		pt[i] = -1;
	}
	for (dx = -4; dx <= 4; dx++)
		for (dy = -4; dy <= 4; dy++) {
			s32 x = x0 + dx, y = y0 + dy;
			s16 e = vfc_lookup[x + VFC_EDGE][y + VFC_EDGE];
			float dd;
			if (e < 0 || (e >> 3) >= lim)
				continue;
			dd = (u - x) * (u - x) + (v - y) * (v - y);
			if (dd < dist[e & 7]) {
				dist[e & 7] = dd;
				pt[e & 7] = e;
			}
		}
}

/* the bits of the frame of the 8 decided symbols in e[]: the shell
   demapping, the differential decoding of z, descrambling, out to link */
void V34DataRx::vfc_decode(u32 *scr, s16 tap)
{
	const V34Params *p_ = &p;
	unsigned char bits[V34_MAX_B];
	s16 n = 0, i, j, k = 0, m[8];
	u16 pat = vfc_shell_bits[p_->vfc_k];
	u32 r0;

	if (vfc_sh & 1)
		pat = static_cast<u16>(pat - 0x4000);
	vfc_sh >>= 1;
	if (!vfc_sh)
		vfc_sh = p_->vfc_pat;
	for (j = 0; j < 8; j++)
		m[j] = (e[j] & 511) >> p_->q;
	r0 = p_->shell_demap(m);
	for (j = 0; j < 8; j++) {
		s16 zz = e[j] >> 9, by = (zz - z) & 3, sb = pat & 3;
		z = zz;
		by = static_cast<s16>(by | (e[j] & ((1 << p_->q) - 1)) << 2);
		pat >>= 2;
		for (i = 0; i < p_->q + 2; i++)
			bits[n++] = (by >> i) & 1;
		for (i = 0; i < sb; i++)
			bits[n++] = (r0 >> k++) & 1;
	}
	for (i = 0; i < n; i++)
		bits[i] = static_cast<unsigned char>(
			descramble(scr, tap, bits[i]));
	link.put(bits, n);
}

/* z: the equalized symbol, in units of the training points; returns the
   tentative decision for the adaptation */
cplx V34DataRx::vfc_symbol(cplx z_, u32 *scr, s16 tap)
{
	const V34Params *p_ = &p;
	float u = z_.re / p_->g, v = z_.im / p_->g, d[8], best = 1e30f;
	s16 pt[8], i, x = 1, y = 0, out;

	{
		static std::once_flag once;
		std::call_once(once, vfc_make_lookup);
	}
	p_->vfc_subsets(u, v, d, pt);
	for (i = 0; i < 8; i++)
		if (pt[i] >= 0 && d[i] < best) {
			best = d[i];
			vfc_point(i, pt[i] >> 3, &x, &y);
		}
	if (vvit.step(p_->states, d, pt, &out)) {
		e[pos++] = out;
		if (pos == 8) {
			vfc_decode(scr, tap);
			pos = 0;
		}
	}
	return cplx_make(p_->g * x, p_->g * y);
}

} // namespace dsp
