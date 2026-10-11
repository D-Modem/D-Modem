/*
 *
 *    v92.cpp - ITU-T V.92 (11/2000) PCM upstream, the digital modem
 *
 *    After an INFO1a that asks for PCM upstream (Table 18) the two modems
 *    go through phase 3 (9.5) and phase 4 (9.6) of V.92; this runs them
 *    on the network's 8000 Hz samples, as V.91 does.
 *
 *    The analogue modem's signals are PAM symbols, one per sample.  The
 *    receiver finds Ru and Su by their period of 6 symbols (1333 Hz), and
 *    the two TRN1u by their known scrambled sequence: the correlation with
 *    it is the channel's impulse response, and gives the symbol (and,
 *    for the second TRN1u, the data frame) alignment.  A decision
 *    feedback equalizer is fitted to TRN1u by least squares and follows
 *    the signal from then on by its decisions.
 *
 *    Our CPd asks for no precoding (the prefilter passes the signal as
 *    it is), a uniform modulus M on all 12 data frame intervals and one
 *    constellation of 4M equally spaced points: the analogue modem
 *    sends, of the points of a class, the one of least power, and the
 *    receiver decides the point and takes its class.  The trellis code's
 *    parity (on the fourth symbol of a trellis frame) is not used.
 *
 *    The upstream rate comes from TRN1u's mean square error, with a
 *    margin for the errors' long tails (V92_MARGIN); a B1u that does not
 *    come through, or a failure before data, retrains to V.34 upstream.
 *
 *    The echo of our signal from the analogue modem's hybrid is the V.34
 *    pump's echo canceller's (dp_v34.cpp), trained while we send Sd to Jd
 *    and the analogue modem is silent.
 *
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "dsp.h"
#include "v92.h"

namespace dsp {

static int field(const unsigned char *b, int first, int nbits)
{
	int i, v = 0;
	for (i = nbits - 1; i >= 0; i--)
		v = (v << 1) | b[first + i];
	return v;
}

static void put_bits(unsigned char *b, int *n, unsigned val, int nbits)
{
	int i;
	for (i = 0; i < nbits; i++)
		b[(*n)++] = (val >> i) & 1;
}

/* the CRC of 17 bit blocks from bit 18 on, after the bits up to end */
static void put_crc(unsigned char *b, int *n)
{
	unsigned crc = 0xFFFF;
	int i;
	for (i = 18; i < *n - 1; i += 17)
		crc = v34_crc_bits(b + i, 16, crc);
	for (i = 15; i >= 0; i--)
		b[(*n)++] = (crc >> i) & 1;
}

static int crc_ok(const unsigned char *b, int len)
{
	unsigned crc = 0xFFFF;
	int i, end = len - 16;
	for (i = 18; i < end - 1; i += 17)
		crc = v34_crc_bits(b + i, 16, crc);
	for (i = 0; i < 16; i++)
		if (b[end + i] != ((crc >> (15 - i)) & 1))
			return 0;
	return 1;
}

/* GPA (7-2/V.34): 1 + x^-5 + x^-23 */
static int gpa_scr(unsigned *r, int bit)
{
	int o = (bit ^ (*r >> 4) ^ (*r >> 22)) & 1;
	*r = (*r << 1) | o;
	return o;
}

static int gpa_descr(unsigned *r, int bit)
{
	int o = (bit ^ (*r >> 4) ^ (*r >> 22)) & 1;
	*r = (*r << 1) | (bit & 1);
	return o;
}

void V92Info1a::parse(const unsigned char *b)
{
	sections = field(b, 12, 2);
	ltot = 192 + 64 * field(b, 14, 2);
	lmax = 128 + 64 * field(b, 16, 2);
	md = field(b, 18, 7);
	uinfo = field(b, 25, 7);
}

/*
 *    sequences
 */

#define V92_TAPS        4       /* of each precoder and prefilter section */

static int fill_to(unsigned char *b, int n, int fill)
{
	while (fill > 0 && n % fill)
		b[n++] = 0;
	return n;
}

