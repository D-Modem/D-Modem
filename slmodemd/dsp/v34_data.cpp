/*
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

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "v34.h"

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

static int subset_label(int x, int y)
{
	return v34_subset[((x - 1) & 6) / 2 + 2 * ((y - 1) & 6)];
}

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

static int shell_tables(struct v34_dparm *p)
{
	int M = p->M, i, j;
	if (M < 1 || 2 * M - 1 > V34_G2_LEN || 8 * M - 6 > V34_Z8_LEN)
		return -1;
	memset(p->g2, 0, sizeof(p->g2));
	memset(p->g4, 0, sizeof(p->g4));
	for (i = 0; i < M; i++)
		for (j = 0; j < M; j++)
			p->g2[i + j]++;
	for (i = 0; i < 2 * M - 1; i++)
		for (j = 0; j < 2 * M - 1; j++)
			p->g4[i + j] += p->g2[i] * p->g2[j];
	p->z8[0] = 0;
	for (i = 0; i < 8 * M - 7; i++) {
		unsigned long long g8 = 0;
		for (j = 0; j <= i; j++)
			if (j < 4 * M - 3 && i - j < 4 * M - 3)
				g8 += p->g4[j] * p->g4[i - j];
		p->z8[i + 1] = p->z8[i] + g8;
	}
	return 0;
}

static unsigned long long g2p(const struct v34_dparm *p, int i)
{
	return i >= 0 && i < 2 * p->M - 1 ? p->g2[i] : 0;
}

static unsigned long long g4p(const struct v34_dparm *p, int i)
{
	return i >= 0 && i < 4 * p->M - 3 ? p->g4[i] : 0;
}

/* the mean power of the points of a frame with k shell mapping bits */
static double frame_power(const struct v34_dparm *p, int k)
{
	int M = p->M, a, m, i;
	double ringp[V34_MAX_RINGS], left = ldexp(1, k), e = 0;
	double g7[8 * V34_MAX_RINGS];

	for (m = 0; m < M; m++) {
		double s = 0;
		for (i = 0; i < 1 << p->q; i++) {
			const signed char *v = v34_quarter[(m << p->q) + i];
			s += v[0] * v[0] + v[1] * v[1];
		}
		ringp[m] = s / (1 << p->q);
	}
	if (k == 0)
		return ringp[0];
	/* 7-tuples of rings by their sum */
	for (a = 0; a <= 7 * (M - 1); a++) {
		g7[a] = 0;
		for (i = 0; i <= a; i++) {
			double g3 = 0;
			for (m = 0; m < M && m <= a - i; m++)
				g3 += (double)g2p(p, a - i - m);
			g7[a] += (double)g4p(p, i) * g3;
		}
	}
	for (a = 0; a <= 8 * (M - 1) && left > 0; a++) {
		double g8 = (double)(p->z8[a + 1] - p->z8[a]), take, ea = 0;
		take = g8 < left ? g8 : left;
		for (m = 0; m < M && m <= a; m++)
			if (a - m <= 7 * (M - 1))
				ea += g7[a - m] * ringp[m];
		e += take * ea / g8;
		left -= take;
	}
	return e / ldexp(1, k);
}

int v34_data_params(struct v34_dparm *p, int baud, int n, int expanded)
{
	const struct v34_rate *rt;
	double pw;

	memset(p, 0, sizeof(*p));
	if (baud < 0 || baud >= V34_NBAUDS || n < 1 || n > 14)
		return -1;
	rt = &v34_rates[baud][n - 1];
	if (!rt->b)
		return -1;
	p->rate = 2400 * n;
	p->states = 16;
	p->b = rt->b;
	p->M = expanded ? rt->m_exp : rt->m_min;
	p->J = v34_bauds[baud].J;
	p->P = v34_bauds[baud].P;
	if (p->b > 12) {
		p->K = p->b - 12;
		while (p->K >= 32) {
			p->K -= 8;
			p->q++;
		}
	}
	/* high mapping frames per data frame (8.2/V.34) */
	p->r = p->rate * 28 / (p->J * 100) - (p->b - 1) * p->P;
	if (shell_tables(p) || (p->M << p->q) > 416)
		return -1;
	pw = frame_power(p, p->K);
	if (p->b > 12)
		pw = (p->r * pw + (p->P - p->r) * frame_power(p, p->K - 1)) / p->P;
	/* the same power as the 4 point training signals */
	p->g = (float)sqrt(2 / pw);
	p->w = p->b < 56 ? 1 : 2;
	return 0;
}

