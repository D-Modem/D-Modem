/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v34_data.cpp - V.34 data mode (section 9/V.34): framing, shell
 *                 mapping, differential encoding, the 16, 32 and 64
 *                 state 4D trellis codes and superframe
 *                 synchronisation
 *
 *    With precoding when the remote asks for it (9.6.2/V.34), without
 *    non-linear encoding.  As the original library does:
 *    - the trellis output Y0 of a 4D symbol is the low bit of the
 *      encoder state before that symbol's labels are shifted in, with
 *      the state recursions of Figures 10 to 12/V.34 (for the 32 and
 *      64 state codes the original's Convolve32/Convolve64 tables give
 *      the same encoder inputs as Table 13/V.34)
 *    - the superframe inversion pattern starts at its bit 2J - 2 with
 *      the first 4D symbol of the data signal
 *
 *    The receiver decodes the trellis code with a Viterbi decoder.
 *
 */

#include <cmath>
#include <cstring>
#include <mutex>

#include "bits.h"
#include "v34.h"

namespace dsp {

/* 0x5FEE, least significant bit first: the superframe inversions */
static const unsigned char v34_sync_pattern[16] = {
	0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0
};

/* 2D subset labels by ((x - 1) & 6) / 2 + 2 * ((y - 1) & 6) */
static const unsigned char v34_subset[16] = {
	0, 7, 4, 3, 5, 2, 1, 6, 4, 3, 0, 7, 1, 6, 5, 2
};

/* the encoder inputs from the subset labels of a 4D symbol:
   Y1 | Y2 << 1 | Y3 << 2 | Y4 << 3 */
static int conv_in(int s0, int s1)
{
	static const unsigned char row[4][8] = {
		{ 0, 0, 1, 1, 0, 0, 1, 1 },
		{ 3, 2, 2, 3, 3, 2, 2, 3 },
		{ 1, 1, 0, 0, 1, 1, 0, 0 },
		{ 2, 3, 3, 2, 2, 3, 3, 2 },
	};
	return row[s0 & 3][s1] | (s0 & 2) << 1 | ((s0 ^ s1) & 4) << 1;
}

/* the next encoder state; its low bit is Y0 */
static int trellis_next(int states, int st, int y)
{
	int y0 = st & 1, y1 = y & 1, y2 = (y >> 1) & 1;
	int y3 = (y >> 2) & 1, y4 = (y >> 3) & 1;
	int r[6], i;

	switch (states) {
	case 16:        /* Figure 10 */
		return (st >> 1) ^ (y1 | y2 << 1 | y2 << 2) ^
		       (y0 << 2) ^ (y0 << 3);
	case 32:        /* Figure 11 */
		return (st >> 1) ^ (y2 | y4 << 1 | y1 << 2 | y2 << 3 |
				    y0 << 4);
	}
	/* Figure 12 */
	for (i = 0; i < 6; i++)
		r[i] = (st >> i) & 1;
	return (r[1] ^ r[3] ^ y2) |
	       r[0] << 1 |
	       r[3] << 2 |
	       (r[3] ^ y1 ^ r[4]) << 3 |
	       (r[2] ^ y3 ^ (r[3] & y2) ^ r[4] ^ r[5]) << 4 |
	       (y4 ^ r[4] ^ r[5] ^ (r[3] & (y1 ^ r[4]))) << 5;
}

/* the 2D subset label (0..7) of the point (x, y), odd coordinates */
static int subset_label(int x, int y)
{
	return v34_subset[((x - 1) & 6) / 2 + 2 * ((y - 1) & 6)];
}

/* for V.92's trellis code (6.4.4/V.92): the same converter and encoders */
int v34_conv_in(int s0, int s1) { return conv_in(s0, s1); }
/* (see v34.h) */
int v34_trellis_next(int states, int st, int y)
{
	return trellis_next(states, st, y);
}
/* (see v34.h) */
int v34_subset_label(int x, int y) { return subset_label(x, y); }

/* clockwise by k quarter turns */
static void rotate(int *x, int *y, int k)
{
	int t;
	for (k &= 3; k; k--) {
		t = *x;
		*x = *y;
		*y = -t;
	}
}


/*
 *    parameters and shell mapping (9.2, 9.4/V.34)
 */

/* the shell mapping tables of 9.4/V.34 for M rings: g2 and g4, the
   numbers of ring pairs and quadruples with each sum, and z8, the number
   of ring 8-tuples with a sum below each value; -1 when M is out of
   range */
int V34Params::shell_tables()
{
	int M_ = M, i, j;
	if (M_ < 1 || 2 * M_ - 1 > V34_G2_LEN || 8 * M_ - 6 > V34_Z8_LEN)
		return -1;
	zero(g2);
	zero(g4);
	for (i = 0; i < M_; i++)
		for (j = 0; j < M_; j++)
			g2[i + j]++;
	for (i = 0; i < 2 * M_ - 1; i++)
		for (j = 0; j < 2 * M_ - 1; j++)
			g4[i + j] += g2[i] * g2[j];
	z8[0] = 0;
	for (i = 0; i < 8 * M_ - 7; i++) {
		uint64_t g8 = 0;
		for (j = 0; j <= i; j++)
			if (j < 4 * M_ - 3 && i - j < 4 * M_ - 3)
				g8 += g4[j] * g4[i - j];
		z8[i + 1] = z8[i] + g8;
	}
	return 0;
}

/* g2(i), 0 outside the table */
uint64_t V34Params::g2p(int i) const
{
	return i >= 0 && i < 2 * M - 1 ? g2[i] : 0;
}

/* g4(i), 0 outside the table */
uint64_t V34Params::g4p(int i) const
{
	return i >= 0 && i < 4 * M - 3 ? g4[i] : 0;
}

/* the mean power of the points of a frame with k shell mapping bits;
   with theta, of the points through the non-linear encoder (9.7/V.34)
   for points of mean power e0 */
double V34Params::frame_power(int k, double theta, double e0) const
{
	int M_ = M, a, m, i;
	double ringp[V34_MAX_RINGS], left = ldexp(1, k), e = 0;
	double g7[8 * V34_MAX_RINGS];

	for (m = 0; m < M_; m++) {
		double s = 0;
		for (i = 0; i < 1 << q; i++) {
			const signed char *v = v34_quarter[(m << q) + i];
			double e = v[0] * v[0] + v[1] * v[1], z;
			z = theta * e / e0;
			z = 1 + z / 6 + z * z / 120;
			s += e * z * z;
		}
		ringp[m] = s / (1 << q);
	}
	if (k == 0)
		return ringp[0];
	/* 7-tuples of rings by their sum */
	for (a = 0; a <= 7 * (M_ - 1); a++) {
		g7[a] = 0;
		for (i = 0; i <= a; i++) {
			double g3 = 0;
			for (m = 0; m < M_ && m <= a - i; m++)
				g3 += static_cast<double>(g2p(a - i - m));
			g7[a] += static_cast<double>(g4p(i)) * g3;
		}
	}
	for (a = 0; a <= 8 * (M_ - 1) && left > 0; a++) {
		double g8 = static_cast<double>(z8[a + 1] - z8[a]), take,
		       ea = 0;
		take = g8 < left ? g8 : left;
		for (m = 0; m < M_ && m <= a; m++)
			if (a - m <= 7 * (M_ - 1))
				ea += g7[a - m] * ringp[m];
		e += take * ea / g8;
		left -= take;
	}
	return e / ldexp(1, k);
}

/* the data mode parameters at symbol rate baud (V34_BAUD_*) and n *
   2400 bit/s, minimum or expanded shaping (Table 10/V.34): frame
   sizes, the switching pattern, the point spacing at the training
   power; 16 states, no precoding, no non-linear encoding.  Returns -1
   when the rate is not allowed at that symbol rate. */
int V34Params::init(int baud, int n, int expanded)
{
	const struct v34_rate *rt;
	double pw;

	reinit(*this);
	if (baud < 0 || baud >= V34_NBAUDS || n < 1 || n > 14)
		return -1;
	rt = &v34_rates[baud][n - 1];
	if (!rt->b)
		return -1;
	rate = 2400 * n;
	states = 16;
	theta = 0;
	nl_gain = 1;
	b = rt->b;
	M = expanded ? rt->m_exp : rt->m_min;
	J = v34_bauds[baud].J;
	P = v34_bauds[baud].P;
	if (b > 12) {
		K = b - 12;
		while (K >= 32) {
			K -= 8;
			q++;
		}
	}
	/* high mapping frames per data frame (8.2/V.34) */
	r = rate * 28 / (J * 100) - (b - 1) * P;
	if (shell_tables() || (M << q) > 416)
		return -1;
	pw = frame_power(K);
	if (b > 12)
		pw = (r * pw + (P - r) * frame_power(K - 1)) / P;
	/* the same power as the 4 point training signals */
	g = static_cast<float>(sqrt(2 / pw));
	pw_ = pw;
	w = b < 56 ? 1 : 2;
	return 0;
}

/* 9.7/V.34 with theta = 0.3125: the transmitter keeps its power (the
   note in 10.1.3), the receiver scales back before decoding */
void V34Params::set_nonlinear(double theta_)
{
	double pw;

	theta = static_cast<float>(theta_);
	nl_gain = 1;
	if (theta <= 0)
		return;
	pw = frame_power(K, theta, pw_);
	if (b > 12)
		pw = (r * pw + (P - r) * frame_power(K - 1, theta, pw_)) / P;
	nl_gain = static_cast<float>(sqrt(pw_ / pw));
}

/* precoding with the coefficients h_ of an MP sequence (14 fractional
   bits; all zero: none), the point spacing g made smaller for the same
   power */
void V34Params::set_precoder(const int h_[6])
{
	double pw = 2 / (g * g);
	int i;

	precode = 0;
	for (i = 0; i < 6; i++) {
		h[i] = h_[i];
		if (h_[i])
			precode = 1;
	}
	/* x(n) = u(n) + c(n) - p(n): u(n) plus a component uniform over
	   a 2^w square; the same power as without precoding */
	if (precode)
		g = static_cast<float>(sqrt(2 / (pw + (1 << 2 * w) / 6.0)));
}

/* to the nearest multiple of 2^s; half way: to the smaller magnitude */
static int32_t round_shift(int64_t v, int s)
{
	return static_cast<int32_t>((v + (1LL << (s - 1)) - 1 + (v < 0)) >> s);
}

/* x(n) = y(n) - p(n) goes in: p(n + 1) and c(n + 1) */
void V34Params::precoder_push(V34Precoder *pc, int yr, int yi) const
{
	int64_t qr = 0, qi = 0;
	int k;

	for (k = 2; k > 0; k--) {
		pc->x[k][0] = pc->x[k - 1][0];
		pc->x[k][1] = pc->x[k - 1][1];
	}
	pc->x[0][0] = static_cast<int32_t>(yr) * 128 - pc->p[0];
	pc->x[0][1] = static_cast<int32_t>(yi) * 128 - pc->p[1];
	for (k = 0; k < 3; k++) {
		int64_t xr = pc->x[k][0], xi = pc->x[k][1];
		int hr = h[2 * k], hi = h[2 * k + 1];
		qr += xr * hr - xi * hi;
		qi += xr * hi + xi * hr;
	}
	pc->p[0] = round_shift(qr, 14);
	pc->p[1] = round_shift(qi, 14);
	pc->c[0] = round_shift(pc->p[0], 7 + w) << w;
	pc->c[1] = round_shift(pc->p[1], 7 + w) << w;
}

/* the bit of the modulo encoder (9.6.3.3) for c(n) */
int V34Precoder::c_parity() const
{
	return ((c[0] + c[1]) >> 1) & 1;
}

/* the shell mapper (9.4/V.34): the rings m[0..7] of a mapping frame
   from the value r0 of its shell mapping bits */
void V34Params::shell_map(uint32_t r0, int m[8]) const
{
	uint64_t r1, r2, r3, r4, r5, t;
	int a, b_, c, d, e, f, g_, h_, M_ = M;

	for (a = 1; a < 8 * M_ - 6 && z8[a] <= r0; a++)
		;
	a--;
	r1 = r0 - z8[a];
	for (b_ = 0; ; b_++) {
		t = g4p(b_) * g4p(a - b_);
		if (r1 < t)
			break;
		r1 -= t;
	}
	r2 = r1 % g4p(b_);
	r3 = r1 / g4p(b_);
	for (c = 0; ; c++) {
		t = g2p(c) * g2p(b_ - c);
		if (r2 < t)
			break;
		r2 -= t;
	}
	r4 = r2;
	for (d = 0; ; d++) {
		t = g2p(d) * g2p(a - b_ - d);
		if (r3 < t)
			break;
		r3 -= t;
	}
	r5 = r3;
	e = static_cast<int>(r4 % g2p(c));
	f = static_cast<int>(r4 / g2p(c));
	g_ = static_cast<int>(r5 % g2p(d));
	h_ = static_cast<int>(r5 / g2p(d));
/* m[i], m[i + 1]: the pair of rings with index v among those with that
   sum */
#define PAIR(i, sum, v) do { \
		if ((sum) < M_) { m[i] = (v); m[(i) + 1] = (sum) - (v); } \
		else { m[(i) + 1] = M_ - 1 - (v); m[i] = (sum) - m[(i) + 1]; } \
	} while (0)
	PAIR(0, c, e);
	PAIR(2, b_ - c, f);
	PAIR(4, d, g_);
	PAIR(6, a - b_ - d, h_);
#undef PAIR
}