int V92Cpd::build(unsigned char *b, int fill) const
{
	int n = 0, i, j, g4 = (int)lrint(4 * g * 65536);

	if (g4 > 65535)
		g4 = 65535;
	if (g4 < 1)
		g4 = 1;
	put_bits(b, &n, 0x1FFFF, 17);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, 0, 1);          /* CPd */
	put_bits(b, &n, 7, 3);          /* all three parts */
	put_bits(b, &n, drn, 5);
	put_bits(b, &n, 0, 2);          /* 16 state trellis */
	put_bits(b, &n, 0, 1);          /* E2u not extended */
	put_bits(b, &n, 0, 3);
	put_bits(b, &n, ack, 1);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, g4, 16);
	/* modulus encoder */
	for (i = 0; i < 12; i += 2) {
		put_bits(b, &n, 0, 1);
		put_bits(b, &n, M[i], 8);
		put_bits(b, &n, M[i + 1], 8);
	}
	/* precoder and prefilter: all zero but z2(0); 4 taps each, as
	   the original library's filters have a multiple of 4 taps (and
	   fail on none) */
	for (i = 0; i < 4; i++) {
		put_bits(b, &n, 0, 1);
		put_bits(b, &n, V92_TAPS, 9);
		put_bits(b, &n, 0, 7);
	}
	for (i = 0; i < 4 * V92_TAPS; i++) {
		put_bits(b, &n, 0, 1);
		put_bits(b, &n, i == 2 * V92_TAPS ? 32767 : 0, 16);
	}
	/* constellations */
	put_bits(b, &n, 0, 1);
	for (i = 0; i < 4; i++)
		put_bits(b, &n, con[i], 4);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, con[4], 4);
	put_bits(b, &n, con[5], 4);
	put_bits(b, &n, 0, 8);
	for (i = 0; i < 6; i += 2) {
		put_bits(b, &n, 0, 1);
		put_bits(b, &n, i < ncon ? lc[i] : 0, 8);
		put_bits(b, &n, i + 1 < ncon ? lc[i + 1] : 0, 8);
	}
	for (i = 0; i < ncon; i++)
		for (j = 0; j < lc[i]; j++) {
			put_bits(b, &n, 0, 1);
			put_bits(b, &n, pts[i][j], 16);
		}
	put_bits(b, &n, 0, 1);
	put_crc(b, &n);
	put_bits(b, &n, 0, 1);
	return fill_to(b, n, fill);
}

int v92_suv_build(unsigned char *b, int ack, int fill)
{
	int n = 0;
	put_bits(b, &n, 0x1FFFF, 17);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, 1, 1);          /* SUVd */
	put_bits(b, &n, 0, 13);
	put_bits(b, &n, 0, 1);          /* no silent period */
	put_bits(b, &n, ack, 1);
	put_bits(b, &n, 0, 1);
	put_crc(b, &n);
	put_bits(b, &n, 0, 1);
	return fill_to(b, n, fill);
}

/* Table 12/V.90's alpha and beta, and the 17 bit blocks of N Ucodes */
static int dil_beta(const unsigned char *b)
{
	int lsp = field(b, 35, 7) + 1, ltp = field(b, 43, 7) + 1;
	return (lsp + 15) / 16 * 17 + (ltp + 15) / 16 * 17;
}

int v92_dil_length(const unsigned char *b, int n)
{
	if (n < 50)
		return -1;
	return 238 + dil_beta(b) + (field(b, 18, 8) + 1) / 2 * 17;
}

int v92_cp_parse(V90Cp *c, const unsigned char *b, int len)
{
	static const int con_bit[6] = { 103, 107, 111, 115, 120, 124 };
	int i, j, k;

	if (!crc_ok(b, len))
		return -1;
	memset(c, 0, sizeof(*c));
	c->cp = field(b, 19, 2) == 1;
	c->drn = field(b, 21, 5);
	c->sr = field(b, 31, 2);
	c->ack = b[33];
	c->alaw = b[35];
	c->ld = field(b, 49, 2);
	c->a1 = (signed char)field(b, 69, 8);
	c->a2 = (signed char)field(b, 77, 8);
	c->b1 = (signed char)field(b, 86, 8);
	c->b2 = (signed char)field(b, 94, 8);
	for (i = 0; i < 6; i++) {
		c->con[i] = field(b, con_bit[i], 4);
		if (c->con[i] + 1 > c->ncon)
			c->ncon = c->con[i] + 1;
	}
	for (j = 0; j < c->ncon; j++)
		for (k = 0; k < 8; k++)
			for (i = 0; i < 16; i++)
				c->mask[j][16 * k + i] =
					b[136 + 136 * j + 17 * k + 1 + i];
	return 0;
}

void V92Frames::init(int mode_)
{
	memset(this, 0, sizeof(*this));
	mode = mode_;
}

int V92Frames::bit(int x)
{
	sr = (sr << 1) | (x & 1);
	if (!n) {
		if ((sr & 0x3FFFF) == 0x3FFFE) {
			memset(b, 1, 17);
			b[17] = 0;
			n = 18;
			len = 0;
		}
		return 0;
	}
	b[n++] = x & 1;
	if (mode == V92F_DIL) {
		if (n == 50) {
			len = v92_dil_length(b, n);
			if (len <= 50 || len > V92_FR_MAX) {
				n = 0;
				return 0;
			}
		}
	} else {
		/* SUVu, CPus: 51 bits; CPt, CPu: by their constellations */
		if (n == 19 && b[18])
			len = 51;
		if (n == 21 && !b[18] && field(b, 19, 2) == 2)
			len = 51;
		if (n == 129 && !len) {
			len = v90_cp_length(b, n);
			if (len <= 129 || len > V92_FR_MAX) {
				n = 0;
				return 0;
			}
		}
	}
	if (len && n == len) {
		n = 0;
		return crc_ok(b, len) ? 1 : -1;
	}
	if (n >= V92_FR_MAX)
		n = 0;
	return 0;
}