void v34_data_precode(struct v34_dparm *p, const int h[6])
{
	double pw = 2 / (p->g * p->g);
	int i;

	p->precode = 0;
	for (i = 0; i < 6; i++) {
		p->h[i] = h[i];
		if (h[i])
			p->precode = 1;
	}
	/* x(n) = u(n) + c(n) - p(n): u(n) plus a component uniform over
	   a 2^w square; the same power as without precoding */
	if (p->precode)
		p->g = (float)sqrt(2 / (pw + (1 << 2 * p->w) / 6.0));
}

/* to the nearest multiple of 2^s; half way: to the smaller magnitude */
static int round_shift(long long v, int s)
{
	return (int)((v + (1LL << (s - 1)) - 1 + (v < 0)) >> s);
}

/* x(n) = y(n) - p(n) goes in: p(n + 1) and c(n + 1) */
static void precoder_push(const struct v34_dparm *p, struct v34_precoder *pc,
			  int yr, int yi)
{
	long long qr = 0, qi = 0;
	int k;

	for (k = 2; k > 0; k--) {
		pc->x[k][0] = pc->x[k - 1][0];
		pc->x[k][1] = pc->x[k - 1][1];
	}
	pc->x[0][0] = yr * 128 - pc->p[0];
	pc->x[0][1] = yi * 128 - pc->p[1];
	for (k = 0; k < 3; k++) {
		long long xr = pc->x[k][0], xi = pc->x[k][1];
		int hr = p->h[2 * k], hi = p->h[2 * k + 1];
		qr += xr * hr - xi * hi;
		qi += xr * hi + xi * hr;
	}
	pc->p[0] = round_shift(qr, 14);
	pc->p[1] = round_shift(qi, 14);
	pc->c[0] = round_shift(pc->p[0], 7 + p->w) << p->w;
	pc->c[1] = round_shift(pc->p[1], 7 + p->w) << p->w;
}

/* the bit of the modulo encoder (9.6.3.3) for c(n) */
static int c_parity(const struct v34_precoder *pc)
{
	return ((pc->c[0] + pc->c[1]) >> 1) & 1;
}

static void shell_map(const struct v34_dparm *p, unsigned long r0, int m[8])
{
	unsigned long long r1, r2, r3, r4, r5, t;
	int a, b, c, d, e, f, g, h, M = p->M;

	for (a = 1; a < 8 * M - 6 && p->z8[a] <= r0; a++)
		;
	a--;
	r1 = r0 - p->z8[a];
	for (b = 0; ; b++) {
		t = g4p(p, b) * g4p(p, a - b);
		if (r1 < t)
			break;
		r1 -= t;
	}
	r2 = r1 % g4p(p, b);
	r3 = r1 / g4p(p, b);
	for (c = 0; ; c++) {
		t = g2p(p, c) * g2p(p, b - c);
		if (r2 < t)
			break;
		r2 -= t;
	}
	r4 = r2;
	for (d = 0; ; d++) {
		t = g2p(p, d) * g2p(p, a - b - d);
		if (r3 < t)
			break;
		r3 -= t;
	}
	r5 = r3;
	e = (int)(r4 % g2p(p, c));
	f = (int)(r4 / g2p(p, c));
	g = (int)(r5 % g2p(p, d));
	h = (int)(r5 / g2p(p, d));
#define PAIR(i, sum, v) do { \
		if ((sum) < M) { m[i] = (v); m[(i) + 1] = (sum) - (v); } \
		else { m[(i) + 1] = M - 1 - (v); m[i] = (sum) - m[(i) + 1]; } \
	} while (0)
	PAIR(0, c, e);
	PAIR(2, b - c, f);
	PAIR(4, d, g);
	PAIR(6, a - b - d, h);