/* the index of the ring pair (x, y) among those with its sum, as
   shell_map() numbers them */
int V34Params::pair_index(int x, int y) const
{
	return x + y < M ? x : M - 1 - y;
}

/* the inverse of shell_map(): r0 from the rings m[0..7] */
uint32_t V34Params::shell_demap(const int m[8]) const
{
	int a = 0, b_, c, d, i;
	uint64_t r_;

	for (i = 0; i < 8; i++)
		a += m[i];
	b_ = m[0] + m[1] + m[2] + m[3];
	c = m[0] + m[1];
	d = m[4] + m[5];
	r_ = z8[a];
	for (i = 0; i < b_; i++)
		r_ += g4p(i) * g4p(a - i);
	/* r1 = r2 + r3 g4(b) */
	{
		uint64_t r2 = 0, r3 = 0;
		for (i = 0; i < c; i++)
			r2 += g2p(i) * g2p(b_ - i);
		r2 += pair_index(m[0], m[1]) +
		      static_cast<uint64_t>(pair_index(m[2], m[3])) *
			      g2p(c);
		for (i = 0; i < d; i++)
			r3 += g2p(i) * g2p(a - b_ - i);
		r3 += pair_index(m[4], m[5]) +
		      static_cast<uint64_t>(pair_index(m[6], m[7])) *
			      g2p(d);
		r_ += r2 + r3 * g4p(b_);
	}
	return static_cast<uint32_t>(r_);
}