/*
 *    receiver
 */

#define TRN_NC          1024    /* symbols correlated */
#define TRN_NT          1536    /* symbols of the least squares fit */
#define TONE_MIN_E      6e4f    /* a block of 6 samples: -34 dBm0 */

static signed char trn_ref[TRN_NT];

/* TRN1u: scrambled ones from a cleared scrambler, 0 positive */
static void trn_ref_init()
{
	unsigned r = 0;
	int m;
	if (trn_ref[0])
		return;
	for (m = 0; m < TRN_NT; m++)
		trn_ref[m] = gpa_scr(&r, 1) ? -1 : 1;
}

static inline float at(const float *buf, long i)
{
	return buf[i & (V92_BUF - 1)];
}

/* the least squares fit: A w = v, A of dim x dim, in place */
static int solve(double *A, double *v, double *w, int dim)
{
	int i, j, k;
	for (i = 0; i < dim; i++) {
		int p = i;
		for (k = i + 1; k < dim; k++)
			if (fabs(A[k * dim + i]) > fabs(A[p * dim + i]))
				p = k;
		if (fabs(A[p * dim + i]) < 1e-12)
			return -1;
		if (p != i) {
			for (j = 0; j < dim; j++) {
				double t = A[i * dim + j];
				A[i * dim + j] = A[p * dim + j];
				A[p * dim + j] = t;
			}
			double t = v[i];
			v[i] = v[p];
			v[p] = t;
		}
		for (k = i + 1; k < dim; k++) {
			double m = A[k * dim + i] / A[i * dim + i];
			if (m == 0)
				continue;
			for (j = i; j < dim; j++)
				A[k * dim + j] -= m * A[i * dim + j];
			v[k] -= m * v[i];
		}
	}
	for (i = dim - 1; i >= 0; i--) {
		double s = v[i];
		for (j = i + 1; j < dim; j++)
			s -= A[i * dim + j] * w[j];
		w[i] = s / A[i * dim + i];
	}
	return 0;
}

/* TRN1u from about start on: find its first symbol in [lo, hi] by the
   correlation with the known sequence, and fit the equalizer */
int V92Up::train(long start, int lo, int hi)
{
	static double A[V92_EQ_N * V92_EQ_N];
	double v[V92_EQ_N], w[V92_EQ_N], phi[V92_EQ_N], e2 = 0;
	double best = 0, c0 = 0;
	int l, bl = lo, m, s, i, j;

	for (l = lo; l <= hi; l++) {
		double c = 0;
		for (m = 0; m < TRN_NC; m++)
			c += at(buf, start + l + m) * trn_ref[m];
		if (fabs(c) > fabs(best)) {
			best = c;
			bl = l;
		}
	}
	for (m = 0; m < TRN_NC; m++)
		c0 += at(buf, start + bl + m) * at(buf, start + bl + m);
	eqL = start + bl;
	DSP_DBG("v92: TRN1u at %+d (of %d..%d): main tap %.0f, %.0f%% of "
		"the power\n", bl, lo, hi, best / TRN_NC,
		100 * best * best / (TRN_NC * c0 + 1));
	memset(A, 0, sizeof(A));
	memset(v, 0, sizeof(v));
	for (s = 40; s < TRN_NT; s++) {
		long base = eqL + s - V92_EQ_PRE;
		for (i = 0; i < V92_EQ_F; i++)
			phi[i] = at(buf, base + i);
		for (j = 0; j < V92_EQ_B; j++)
			phi[V92_EQ_F + j] = -trn_ref[s - 1 - j];
		for (i = 0; i < V92_EQ_N; i++) {
			for (j = 0; j < V92_EQ_N; j++)
				A[i * V92_EQ_N + j] += phi[i] * phi[j];
			v[i] += phi[i] * trn_ref[s];
		}
	}
	/* a little ridge, by column: the feedback columns are of +-1,
	   the others of samples */
	for (i = 0; i < V92_EQ_N; i++)
		A[i * V92_EQ_N + i] *= 1 + 1e-6;
	if (solve(A, v, w, V92_EQ_N)) {
		DSP_DBG("v92: no equalizer\n");
		return -1;
	}
	for (i = 0; i < V92_EQ_F; i++)
		f[i] = w[i];
	for (j = 0; j < V92_EQ_B; j++)
		b[j] = w[V92_EQ_F + j];
	for (s = 40; s < TRN_NT; s++) {
		double y = 0;
		long base = eqL + s - V92_EQ_PRE;
		for (i = 0; i < V92_EQ_F; i++)
			y += f[i] * at(buf, base + i);
		for (j = 0; j < V92_EQ_B; j++)
			y -= b[j] * trn_ref[s - 1 - j];
		e2 += (trn_ref[s] - y) * (trn_ref[s] - y);
	}
	mse_trn = (float)(e2 / (TRN_NT - 40));
	DSP_DBG("v92: equalizer fitted, mse %.5f (%.1f dB)\n", mse_trn,
		-10 * log10f(mse_trn + 1e-9f));
	/* on from the end of the fit, with its decisions */
	for (j = 0; j < V92_EQ_B; j++)
		dh[j] = trn_ref[TRN_NT - 1 - j];
	sym = TRN_NT;
	last_d = trn_ref[TRN_NT - 1] < 0;
	dscr = 0;
	for (s = TRN_NT - 24; s < TRN_NT; s++) {
		int d = trn_ref[s] < 0;
		gpa_descr(&dscr, d ^ (trn_ref[s - 1] < 0));
	}
	mse = mse_trn;
	trained = 1;
	return 0;
}