#undef PAIR
}

static int pair_index(const struct v34_dparm *p, int x, int y)
{
	return x + y < p->M ? x : p->M - 1 - y;
}

static unsigned long shell_demap(const struct v34_dparm *p, const int m[8])
{
	int a = 0, b, c, d, i;
	unsigned long long r;

	for (i = 0; i < 8; i++)
		a += m[i];
	b = m[0] + m[1] + m[2] + m[3];
	c = m[0] + m[1];
	d = m[4] + m[5];
	r = p->z8[a];
	for (i = 0; i < b; i++)
		r += g4p(p, i) * g4p(p, a - i);
	/* r1 = r2 + r3 g4(b) */
	{
		unsigned long long r2 = 0, r3 = 0;
		for (i = 0; i < c; i++)
			r2 += g2p(p, i) * g2p(p, b - i);
		r2 += pair_index(p, m[0], m[1]) +
			(unsigned long long)pair_index(p, m[2], m[3]) * g2p(p, c);
		for (i = 0; i < d; i++)
			r3 += g2p(p, i) * g2p(p, a - b - i);
		r3 += pair_index(p, m[4], m[5]) +
			(unsigned long long)pair_index(p, m[6], m[7]) * g2p(p, d);
		r += r2 + r3 * g4p(p, b);
	}
	return (unsigned long)r;
}


/*
 *    transmitter
 */

void v34_dtx_init(struct v34_dtx *d, const struct v34_dparm *p)
{
	memset(d, 0, sizeof(*d));
	d->p = *p;
	d->pat = 2 * p->J - 2;
	d->v0 = v34_sync_pattern[d->pat];
	d->pos = 8;
}