/*
 *    transmitter
 */

/* the data transmitter with the parameters p_: differential, trellis
   and precoder states from zero, the superframe pattern from its bit
   2J - 2, a new mapping frame at the first symbol */
void V34DataTx::init(const V34Params *p_)
{
	reinit(*this);
	p = *p_;
	pat = 2 * p_->J - 2;
	v0 = v34_sync_pattern[pat];
	pos = 8;
}

/* 9.7/V.34: x'(n) = (1 + z/6 + z^2/120) x(n), z = theta |x(n)|^2 over the
   average energy of x(n); the points are g x(n) and g^2 times that
   average is 2 */
static cplx nonlinear(cplx s, float theta)
{
	float z = theta * (s.re * s.re + s.im * s.im) / 2;
	return cplx_scale(s, 1 + z / 6 + z * z / 120);
}

/* the 8 symbols of the next mapping frame into sym[]: high or low by
   the switching pattern, its bits from link scrambled (scr, tap:
   the primary channel's scrambler), shell mapped, coded differentially
   and by the trellis code, precoded and through the non-linear encoder
   as set */
void V34DataTx::encode_frame(uint32_t *scr, int tap)
{
	const V34Params *p_ = &p;
	unsigned char bits[V34_MAX_B];
	int bb = p_->b, kk = p_->K, n = 0, i, j, m[8], ib[4], qb[8];
	uint32_t r0 = 0;

	sbc += p_->r;
	if (sbc >= p_->P)
		sbc -= p_->P;
	else if (bb > 12) {
		bb--;
		kk--;
	}
	link.get(bits, bb);
	for (i = 0; i < bb; i++)
		bits[i] =
			static_cast<unsigned char>(scramble(scr, tap, bits[i]));
	if (p_->K) {
		for (i = 0; i < kk; i++)
			r0 |= static_cast<uint32_t>(bits[n++]) << i;
		for (i = 0; i < 4; i++) {
			ib[i] = bits[n] | bits[n + 1] << 1 | bits[n + 2] << 2;
			n += 3;
			for (j = 0; j < 2; j++) {
				int k, v = 0;
				for (k = 0; k < p_->q; k++)
					v |= bits[n++] << k;
				qb[2 * i + j] = v;
			}
		}
		p_->shell_map(r0, m);
	} else {
		/* 9.3.2/V.34: 3 I bits in the first b - 8 4D symbols, 2 in
		   the others */
		for (i = 0; i < 4; i++) {
			ib[i] = bits[n] | bits[n + 1] << 1;
			n += 2;
			if (i < bb - 8)
				ib[i] |= bits[n++] << 2;
		}
		zero(m);
		zero(qb);
	}

	for (i = 0; i < 4; i++) {
		int x[2], y[2], k, rot, u0 = 0, c0 = 0, lab[2];
		z = (z + (ib[i] >> 1)) & 3;
		/* in the order of Table 11/V.34 */
		for (k = 0; k < 2; k++) {
			const signed char *v = v34_quarter[(m[2 * i + k] << p_->q) +
							   qb[2 * i + k]];
			if (k) {
				c0 ^= pc.c_parity();
				u0 = (state ^ v0 ^ c0) & 1;
			} else
				c0 = pc.c_parity();
			rot = k ? z + 2 * (ib[i] & 1) + u0 : z;
			x[k] = v[0];
			y[k] = v[1];
			rotate(&x[k], &y[k], rot);
			if (p_->precode) {
				int32_t px = pc.p[0], py = pc.p[1];
				/* y(n) = u(n) + c(n) */
				x[k] += pc.c[0];
				y[k] += pc.c[1];
				p_->precoder_push(&pc, x[k], y[k]);
				sym[2 * i + k] =
					cplx_make(p_->g * (x[k] - px / 128.0f),
						  p_->g * (y[k] - py / 128.0f));
			} else
				sym[2 * i + k] = cplx_make(p_->g * x[k],
							      p_->g * y[k]);
			if (p_->theta > 0)
				sym[2 * i + k] = cplx_scale(
					nonlinear(sym[2 * i + k], p_->theta),
					p_->nl_gain);
			lab[k] = subset_label(x[k], y[k]);
		}
		state = trellis_next(p_->states, state,
					conv_in(lab[0], lab[1]));
		/* superframe synchronisation: the first 4D symbol of each
		   half data frame */
		if (++half >= 2 * p_->P) {
			half = 0;
			if (++pat >= 2 * p_->J)
				pat = 0;
			v0 = v34_sync_pattern[pat];
		} else
			v0 = 0;
	}
}