float V92Up::equalize(long s)
{
	long base = eqL + s - V92_EQ_PRE;
	double y = 0;
	int i;
	for (i = 0; i < V92_EQ_F; i++)
		y += f[i] * at(buf, base + i);
	for (i = 0; i < V92_EQ_B; i++)
		y -= b[i] * dh[i];
	return (float)y;
}

/* LMS, then the decision t goes into the feedback */
void V92Up::adapt(long s, float err, float mu)
{
	long base = eqL + s - V92_EQ_PRE;
	float g = mu / (V92_EQ_F * pr + 1);
	int i;
	for (i = 0; i < V92_EQ_F; i++)
		f[i] += g * err * at(buf, base + i);
	for (i = 0; i < V92_EQ_B; i++)
		b[i] -= mu / V92_EQ_B * err * dh[i];
}

static void push(float *dh, float t)
{
	memmove(dh + 1, dh, (V92_EQ_B - 1) * sizeof(*dh));
	dh[0] = t;
}

/* the period 6 tones (Ru, Su): a sliding DFT of 12 samples at 1333 Hz,
   and their phase reversals in the gap between two of its windows */
#define TONE_W          12
#define TONE_GAP        6

void V92Up::tone_sample(float x)
{
	static const float c6[6] = { 1, 0.5f, -0.5f, -1, -0.5f, 0.5f };
	static const float s6[6] = { 0, -0.8660254f, -0.8660254f, 0,
				     0.8660254f, 0.8660254f };
	int k = (int)(n % 6), j, coh, h = (int)(n & 31);
	cplx bb = cplx_make(0, 0);
	float e = 0, d;

	th[n & 15] = x;
	for (j = 0; j < TONE_W; j++) {
		float v = th[(n - j) & 15];
		int kk = (k - j + 12) % 6;
		bb.re += v * c6[kk];
		bb.im += v * s6[kk];
		e += v * v;
	}
	tbh[h] = bb;
	/* |B|^2 is E W / 2 for a sine of period 6 */
	coh = e > TONE_MIN_E * 2 && cplx_pow(bb) > 0.6f * e * TONE_W / 2;
	tcoh[h] = (unsigned char)coh;
	if (coh) {
		tone_n++;
		tone_miss = 0;
	} else if (++tone_miss > 12)
		tone_n = 0;
	if (rev_hold > 0)
		rev_hold--;
	/* a reversal between the windows */
	d = 0;
	if (coh && tcoh[(n - TONE_W - TONE_GAP) & 31]) {
		cplx o = tbh[(n - TONE_W - TONE_GAP) & 31];
		d = (bb.re * o.re + bb.im * o.im) /
			sqrtf(cplx_pow(bb) * cplx_pow(o) + 1e-9f);
	}
	if (d < -0.7f && !rev_hold) {
		if (!rev_n || d < rev_min) {
			rev_min = d;
			rev_cand = n - TONE_W - TONE_GAP / 2 + 1;
		}
		rev_n++;
		return;
	}
	if (!rev_n)
		return;
	/* the most reversed one */
	rev_n = 0;
	rev_hold = 12;
	rev_at = rev_cand;
	switch (rx) {
	case V92R_RU:
		if (tone_n < 150)
			break;
		DSP_DBG("v92: Ru reversal at %ld\n", rev_at);
		if (ia.md && !md_done) {
			/* MD, then Ru again */
			md_done = 1;
			rx = V92R_MD;
			t0 = rev_at + 24 + ia.md * 276L;
			break;
		}
		t0 = rev_at + 24;
		rx = V92R_TRN1;
		break;
	case V92R_SU:
		revs = 1;
		rx = V92R_SUBAR;
		DSP_DBG("v92: Su reversal at %ld\n", rev_at);
		break;
	case V92R_SUBAR:
		revs = 2;
		rx = V92R_SU2;
		DSP_DBG("v92: Su-bar reversal at %ld\n", rev_at);
		break;
	case V92R_SU2:
		revs = 3;
		rx = V92R_TRN1B;
		t0 = rev_at + 24;
		DSP_DBG("v92: Su reversal after Jp at %ld\n", rev_at);
		break;
	default:
		break;
	}
}