static void dtx_frame(struct v34_dtx *d, unsigned *scr, int tap)
{
	const struct v34_dparm *p = &d->p;
	unsigned char bits[V34_MAX_B];
	int bb = p->b, kk = p->K, n = 0, i, j, m[8], ib[4], qb[8];
	unsigned long r0 = 0;

	d->sbc += p->r;
	if (d->sbc >= p->P)
		d->sbc -= p->P;
	else if (bb > 12) {
		bb--;
		kk--;
	}
	d->get_bits(d->ctx, bits, bb);
	for (i = 0; i < bb; i++) {
		int b = (bits[i] ^ (*scr >> (tap - 1)) ^ (*scr >> 22)) & 1;
		*scr = (*scr << 1) | b;
		bits[i] = b;
	}
	if (p->K) {
		for (i = 0; i < kk; i++)
			r0 |= (unsigned long)bits[n++] << i;
		for (i = 0; i < 4; i++) {
			ib[i] = bits[n] | bits[n + 1] << 1 | bits[n + 2] << 2;
			n += 3;
			for (j = 0; j < 2; j++) {
				int k, v = 0;
				for (k = 0; k < p->q; k++)
					v |= bits[n++] << k;
				qb[2 * i + j] = v;
			}
		}
		shell_map(p, r0, m);
	} else {
		/* 9.3.2/V.34: 3 I bits in the first b - 8 4D symbols, 2 in
		   the others */
		for (i = 0; i < 4; i++) {
			ib[i] = bits[n] | bits[n + 1] << 1;
			n += 2;
			if (i < bb - 8)
				ib[i] |= bits[n++] << 2;
		}
		memset(m, 0, sizeof(m));
		memset(qb, 0, sizeof(qb));
	}

	for (i = 0; i < 4; i++) {
		int x[2], y[2], k, rot, u0 = 0, c0 = 0, lab[2];
		d->z = (d->z + (ib[i] >> 1)) & 3;
		/* in the order of Table 11/V.34 */
		for (k = 0; k < 2; k++) {
			const signed char *v = v34_quarter[(m[2 * i + k] << p->q) +
							   qb[2 * i + k]];
			if (k) {
				c0 ^= c_parity(&d->pc);
				u0 = (d->state ^ d->v0 ^ c0) & 1;
			} else
				c0 = c_parity(&d->pc);
			rot = k ? d->z + 2 * (ib[i] & 1) + u0 : d->z;
			x[k] = v[0];
			y[k] = v[1];
			rotate(&x[k], &y[k], rot);
			if (p->precode) {
				int px = d->pc.p[0], py = d->pc.p[1];
				/* y(n) = u(n) + c(n) */
				x[k] += d->pc.c[0];
				y[k] += d->pc.c[1];
				precoder_push(p, &d->pc, x[k], y[k]);
				d->sym[2 * i + k] =
					cplx_make(p->g * (x[k] - px / 128.0f),
						  p->g * (y[k] - py / 128.0f));
			} else
				d->sym[2 * i + k] = cplx_make(p->g * x[k],
							      p->g * y[k]);
			lab[k] = subset_label(x[k], y[k]);
		}
		d->state = trellis_next(p->states, d->state,
					conv_in(lab[0], lab[1]));
		/* superframe synchronisation: the first 4D symbol of each
		   half data frame */
		if (++d->half >= 2 * p->P) {
			d->half = 0;
			if (++d->pat >= 2 * p->J)
				d->pat = 0;
			d->v0 = v34_sync_pattern[d->pat];
		} else
			d->v0 = 0;
	}
}

cplx v34_dtx_symbol(struct v34_dtx *d, unsigned *scr, int tap)
{
	if (d->pos >= 8) {
		dtx_frame(d, scr, tap);
		d->pos = 0;
	}
	return d->sym[d->pos++];
}


/*
 *    receiver
 */

/* rotation << 9 | index of each odd point with |x|, |y| <= 45 */
static short lookup[46][46];

static void make_lookup(void)
{
	int i, k;
	memset(lookup, 0xFF, sizeof(lookup));
	for (i = 0; i < 416; i++)
		for (k = 0; k < 4; k++) {
			int x = v34_quarter[i][0], y = v34_quarter[i][1];
			rotate(&x, &y, k);
			lookup[(x + 45) / 2][(y + 45) / 2] = (short)(k << 9 | i);
		}
}

static void v34_viterbi_init(struct v34_drx *d);

void v34_drx_init(struct v34_drx *d, const struct v34_dparm *p)
{
	memset(d, 0, sizeof(*d));
	d->p = *p;
	{
		static pthread_once_t once = PTHREAD_ONCE_INIT;
		pthread_once(&once, make_lookup);
	}
	v34_viterbi_init(d);
}

/* the nearest point of the constellation in use */
static int decide(const struct v34_dparm *p, cplx z, int *px, int *py)
{
	float u = z.re / p->g, v = z.im / p->g, best = 1e9f;
	int x0 = 2 * (int)floorf(u / 2) + 1, y0 = 2 * (int)floorf(v / 2) + 1;
	int dx, dy, ret = -1, lim = p->M << p->q;

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
		cplx w = cplx_make(z.re * s * 0.9f, z.im * s * 0.9f);
		if (s < 1)
			return decide(p, w, px, py);
		*px = *py = 1;
		return 0;
	}
	return ret;
}