/* the next data symbol, in units of the training points: a new mapping
   frame every 8 */
cplx V34DataTx::symbol(uint32_t *scr, int tap)
{
	if (pos >= 8) {
		encode_frame(scr, tap);
		pos = 0;
	}
	return sym[pos++];
}



/*
 *    receiver
 */

/* rotation << 9 | index of each odd point with |x|, |y| <= 45 */
static int16_t lookup[46][46];

/* lookup[] from the quarter superconstellation and its rotations */
static void make_lookup(void)
{
	int i, k;
	for (auto &row : lookup)
		std::fill(std::begin(row), std::end(row), -1);
	for (i = 0; i < 416; i++)
		for (k = 0; k < 4; k++) {
			int x = v34_quarter[i][0], y = v34_quarter[i][1];
			rotate(&x, &y, k);
			lookup[(x + 45) / 2][(y + 45) / 2] =
				static_cast<int16_t>(k << 9 | i);
		}
}


/* the data receiver with the parameters p_ (those of the remote's
   transmitter) */
void V34DataRx::init(const V34Params *p_)
{
	reinit(*this);
	p = *p_;
	{
		static std::once_flag once;
		std::call_once(once, make_lookup);
	}
	viterbi_init();
}

/* the nearest point of the constellation in use to z (in units of the
   training points): its coordinates into *px, *py; returns its
   rotation << 9 | index, as lookup[] */