void V92Up::rx_sample(float x)
{
	buf[n & (V92_BUF - 1)] = x;
	pr += 0.002f * (x * x - pr);
	tone_sample(x);
	/* tone A: the analogue modem retrains (9.7.1.2) */
	ta_e += x * x;
	if (ta_g.run(x)) {
		float e = ta_e / 64;
		ta_e = 0;
		ta_blocks = e > TONE_MIN_E / 6 && ta_g.power > 0.6f * e ?
			ta_blocks + 1 : 0;
	}
	switch (rx) {
	case V92R_MD:
		if (n >= t0) {
			tone_n = rev_n = 0;
			rx = V92R_RU;
		}
		break;
	case V92R_TRN1:
		if (!trained && n >= t0 + 64 + TRN_NT + V92_EQ_F + 8) {
			if (train(t0, -16, 64))
				rx = V92R_QUIET;
			else
				rx = V92R_JA;
		}
		break;
	case V92R_QUIET:
		if (tone_n >= 96) {
			DSP_DBG("v92: Su at %ld\n", n);
			rx = V92R_SU;
		}
		break;
	case V92R_TRN1B:
		/* Su-bar may last until our Jp' is heard */
		if (!trained && n >= t0 + 1200 + TRN_NT + V92_EQ_F + 8) {
			if (train(t0, -16, 1200))
				rx = V92R_QUIET;
			else {
				rx = V92R_CPT;
				fr.init(V92F_CP);
				frame_zero = 0;
			}
		}
		break;
	default:
		break;
	}
	if (trained)
		while (eqL + sym + V92_EQ_F - V92_EQ_PRE - 1 <= n) {
			symbol(equalize(sym));
			sym++;
			if (!trained)
				break;
		}
}

/* a bit of 2 point signals: differential, scrambled */
void V92Up::bit2(int d)
{
	int c = d ^ last_d;
	last_d = d;
	frame_bits(gpa_descr(&dscr, c));
}

/* the frames in the decoded bits */
void V92Up::frame_bits(int bit)
{
	int r = fr.bit(bit);

	if (!bit)
		frame_zero++;
	if (r < 0)
		DSP_DBG("v92: frame with a bad CRC (%d bits) at symbol %ld\n",
			fr.len, sym);
	if (r <= 0)
		return;
	if (rx == V92R_JA) {
		int beta = dil_beta(fr.b), nn = field(fr.b, 18, 8);
		int o = 188 + beta + (nn + 1) / 2 * 17;
		if (dil.parse(fr.b, fr.len))
			return;
		up_mask = field(fr.b, o, 16) | field(fr.b, o + 17, 3) << 16;
		got_ja = 1;
		DSP_DBG("v92: Ja: DIL N %d, LSP %d, LTP %d, rates %05x\n",
			dil.n, dil.lsp, dil.ltp, up_mask);
		/* the analogue modem stops Ja when it hears Sd */
		trained = 0;
		rx = V92R_QUIET;
		tone_n = rev_n = 0;
		return;
	}
	if (rx == V92R_CPT && !fr.b[18] && !field(fr.b, 19, 2)) {
		if (!got_cpt) {
			if (v92_cp_parse(&cpt, fr.b, fr.len))
				return;
			got_cpt = 1;
			DSP_DBG("v92: CPt at symbol %ld: drn %d, %d "
				"constellations\n", sym, cpt.drn, cpt.ncon);
		}
		return;
	}
	if (rx == V92R_TRN2) {
		if (fr.b[18]) {
			/* SUVu */
			if (!got_suvu)
				DSP_DBG("v92: SUVu at symbol %ld\n", sym);
			got_suvu = 1;
			if (fr.b[33] && !got_ack) {
				got_ack = 1;
				DSP_DBG("v92: SUVu' at symbol %ld\n", sym);
			}
		} else if (field(fr.b, 19, 2) == 1) {
			V90Cp c;
			if (v92_cp_parse(&c, fr.b, fr.len))
				return;
			if (!got_cpu)
				DSP_DBG("v92: CPu at symbol %ld: drn %d, Sr %d, "
					"ld %d, %d constellations\n", sym,
					c.drn, c.sr, c.ld, c.ncon);
			cpu = c;
			got_cpu = 1;
			if (c.ack && !got_ack) {
				got_ack = 1;
				DSP_DBG("v92: CPu' at symbol %ld\n", sym);
			}
		} else if (fr.b[33] && !got_ack) {
			got_ack = 1;
			DSP_DBG("v92: CPus' at symbol %ld\n", sym);
		}
	}
}

/* the 4 points of TRN2u (Table 28): sign, differential, and magnitude;
   which of the two bits goes first is found on TRN2u's ones */