static void drx_frame(struct v34_drx *d, unsigned *scr, int tap)
{
	const struct v34_dparm *p = &d->p;
	unsigned char bits[V34_MAX_B];
	int bb = p->b, kk = p->K, n = 0, i, j, m[8];
	unsigned long r0;

	d->sbc += p->r;
	if (d->sbc >= p->P)
		d->sbc -= p->P;
	else if (bb > 12) {
		bb--;
		kk--;
	}
	for (i = 0; i < 8; i++)
		m[i] = (d->e[i] & 511) >> p->q;
	if (p->K) {
		r0 = shell_demap(p, m);
		for (i = 0; i < kk; i++)
			bits[n++] = (r0 >> i) & 1;
	}
	for (i = 0; i < 4; i++) {
		int re = d->e[2 * i] >> 9, ro = d->e[2 * i + 1] >> 9;
		int i32 = (re - d->z) & 3, rel = (ro - re) & 3;
		int nb = p->K || i < bb - 8 ? 3 : 2;
		d->z = re;
		bits[n++] = rel >> 1;
		bits[n++] = i32 & 1;
		if (nb == 3)
			bits[n++] = i32 >> 1;
		for (j = 0; j < 2 && p->K; j++) {
			int k, v = d->e[2 * i + j] & ((1 << p->q) - 1);
			for (k = 0; k < p->q; k++)
				bits[n++] = (v >> k) & 1;
		}
	}
	for (i = 0; i < n; i++) {
		int b = bits[i];
		bits[i] = (b ^ (*scr >> (tap - 1)) ^ (*scr >> 22)) & 1;
		*scr = (*scr << 1) | b;
	}
	d->put_bits(d->ctx, bits, n);
}

/*
 *    Viterbi decoder for the 16 state code
 */

#define PACK(x, y)      ((int)(((x) & 0xFFFF) | (unsigned)(y) << 16))
#define UNPACK_X(v)     ((short)((v) & 0xFFFF))
#define UNPACK_Y(v)     ((v) >> 16)

/* the nearest point of each 2D subset to (u, v) (in units of the grid):
   among the points in use, or with precoding on the whole grid */