int V34Params::decide(cplx z, int *px, int *py) const
{
	float u = z.re / g, v = z.im / g, best = 1e9f;
	int x0 = 2 * static_cast<int>(floorf(u / 2)) + 1,
	    y0 = 2 * static_cast<int>(floorf(v / 2)) + 1;
	int dx, dy, ret = -1, lim = M << q;

	for (dx = -2; dx <= 2; dx += 2)
		for (dy = -2; dy <= 2; dy += 2) {
			int x = x0 + dx, y = y0 + dy, e;
			float dd;
			if (x < -45 || x > 45 || y < -45 || y > 45)
				continue;
			e = lookup[(x + 45) / 2][(y + 45) / 2];
			if (e < 0 || (e & 511) >= lim)
				continue;
			dd = (u - x) * (u - x) + (v - y) * (v - y);
			if (dd < best) {
				best = dd;
				ret = e;
				*px = x;
				*py = y;
			}
		}
	if (ret < 0) {
		/* far outside: the outermost points nearby */
		float s = 45 / (fabsf(u) > fabsf(v) ? fabsf(u) : fabsf(v));
		cplx wz = cplx_make(z.re * s * 0.9f, z.im * s * 0.9f);
		if (s < 1)
			return decide(wz, px, py);
		*px = *py = 1;
		return 0;
	}
	return ret;
}