void V92Up::bits4(int s, int m)
{
	int k, c = s ^ last_d, a, bb;

	last_d = s;
	for (k = 0; k < 2; k++) {
		a = k ? m : c;
		bb = k ? c : m;
		a = gpa_descr(&dscr2[k], a);
		bb = gpa_descr(&dscr2[k], bb);
		ones2[k] += (a & bb) ? 1 : -1;
		if (k == order) {
			frame_bits(a);
			frame_bits(bb);
		}
	}
}

void V92Up::symbol(float y)
{
	float t, err;
	int d, i = (int)(sym % 12);

	switch (rx) {
	case V92R_JA:
	case V92R_CPT:
		d = y < 0;
		t = d ? -1.0f : 1.0f;
		err = t - y;
		mse += 0.005 * (err * err - mse);
		adapt(sym, err, 0.002f);
		push(dh, t);
		if (i == 0)
			frame_zero = 0;
		bit2(d);
		if (rx == V92R_CPT && i == 11) {
			/* E1u: a data frame of zeros after CPt */
			if (got_cpt && frame_zero == 12 && !fr.n) {
				got_e1u = 1;
				DSP_DBG("v92: E1u at symbol %ld, mse %.4f\n",
					sym, mse);
				rx = V92R_TRN2;
				e2u_sym = sym;
				fr.init(V92F_CP);
				dscr2[0] = dscr2[1] = 0;
				ones2[0] = ones2[1] = 0;
				order = 0;
			}
		}
		if ((sym & 1023) == 0)
			DSP_TRACE("v92: symbol %ld mse %.4f\n", sym, mse);
		break;
	case V92R_TRN2: {
		const float l1 = 0.4472136f, l3 = 1.3416408f;
		int s = y < 0, m = fabsf(y) > 2 * l1;
		t = (s ? -1 : 1) * (m ? l3 : l1);
		err = t - y;
		mse += 0.005 * (err * err - mse);
		adapt(sym, err, 0.002f);
		push(dh, t);
		if (i == 0)
			frame_zero = 0;
		bits4(s, m);
		if (sym - e2u_sym == 240) {
			order = ones2[1] > ones2[0];
			DSP_DBG("v92: TRN2u, %s bit first (%ld %ld), mse "
				"%.4f\n", order ? "magnitude" : "sign",
				ones2[0], ones2[1], mse);
		}
		/* E2u: a data frame of zeros, once the analogue modem has
		   acknowledged our CPd */
		if (i == 11 && got_ack && frame_zero == 24 && !fr.n) {
			got_e2u = 1;
			DSP_DBG("v92: E2u at symbol %ld\n", sym);
			rx = V92R_DATA;
			dscr = 0;
			dprev = 0;
			dframes = derrs = 0;
			dmse = 0;
		}
		break;
	}
	case V92R_DATA:
		data_symbol(y);
		break;
	default:
		break;
	}
}

/*
 *    the data mode receiver
 */

/* the modulus encoder's numbers: up to 12 * 8 bits */
struct Big {
	unsigned w[4];          /* least significant first */

	void set(unsigned v) { memset(w, 0, sizeof(w)); w[0] = v; }
	void muladd(unsigned m, unsigned a)
	{
		unsigned long long c = a;
		for (int i = 0; i < 4; i++) {
			c += (unsigned long long)w[i] * m;
			w[i] = (unsigned)c;
			c >>= 32;
		}
	}
	/* this = a - this */
	void rsub(const Big &a)
	{
		long long br = 0;
		for (int i = 0; i < 4; i++) {
			long long d = (long long)a.w[i] - w[i] - br;
			br = d < 0;
			w[i] = (unsigned)d;
		}
	}
	int cmp(const Big &a) const
	{
		for (int i = 3; i >= 0; i--)
			if (w[i] != a.w[i])
				return w[i] < a.w[i] ? -1 : 1;
		return 0;
	}
	int bit(int k) const { return (w[k >> 5] >> (k & 31)) & 1; }
};