static void subset_points(const struct v34_dparm *p, float u, float v,
			  float dist[8], int pt[8])
{
	int x0 = 2 * (int)floorf(u / 2) + 1, y0 = 2 * (int)floorf(v / 2) + 1;
	int dx, dy, i, lim = p->M << p->q, edge = p->precode ? 261 : 45;

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
			if (!p->precode) {
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

static void v34_viterbi_init(struct v34_drx *d)
{
	int s;
	/* the encoder starts at 0, but a receiver rotated by 90, 180 or
	   270 degrees sees the code sequence of another start state */
	for (s = 0; s < V34_STATES; s++)
		d->metric[s] = 0;
	d->vt = d->vn = 0;
	d->half = 0;
	d->pat = 2 * d->p.J - 2;
	d->v0 = v34_sync_pattern[d->pat];
}

/* one 4D symbol in; returns 1 with a decided pair of point codes, after
   the decoding delay */
static int viterbi_4d(struct v34_drx *d, const float da[8], const int ca[8],
		      const float db[8], const int cb[8], int out[2])
{
	float gm[32], m2[V34_STATES], best;
	int ga[32], gb[32];
	int states = d->p.states, l0, l1, g, s, y, t = d->vt, bs, k;
	/* the 16 state code only uses Y1 and Y2 */
	int ny = states == 16 ? 4 : 16;

	/* the best pair of subsets for each trellis input and Y0 */
	for (g = 0; g < 2 * ny; g++)
		gm[g] = 1e30f;
	for (l0 = 0; l0 < 8; l0++)
		for (l1 = 0; l1 < 8; l1++) {
			float m = da[l0] + db[l1];
			g = (conv_in(l0, l1) & (ny - 1)) +
			    ((l0 ^ l1) & 1) * ny;
			if (m < gm[g]) {
				gm[g] = m;
				ga[g] = ca[l0];
				gb[g] = cb[l1];
			}
		}
	for (s = 0; s < states; s++)
		m2[s] = 1e30f;
	for (s = 0; s < states; s++) {
		/* U0 = Y0 ^ V0 (no precoding: C0 = 0) */
		int y0 = (s ^ d->v0) & 1;
		if (d->metric[s] >= 1e29f)
			continue;
		for (y = 0; y < ny; y++) {
			int ns = trellis_next(states, s, y);
			float m = d->metric[s] + gm[y + y0 * ny];
			if (m < m2[ns]) {
				m2[ns] = m;
				d->prev[t][ns] = (unsigned char)s;
				d->pa[t][ns] = ga[y + y0 * ny];
				d->pb[t][ns] = gb[y + y0 * ny];
			}
		}
	}
	best = m2[0];
	bs = 0;
	for (s = 1; s < states; s++)
		if (m2[s] < best) {
			best = m2[s];
			bs = s;
		}
	for (s = 0; s < states; s++)
		d->metric[s] = m2[s] - best;

	/* the superframe inversions, as the transmitter */
	if (++d->half >= 2 * d->p.P) {
		d->half = 0;
		if (++d->pat >= 2 * d->p.J)
			d->pat = 0;
		d->v0 = v34_sync_pattern[d->pat];
	} else
		d->v0 = 0;

	d->vt = (t + 1) % V34_TB;
	if (++d->vn < V34_TB)
		return 0;
	/* trace back from the best state to the oldest symbol kept */
	s = bs;
	for (k = 0; k < V34_TB - 1; k++) {
		s = d->prev[t][s];
		t = (t + V34_TB - 1) % V34_TB;
	}
	out[0] = d->pa[t][s];
	out[1] = d->pb[t][s];
	return 1;
}

/* a decided channel output point y(n): with precoding u(n) = y(n) - c(n),
   as the transmitter's precoder had it */
static void drx_point(struct v34_drx *d, int pt, unsigned *scr, int tap)
{
	int x = UNPACK_X(pt), y = UNPACK_Y(pt), e = -1;

	if (d->p.precode) {
		int cx = d->pc.c[0], cy = d->pc.c[1];
		precoder_push(&d->p, &d->pc, x, y);
		x -= cx;
		y -= cy;
	}
	if (x >= -45 && x <= 45 && y >= -45 && y <= 45)
		e = lookup[(x + 45) / 2][(y + 45) / 2];
	d->e[d->pos++] = e < 0 ? 0 : e;
	if (d->pos == 8) {
		drx_frame(d, scr, tap);
		d->pos = 0;
	}
}

/* z: the equalized symbol, in units of the training points (with
   precoding: equalized to 1 + H(z)); returns the (tentative) decided
   point for the adaptation */
cplx v34_drx_symbol(struct v34_drx *d, cplx z, unsigned *scr, int tap)
{
	const struct v34_dparm *p = &d->p;
	float u = z.re / p->g, v = z.im / p->g;
	cplx ret;
	int out[2];

	if (p->precode) {
		/* z is y(n) + noise (the receiver's equalizer includes
		   1 + H(z)): the nearest point of the grid */
		float yu = 2 * floorf(u / 2) + 1, yv = 2 * floorf(v / 2) + 1;
		yu = yu > 261 ? 261 : yu < -261 ? -261 : yu;
		yv = yv > 261 ? 261 : yv < -261 ? -261 : yv;
		ret = cplx_make(p->g * yu, p->g * yv);
	} else {
		int x, y;
		decide(p, z, &x, &y);
		ret = cplx_make(p->g * x, p->g * y);
	}
	if (!d->half2) {
		subset_points(p, u, v, d->da, d->ca);
		d->half2 = 1;
	} else {
		float db[8];
		int cb[8];
		subset_points(p, u, v, db, cb);
		d->half2 = 0;
		if (viterbi_4d(d, d->da, d->ca, db, cb, out)) {
			drx_point(d, out[0], scr, tap);
			drx_point(d, out[1], scr, tap);
		}
	}
	return ret;
}