/* the bits of the mapping frame of the 8 decided points in e[]: shell
   demapping, the differential decoding of the rotations, descrambling
   (scr, tap), out to link */
void V34DataRx::decode_frame(uint32_t *scr, int tap)
{
	const V34Params *p_ = &p;
	unsigned char bits[V34_MAX_B];
	int bb = p_->b, kk = p_->K, n = 0, i, j, m[8];
	uint32_t r0;

	sbc += p_->r;
	if (sbc >= p_->P)
		sbc -= p_->P;
	else if (bb > 12) {
		bb--;
		kk--;
	}
	for (i = 0; i < 8; i++)
		m[i] = (e[i] & 511) >> p_->q;
	if (p_->K) {
		r0 = p_->shell_demap(m);
		for (i = 0; i < kk; i++)
			bits[n++] = (r0 >> i) & 1;
	}
	for (i = 0; i < 4; i++) {
		int re = e[2 * i] >> 9, ro = e[2 * i + 1] >> 9;
		int i32 = (re - z) & 3, rel = (ro - re) & 3;
		int nb = p_->K || i < bb - 8 ? 3 : 2;
		z = re;
		bits[n++] = rel >> 1;
		bits[n++] = i32 & 1;
		if (nb == 3)
			bits[n++] = i32 >> 1;
		for (j = 0; j < 2 && p_->K; j++) {
			int k, v = e[2 * i + j] & ((1 << p_->q) - 1);
			for (k = 0; k < p_->q; k++)
				bits[n++] = (v >> k) & 1;
		}
	}
	for (i = 0; i < n; i++)
		bits[i] = static_cast<unsigned char>(
			descramble(scr, tap, bits[i]));
	link.put(bits, n);
}

/*
 *    Viterbi decoder for the 16 state code
 */

/* a point (x, y) as one int, and back */
#define PACK(x, y)      ((int32_t)(((x) & 0xFFFF) | (uint32_t)(y) << 16))
#define UNPACK_X(v)     ((int16_t)((v) & 0xFFFF))
#define UNPACK_Y(v)     ((v) >> 16)

/* the nearest point of each 2D subset to (u, v) (in units of the grid):
   among the points in use, or with precoding on the whole grid */