void V92Up::data_symbol(float y)
{
	int i = (int)(sym % 12), c = cpd.con[i % 6], lc = cpd.lc[c];
	float u = (float)(y / cpd.g), best = 1e30f, t, err;
	int eta = 0, e, K, M = cpd.M[i];

	/* the nearest point */
	for (e = -lc; e < lc; e++) {
		float a = e >= 0 ? cpd.pts[c][e] : -cpd.pts[c][-1 - e];
		float dd = (u - a) * (u - a);
		if (dd < best) {
			best = dd;
			eta = e;
		}
	}
	t = (float)(cpd.g * (eta >= 0 ? cpd.pts[c][eta] :
			     -cpd.pts[c][-1 - eta]));
	err = t - y;
	dmse += 0.005 * (err * err - dmse);
	/* decisions near the middle between two points */
	if (fabsf(err) > 0.6f * 16 * cpd.g)
		dfar++;
	adapt(sym, err, 0.0005f);
	push(dh, t);
	/* the class: eta = K + zM, and on the 4th symbol of a trellis
	   frame 2K + 2zM + parity */
	if (i % 4 == 3)
		K = ((eta - (eta & 1)) / 2) % M;
	else
		K = eta % M;
	if (K < 0)
		K += M;
	dk[i] = K;
	if (i == 11) {
		Big R, Mt, R2;
		int j, nb = 2 * (cpd.drn + 17), sgn;
		unsigned char bits[80];
		R.set(0);
		Mt.set(1);
		for (j = 11; j >= 0; j--)
			R.muladd(cpd.M[j], dk[j]);
		for (j = 0; j < 12; j++)
			Mt.muladd(cpd.M[j], 0);
		/* Mt - 1 */
		R2.set(1);
		R2.rsub(Mt);
		if (dprev)
			R.rsub(R2);
		/* R > (Mt - 1) / 2, as 2R > Mt - 1 */
		{
			Big t = R;
			t.muladd(2, 0);
			sgn = t.cmp(R2) > 0;
		}
		dprev ^= sgn;
		for (j = 0; j < nb; j++)
			bits[j] = (unsigned char)gpa_descr(&dscr, R.bit(j));
		dframes++;
		if (dframes <= 48) {
			/* B1u: ones */
			int z = 0;
			for (j = 0; j < nb; j++)
				z += !bits[j];
			derrs += z;
			if (dframes == 48) {
				DSP_DBG("v92: B1u: %ld zeros in %d bits, mse "
					"%.4f\n", derrs, 48 * nb, dmse);
				b1_bad = derrs * 20 > 48 * nb;
			}
		} else
			put_bits(ctx, bits, nb);
		if ((dframes & 1023) == 0) {
			DSP_DBG("v92: data frame %ld, mse %.4f, %ld uncertain"
				"\n", dframes, dmse, dfar);
			dfar = 0;
		}
	}
}

/*
 *    transmitter
 */

/* our CPd: the fastest rate whose uniform constellation the mse of
   TRN1u allows, of those the analogue modem has: the half distance
   between points at least sqrt(V92_MARGIN) times the rms error (the
   errors have longer tails than a normal distribution's: 1 % of the
   decisions are 0.6 of the half distance off, at 5.7 times the rms) */
#define V92_MARGIN      40
void V92Up::design()
{
	int drn, M = 8, i, j;
	double msa = 0;
	const char *fix = getenv("DSP_V92_UP_DRN");

	for (drn = 19; drn >= 1; drn--) {
		int K = 2 * (drn + 17), k;
		double ms3 = 0, ms0 = 0;
		if (up_mask && !((up_mask >> (drn - 1)) & 1))
			continue;
		M = (int)ceil(pow(2.0, K / 12.0) - 1e-9);
		/* least power of a class (points 2j + 1), and all 2M
		   points on the trellis frame's 4th symbol */
		for (k = 0; k < M; k++) {
			int a = 2 * k + 1, bb = 2 * (M - k) - 1;
			ms0 += (a < bb ? a * a : bb * bb);
		}
		ms0 /= M;
		ms3 = (4.0 * M * M - 1) / 3;
		msa = (9 * ms0 + 3 * ms3) / 12;
		if (fix ? drn == atoi(fix) : msa * mse_trn * V92_MARGIN <= 1)
			break;
	}
	if (drn < 1) {
		drn = 1;
		M = 8;
		msa = 37;
	}
	memset(&cpd, 0, sizeof(cpd));
	cpd.drn = drn;
	for (i = 0; i < 12; i++)
		cpd.M[i] = M;
	/* 4M points: the original library finds no point of a class on
	   the trellis frame's 4th symbol in 2M (it sends 0) */
	cpd.ncon = 1;
	cpd.lc[0] = 2 * M;
	for (j = 0; j < 2 * M; j++)
		cpd.pts[0][j] = (2 * j + 1) * 16;
	cpd.g = 1 / (16 * sqrt(msa));
	DSP_DBG("v92: CPd: %d bit/s, M %d, G %.5f (mse %.5f)\n",
		(drn + 17) * 8000 / 6, M, cpd.g, mse_trn);
}

void V92Up::make_seq(int cpd_, int ack)
{
	if (cpd_) {
		if (!cpd.drn)
			design();
		cpd.ack = ack;
		seq_len = cpd.build(seq, D);
		cpd_sent++;
		cpd_at = 0;
	} else
		seq_len = v92_suv_build(seq, ack, D);
	seq_pos = 0;
	seq_ack = ack;
	seq_cpd = cpd_;
}

