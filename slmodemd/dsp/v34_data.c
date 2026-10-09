/*
 *
 *    v34_data.c - V.34 data mode (section 9/V.34): framing, shell
 *                 mapping, differential encoding, the 16 state 4D
 *                 trellis code and superframe synchronisation
 *
 *    Without precoding (the MP sequences carry zero coefficients) or
 *    non-linear encoding, as the original library negotiates it.  As it
 *    does:
 *    - the trellis output Y0 of a 4D symbol is the low bit of the
 *      encoder state before that symbol's labels are shifted in, with
 *      the state recursion of Figure 10/V.34
 *    - the superframe inversion pattern starts at its bit 2J - 2 with
 *      the first 4D symbol of the data signal
 *
 *    The receiver decides on the nearest points (no Viterbi decoder).
 *
 */

#include <math.h>
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

/* the bits that the labels of a 4D symbol feed into the 16 state
   encoder: bit 0 into the first stage, bit 1 into the second and
   third */
static int conv_in(int s0, int s1)
{
	static const unsigned char row[4][8] = {
		{ 0, 0, 1, 1, 0, 0, 1, 1 },
		{ 3, 2, 2, 3, 3, 2, 2, 3 },
		{ 1, 1, 0, 0, 1, 1, 0, 0 },
		{ 2, 3, 3, 2, 2, 3, 3, 2 },
	};
	return row[s0 & 3][s1];
}

static int trellis_next(int st, int y)
{
	int t1 = st & 1;
	int nt = st >> 1;
	nt ^= (y & 1) | ((y >> 1) & 1) << 1 | ((y >> 1) & 1) << 2;
	return nt ^ (t1 << 2) ^ (t1 << 3);
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
	return 0;
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
		int x[2], y[2], k, rot, u0, lab[2];
		u0 = (d->state ^ d->v0) & 1;
		d->z = (d->z + (ib[i] >> 1)) & 3;
		for (k = 0; k < 2; k++) {
			const signed char *v = v34_quarter[(m[2 * i + k] << p->q) +
							   qb[2 * i + k]];
			rot = k ? d->z + 2 * (ib[i] & 1) + u0 : d->z;
			x[k] = v[0];
			y[k] = v[1];
			rotate(&x[k], &y[k], rot);
			lab[k] = subset_label(x[k], y[k]);
			d->sym[2 * i + k] = cplx_make(p->g * x[k], p->g * y[k]);
		}
		d->state = trellis_next(d->state, conv_in(lab[0], lab[1]));
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

static int lookup_init;
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
	lookup_init = 1;
}

static void v34_viterbi_init(struct v34_drx *d);

void v34_drx_init(struct v34_drx *d, const struct v34_dparm *p)
{
	memset(d, 0, sizeof(*d));
	d->p = *p;
	if (!lookup_init)
		make_lookup();
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

/* the nearest valid point of each 2D subset to z */
static void subset_points(const struct v34_dparm *p, cplx z,
			  float dist[8], short code[8])
{
	float u = z.re / p->g, v = z.im / p->g;
	int x0 = 2 * (int)floorf(u / 2) + 1, y0 = 2 * (int)floorf(v / 2) + 1;
	int dx, dy, i, lim = p->M << p->q;

	/* outside the constellation: from its edge */
	if (x0 > 45) x0 = 45;
	if (x0 < -45) x0 = -45;
	if (y0 > 45) y0 = 45;
	if (y0 < -45) y0 = -45;
	for (i = 0; i < 8; i++) {
		dist[i] = 1e30f;
		code[i] = -1;
	}
	for (dx = -6; dx <= 6; dx += 2)
		for (dy = -6; dy <= 6; dy += 2) {
			int x = x0 + dx, y = y0 + dy, e, l;
			float dd;
			if (x < -45 || x > 45 || y < -45 || y > 45)
				continue;
			e = lookup[(x + 45) / 2][(y + 45) / 2];
			if (e < 0 || (e & 511) >= lim)
				continue;
			l = subset_label(x, y);
			dd = (u - x) * (u - x) + (v - y) * (v - y);
			if (dd < dist[l]) {
				dist[l] = dd;
				code[l] = (short)e;
			}
		}
}

static void v34_viterbi_init(struct v34_drx *d)
{
	int s;
	for (s = 0; s < V34_STATES; s++)
		d->metric[s] = s ? 1e30f : 0;   /* the encoder starts at 0 */
	d->vt = d->vn = 0;
	d->half = 0;
	d->pat = 2 * d->p.J - 2;
	d->v0 = v34_sync_pattern[d->pat];
}

/* one 4D symbol in; returns 1 with a decided pair of point codes, after
   the decoding delay */
static int viterbi_4d(struct v34_drx *d, const float da[8], const short ca[8],
		      const float db[8], const short cb[8], short out[2])
{
	float gm[8], m2[V34_STATES], best;
	short ga[8], gb[8];
	int l0, l1, g, s, y, t = d->vt, bs, k;

	/* the best pair of subsets for each trellis input and Y0 */
	for (g = 0; g < 8; g++)
		gm[g] = 1e30f;
	for (l0 = 0; l0 < 8; l0++)
		for (l1 = 0; l1 < 8; l1++) {
			float m = da[l0] + db[l1];
			g = conv_in(l0, l1) | ((l0 ^ l1) & 1) << 2;
			if (m < gm[g]) {
				gm[g] = m;
				ga[g] = ca[l0];
				gb[g] = cb[l1];
			}
		}
	for (s = 0; s < V34_STATES; s++)
		m2[s] = 1e30f;
	for (s = 0; s < V34_STATES; s++) {
		/* U0 = Y0 ^ V0 (no precoding: C0 = 0) */
		int y0 = (s ^ d->v0) & 1;
		if (d->metric[s] >= 1e29f)
			continue;
		for (y = 0; y < 4; y++) {
			int ns = trellis_next(s, y);
			float m = d->metric[s] + gm[y | y0 << 2];
			if (m < m2[ns]) {
				m2[ns] = m;
				d->prev[t][ns] = (unsigned char)s;
				d->pa[t][ns] = ga[y | y0 << 2];
				d->pb[t][ns] = gb[y | y0 << 2];
			}
		}
	}
	best = m2[0];
	bs = 0;
	for (s = 1; s < V34_STATES; s++)
		if (m2[s] < best) {
			best = m2[s];
			bs = s;
		}
	for (s = 0; s < V34_STATES; s++)
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

static void drx_point(struct v34_drx *d, int code, unsigned *scr, int tap)
{
	d->e[d->pos++] = code;
	if (d->pos == 8) {
		drx_frame(d, scr, tap);
		d->pos = 0;
	}
}

/* z: the equalized symbol, in units of the training points; returns
   the (tentative) decided point for the adaptation */
cplx v34_drx_symbol(struct v34_drx *d, cplx z, unsigned *scr, int tap)
{
	int x, y;
	short out[2];

	decide(&d->p, z, &x, &y);
	if (!d->half2) {
		subset_points(&d->p, z, d->da, d->ca);
		d->half2 = 1;
	} else {
		float db[8];
		short cb[8];
		subset_points(&d->p, z, db, cb);
		d->half2 = 0;
		if (viterbi_4d(d, d->da, d->ca, db, cb, out)) {
			drx_point(d, out[0] < 0 ? 0 : out[0], scr, tap);
			drx_point(d, out[1] < 0 ? 0 : out[1], scr, tap);
		}
	}
	return cplx_make(d->p.g * x, d->p.g * y);
}