void V34Params::subset_points(float u, float v, float dist[8],
			      int32_t pt[8]) const
{
	int x0 = 2 * static_cast<int>(floorf(u / 2)) + 1,
	    y0 = 2 * static_cast<int>(floorf(v / 2)) + 1;
	int dx, dy, i, lim = M << q, edge = precode ? 261 : 45;

	/* outside: from the edge */
	if (x0 > edge) x0 = edge;
	if (x0 < -edge) x0 = -edge;
	if (y0 > edge) y0 = edge;
	if (y0 < -edge) y0 = -edge;
	for (i = 0; i < 8; i++) {
		dist[i] = 1e30f;
		pt[i] = PACK(1, 1);
	}
	for (dx = -6; dx <= 6; dx += 2)
		for (dy = -6; dy <= 6; dy += 2) {
			int x = x0 + dx, y = y0 + dy, l;
			float dd;
			if (!precode) {
				int e;
				if (x < -45 || x > 45 || y < -45 || y > 45)
					continue;
				e = lookup[(x + 45) / 2][(y + 45) / 2];
				if (e < 0 || (e & 511) >= lim)
					continue;
			}
			l = subset_label(x, y);
			dd = (u - x) * (u - x) + (v - y) * (v - y);
			if (dd < dist[l]) {
				dist[l] = dd;
				pt[l] = PACK(x, y);
			}
		}
}

/* the Viterbi decoder and the superframe pattern at the start of the
   data signal */
void V34DataRx::viterbi_init()
{
	/* the encoder starts at 0, but a receiver rotated by 90, 180 or
	   270 degrees sees the code sequence of another start state */
	vit.init(-1);
	half = 0;
	pat = 2 * p.J - 2;
	v0 = v34_sync_pattern[pat];
}

/* a decided channel output point y(n): with precoding u(n) = y(n) - c(n),
   as the transmitter's precoder had it */
void V34DataRx::point(int32_t pt, uint32_t *scr, int tap)
{
	int x = UNPACK_X(pt), y = UNPACK_Y(pt), e_ = -1;

	if (p.precode) {
		int32_t cx = pc.c[0], cy = pc.c[1];
		p.precoder_push(&pc, x, y);
		x -= cx;
		y -= cy;
	}
	if (x >= -45 && x <= 45 && y >= -45 && y <= 45)
		e_ = lookup[(x + 45) / 2][(y + 45) / 2];
	e[pos++] = e_ < 0 ? 0 : e_;
	if (pos == 8) {
		decode_frame(scr, tap);
		pos = 0;
	}
}

/* z: the equalized symbol, in units of the training points (with
   precoding: equalized to 1 + H(z)); returns the (tentative) decided
   point for the adaptation */
cplx V34DataRx::symbol(cplx z_, uint32_t *scr, int tap)
{
	const V34Params *p_ = &p;
	float u = z_.re / p_->g, v = z_.im / p_->g;
	cplx ret;
	int32_t out[2];

	if (p_->precode) {
		/* z is y(n) + noise (the receiver's equalizer includes
		   1 + H(z)): the nearest point of the grid */
		float yu = 2 * floorf(u / 2) + 1, yv = 2 * floorf(v / 2) + 1;
		yu = yu > 261 ? 261 : yu < -261 ? -261 : yu;
		yv = yv > 261 ? 261 : yv < -261 ? -261 : yv;
		ret = cplx_make(p_->g * yu, p_->g * yv);
	} else {
		int x, y;
		p_->decide(z_, &x, &y);
		ret = cplx_make(p_->g * x, p_->g * y);
	}
	if (!half2) {
		p_->subset_points(u, v, da, ca);
		half2 = 1;
	} else {
		float db[8];
		int32_t cb[8];
		p_->subset_points(u, v, db, cb);
		half2 = 0;
		int got = vit.step(p.states, v0, da, ca, db, cb, out);
		/* the superframe inversions, as the transmitter */
		if (++half >= 2 * p.P) {
			half = 0;
			if (++pat >= 2 * p.J)
				pat = 0;
			v0 = v34_sync_pattern[pat];
		} else
			v0 = 0;
		if (got) {
			point(out[0], scr, tap);
			point(out[1], scr, tap);
		}
	}
	return ret;
}

} // namespace dsp