/* the frames of D bits the encoder takes */
static void v92_get_frame(void *ctx, V90Tx *t, unsigned char *bits,
			  int *last)
{
	V92Up *v = static_cast<V92Up*>(ctx);

	if (v->fmode == V92F_B1 && v->fcount == 0) {
		v->D = v->cpu.drn + 20;
		t->enc.init(&v->cpu, v->D, t->lin);
	}
	v->fcount++;
	switch (v->fmode) {
	case V92F_TRN:
		memset(bits, 1, v->D);
		/* TRN2d: 2160 symbols */
		if (v->fcount >= 360) {
			v->fmode = V92F_SEQ;
			v->make_seq(0, v->got_cpu);
		}
		break;
	case V92F_SEQ:
		memcpy(bits, v->seq + v->seq_pos, v->D);
		v->seq_pos += v->D;
		v->cpd_at++;
		if (v->seq_pos < v->seq_len)
			break;
		if (v->seq_ack)
			v->ack_sent = 1;
		if (v->ack_sent && (v->got_ack || v->got_e2u)) {
			DSP_DBG("v92: Ed\n");
			v->fmode = V92F_ED;
			v->fcount = 0;
		} else if (v->got_suvu &&
			   (!v->cpd_sent || (!v->got_ack && v->cpd_at > 400)))
			v->make_seq(1, v->got_cpu);
		else
			v->make_seq(0, v->got_cpu);
		break;
	case V92F_ED:
		memset(bits, 0, v->D);
		if (v->fcount >= 2) {
			*last = 1;
			v->fmode = V92F_B1;
			v->fcount = 0;
		}
		break;
	case V92F_B1:
		memset(bits, 1, v->D);
		if (v->fcount >= 48)
			v->fmode = V92F_DATA;
		break;
	case V92F_DATA:
		memset(bits, 1, v->D);
		v->get_bits(v->ctx, bits, v->D);
		break;
	}
}

int V92Up::tx_step()
{
	if (ta_blocks >= 10) {
		DSP_DBG("v92: tone A\n");
		return V92_RETRAIN;
	}
	switch (tstep) {
	case 0:
		if (got_ja) {
			tstep = 1;
			tstep_at = n;
		}
		break;
	case 1:
		/* the analogue modem stops Ja when it hears Sd */
		if (n - tstep_at >= 400) {
			tx.dil = dil;
			tx.trn1d_len = 8000;
			tx.segment(V90_SD);
			tstep = 2;
		}
		break;
	case 2:
		/* Su, Su-bar, Su: Jp */
		if (revs >= 2) {
			tx.end_jd();
			tstep = 3;
		}
		break;
	case 3:
		/* Su-bar: Jp', DIL or SCR */
		if (revs >= 3) {
			tx.end_jd();
			tstep = 4;
		}
		break;
	case 4:
		if (got_cpt) {
			tx.end_dil();
			tstep = 5;
		}
		break;
	case 5:
		if (tx.next) {
			D = cpt.drn + 8;
			if (tx.enc.init(&cpt, D, tx.lin)) {
				DSP_DBG("v92: CPt does not fit\n");
				return V92_FAIL;
			}
			tx.get_frame = v92_get_frame;
			tx.ctx = this;
			fmode = V92F_TRN;
			fcount = 0;
			tx.segment(V90_RI);
			tstep = 6;
		}
		break;
	case 6:
		if (got_e1u) {
			tx.end_ri();
			tstep = 7;
		}
		break;
	case 7:
		if (b1_bad) {
			DSP_DBG("v92: B1u does not come through\n");
			return V92_FAIL;
		}
		if (fmode == V92F_DATA && rx == V92R_DATA && dframes > 48 &&
		    !connected) {
			connected = 1;
			DSP_DBG("v92: connect %d/%d\n",
				(cpu.drn + 20) * 8000 / 6,
				(cpd.drn + 17) * 8000 / 6);
			return V92_CONNECT;
		}
		break;
	}
	if (!connected && n > 8000L * 40) {
		DSP_DBG("v92: start-up timeout (tx %d, rx %d)\n", tstep, rx);
		return V92_FAIL;
	}
	return V92_NONE;
}

void V92Up::init(int alaw_, const V92Info1a *ia_, const long *fclock,
		 long fbase)
{
	memset(this, 0, sizeof(*this));
	trn_ref_init();
	alaw = alaw_;
	ia = *ia_;
	tx.init(alaw_, ia.uinfo);
	tx.v92 = 1;
	/* back to the phase of the first TRN1u after Su-bar's half
	   symbol */
	tx.jp_frac = 0x8000;
	tx.fclock = fclock;
	tx.fbase = fbase;
	rx = V92R_RU;
	fr.init(V92F_DIL);
	ta_g.init(2400, 64, 8000);
	DSP_DBG("v92: PCM upstream: filter sections %d, coefficients %d "
		"(%d each), MD %d, UINFO %d\n", ia.sections, ia.ltot, ia.lmax,
		ia.md, ia.uinfo);
}

int V92Up::sample(int x, int *y)
{
	int ev;
	rx_sample((float)x);
	ev = tx_step();
	*y = tx.sample();
	n++;
	return ev;
}

} // namespace dsp
