/*
 *
 *    v92.cpp - ITU-T V.92 (11/2000) PCM upstream, the digital modem
 *
 *    After an INFO1a that asks for PCM upstream (Table 18) the two modems
 *    go through phase 3 (9.5) and phase 4 (9.6) of V.92; this runs them
 *    on the network's 8000 Hz samples, as V.91 does.
 *
 *    The analogue modem's signals are PAM symbols, one per sample.  The
 *    receiver finds Ru and Su by their period of 6 symbols (1333 Hz; Su
 *    may also have Sd's shape, mostly 4000 Hz) and their reversals, and
 *    the two TRN1u by their known scrambled sequence: the correlation with
 *    it is the channel's impulse response, and gives the symbol (and,
 *    for the second TRN1u, the data frame) alignment.  A decision
 *    feedback equalizer is fitted to TRN1u by least squares and follows
 *    the signal from then on by its decisions.
 *
 *    Our CPd hands the equalizer's feedback taps to the analogue modem's
 *    precoder: it picks, of the points of a class, the one that keeps its
 *    output least (a modulo precoder, so 1 / C(z) need not be stable),
 *    and the receiver is left with the feed forward taps and the points
 *    themselves.  A uniform modulus M on all 12 data frame intervals and
 *    one constellation of equally spaced points; the trellis code (64
 *    states) is decoded with a Viterbi decoder on the pairs of symbols.
 *
 *    The upstream rate comes from the fit's mean square error, with a
 *    margin (V92_MARGIN_CODED); a B1u that does not come through, or a
 *    failure before data, retrains to V.34 upstream.
 *
 *    The echo of our signal from the analogue modem's hybrid is the V.34
 *    pump's echo canceller's (dp_v34.cpp), trained while we send Sd to Jd
 *    and the analogue modem is silent.
 *
 */

#include <limits.h>
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
	put_bits(b, &n, trellis, 2);
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
	/* precoder and prefilter */
	for (i = 0; i < 4; i++) {
		put_bits(b, &n, 0, 1);
		put_bits(b, &n, len[i], 9);
		put_bits(b, &n, 0, 7);
	}
	for (i = 0; i < 4; i++)
		for (j = 0; j < len[i]; j++) {
			put_bits(b, &n, 0, 1);
			put_bits(b, &n, (unsigned short)coef[i][j], 16);
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

	if (pf_on) {
		if (s == pf_rst)
			pf_reset(s);
		if (s == pf_new_at && pf_new) {
			/* the design for the slip, from B1u on */
			pf_rx_take();
			pf_new = 0;
			DSP_DBG("v92: the new prefilter and precoder at symbol "
				"%ld\n", s);
		}
		/* the point: the level at the delay, through the few taps
		   that follow the timing */
		for (i = 0; i < V92_PF_F; i++)
			y += pf_f[i] * pf_sample(s + pf_d + i - V92_PF_F / 2);
		return (float)y;
	}
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

	if (pf_on) {
		g = mu / (V92_PF_F * pr + 1);
		for (i = 0; i < V92_PF_F; i++)
			pf_f[i] += g * err *
				pf_sample(s + pf_d + i - V92_PF_F / 2);
		return;
	}
	for (i = 0; i < V92_EQ_F; i++)
		f[i] += g * err * at(buf, base + i);
	if (!b_frozen)
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
	float e = 0, r = 0, d;

	th[n & 31] = x;
	for (j = 0; j < TONE_W; j++) {
		float v = th[(n - j) & 31], u = v - th[(n - j - 6) & 31];
		int kk = (k - j + 12) % 6;
		bb.re += v * c6[kk];
		bb.im += v * s6[kk];
		e += v * v;
		r += u * u;
	}
	tbh[h] = bb;
	/* |B|^2 is E W / 2 for a sine of period 6.  Su may be Sd's shape
	   instead, its energy mostly at 4000 Hz (the HSF modem's Su,
	   Su-bar and the start of the Su after it, before it goes on at a
	   lower level with a sine): while we send Jd (when Su comes), any
	   signal of period 6 with some of its energy at 1333 Hz is one */
	coh = e > TONE_MIN_E * 2 && (cplx_pow(bb) > 0.6f * e * TONE_W / 2 ||
		(tx.seg == V90_JD && r < 0.02f * e &&
		 cplx_pow(bb) > 0.08f * e * TONE_W / 2));
	tcoh[h] = (unsigned char)coh;
	if (coh) {
		tone_n++;
		tone_miss = 0;
	} else if (++tone_miss > 12)
		tone_n = 0;
	if (rev_hold > 0)
		rev_hold--;
	/* Su, Su-bar, Su: the HSF modem's Su-bar is 24 symbols, too short
	   for the windows and the gap between them (its two reversals would
	   be one); each period against Su's phase instead */
	if (rx == V92R_SU || rx == V92R_SUBAR || rx == V92R_SU2) {
		cplx b6 = cplx_make(0, 0);
		float e6 = 0;
		for (j = 0; j < 6; j++) {
			float v = th[(n - j) & 31];
			int kk = (k - j + 12) % 6;
			b6.re += v * c6[kk];
			b6.im += v * s6[kk];
			e6 += v * v;
		}
		d = e6 > TONE_MIN_E && cplx_pow(b6) > 0.04f * e6 * 3 ?
			(b6.re * su_ref.re + b6.im * su_ref.im) /
			sqrtf(cplx_pow(b6) * cplx_pow(su_ref) + 1e-9f) : 0;
		if (rx == V92R_SUBAR)
			su_back = d > 0.5f ? su_back + 1 : 0;
		else
			su_back = d < -0.7f ? su_back + 1 : 0;
		if (rx == V92R_SU && su_back >= 6) {
			revs = 1;
			rx = V92R_SUBAR;
			rev_at = n - 8;
			DSP_DBG("v92: Su reversal at %ld\n", rev_at);
		} else if (rx == V92R_SUBAR && su_back >= TONE_W) {
			revs = 2;
			rx = V92R_SU2;
			rev_at = n - TONE_W - 2;
			/* the Su after it may be at another phase */
			su_ref = b6;
			DSP_DBG("v92: Su-bar reversal at %ld\n", rev_at);
		} else if (rx == V92R_SU2 && su_back >= 6) {
			/* after our Jp: Su-bar of 24 symbols, then TRN1u */
			revs = 3;
			rx = V92R_TRN1B;
			rev_at = n - 8;
			t0 = rev_at + 24;
			DSP_DBG("v92: Su reversal after Jp at %ld\n", rev_at);
		} else
			return;
		/* the windows' view of them is not another */
		su_back = 0;
		rev_n = 0;
		rev_hold = 48;
		return;
	}
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
	case V92R_RN_RU:
		if (tone_n < 150)
			break;
		{
			/* Ru starts on a data frame boundary, Ru-bar 384
			   symbols later, TRN2u 24 after it */
			long r = rev_at - eqL;
			rn_trn = (r + 6) / 12 * 12 + 24;
			rx = V92R_RN_WAIT;
			DSP_DBG("v92: Ru-bar at symbol %ld, TRN2u at %ld\n", r,
				rn_trn);
		}
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
		/* (the analogue modem sends Su once it has heard our Jd:
		   before, a tone is the echo of our own Sd) */
		if (tone_n >= 96 &&
		    !(tx.seg >= V90_SD && tx.seg <= V90_TRN1D)) {
			DSP_DBG("v92: Su at %ld\n", n);
			su_ref = tbh[n & 31];
			su_back = 0;
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
	/* the analogue modem's Ru in data mode: a rate renegotiation */
	if (rx == V92R_DATA && tone_n >= 150)
		rn_rx_start();
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
	if (rx == V92R_TRN2 || fpe_rx == 3) {
		if (fr.b[18]) {
			/* SUVu */
			if (!got_suvu)
				DSP_DBG("v92: SUVu at symbol %ld\n", sym);
			got_suvu = 1;
			if (fr.b[32] && rn && !rn_silence &&
			    !rn_wait_suv2) {
				rn_silence = 1;
				DSP_DBG("v92: SUVu asks for silence\n");
			}
			if (!fr.b[32] && rn_wait_suv2 && !rn_suv2) {
				rn_suv2 = 1;
				DSP_DBG("v92: SUVu after the silence at "
					"symbol %ld\n", sym);
			}
			if (fr.b[33] && !got_ack) {
				got_ack = 1;
				DSP_DBG("v92: SUVu' at symbol %ld\n", sym);
			}
		} else if (field(fr.b, 19, 2) == 1) {
			V90Cp c;
			if (v92_cp_parse(&c, fr.b, fr.len))
				return;
			if (!got_cpu || c.drn != cpu.drn ||
			    memcmp(c.mask, cpu.mask, sizeof(c.mask))) {
				int pts = 0;
				for (int k = 0; k < 128; k++)
					pts += c.mask[0][k];
				DSP_DBG("v92: CPu at symbol %ld: drn %d, Sr %d, "
					"ld %d, %d constellations (%d "
					"points)\n", sym, c.drn, c.sr, c.ld,
					c.ncon, pts);
			}
			cpu = c;
			got_cpu = 1;
			if (c.ack && !got_ack) {
				got_ack = 1;
				DSP_DBG("v92: CPu' at symbol %ld\n", sym);
			}
		} else if (field(fr.b, 19, 2) == 2) {
			/* CPus: the digital modem's parameters as they are */
			if (!got_cpu)
				DSP_DBG("v92: CPus at symbol %ld\n", sym);
			got_cpu = 1;
			if (fr.b[33] && !got_ack) {
				got_ack = 1;
				DSP_DBG("v92: CPus' at symbol %ld\n", sym);
			}
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
		dring[sym & (V92_DR - 1)] = t;
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
		dring[sym & (V92_DR - 1)] = t;
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
		if (rn_wait_suv2) {
			/* in the silent period: until SUVu with bit 32
			   clear */
		} else if (i == 11 && got_ack && frame_zero == 24 && !fr.n &&
		    rn_silence) {
			/* the silent period: TRN2u, then SUVu with bit 32
			   clear (9.8.1.1.4) */
			got_e2u = 1;
			rn_wait_suv2 = 1;
			e2u_sym = sym;
			fr.init(V92F_CP);
			DSP_DBG("v92: E2u before the silence at symbol %ld\n",
				sym);
		} else if (i == 11 && got_ack && frame_zero == 24 && !fr.n) {
			got_e2u = 1;
			DSP_DBG("v92: E2u at symbol %ld\n", sym);
			rx = V92R_DATA;
			/* with the prefilter: the points at the delay, from
			   B1u on */
			if (pf) {
				int ahead = V92_EQ_F - V92_EQ_PRE - 1;
				pf_on = 1;
				pf_eqL = eqL;
				pf_s0 = sym + 1;
				/* the taps reach V92_PF_F / 2 past the delay */
				if (pf_d + V92_PF_F / 2 > ahead)
					eqL += pf_d + V92_PF_F / 2 - ahead;
				pf_rx_take();
				memset(pf_xr, 0, sizeof(pf_xr));
				pf_rst = -1;
				dmse_g = 0;
			}
			dscr = 0;
			dprev = 0;
			dframes = derrs = 0;
			dsn = ddn = 0;
			fpe_rx = fpe_sw = 0;
			dmse = 0;
			vi = 0;
			vt = vn = 0;
			for (int k = 0; k < 64; k++)
				vmetric[k] = k ? 1e30f : 0;
		}
		break;
	}
	case V92R_DATA:
		data_symbol(y);
		break;
	case V92R_RN_WAIT:
		if (sym >= rn_trn) {
			/* TRN2u: phase 4 again (with decision feedback: no
			   precoding until B1u) */
			DSP_DBG("v92: TRN2u (renegotiation) at symbol %ld\n",
				sym);
			rx = V92R_TRN2;
			rn_rxd = 1;
			e2u_sym = sym;
			fr.init(V92F_CP);
			dscr2[0] = dscr2[1] = 0;
			ones2[0] = ones2[1] = 0;
			got_suvu = got_cpu = got_ack = got_e2u = 0;
			frame_zero = 0;
			symbol(y);
			break;
		}
		/* fall through */
	case V92R_RN_RU:
		/* Ru and Ru-bar: 2 points, past the precoder */
		d = y < 0;
		t = d ? -1.0f : 1.0f;
		push(dh, t);
		dring[sym & (V92_DR - 1)] = t;
		last_d = d;
		break;
	default:
		break;
	}
}

/* the analogue modem's Ru in data mode (9.8.1.2/V.92) */
void V92Up::rn_rx_start()
{
	DSP_DBG("v92: Ru at symbol %ld: rate renegotiation\n", sym);
	rx = V92R_RN_RU;
	b_frozen = 0;
	if (pf_on) {
		pf_on = 0;
		eqL = pf_eqL;
	}
	if (!rn) {
		rn = 1;
		rn_at = n;
	}
	/* in the middle of a fast parameter exchange (the original library
	   gives ours up so when it cannot read our CPd): phase 4 again
	   instead, its CPd from TRN2u */
	if (fpe) {
		DSP_DBG("v92: the fast parameter exchange given up\n");
		fpe = fpe_rf = fpe_req = fpe_slip = fpe_tx_init = 0;
		fpe_rx = fpe_sw = fpe_early = 0;
		pf_new = 0;
	}
	tone_n = 0;
	rev_n = 0;
}

/* our Rd and phase 4 again: TRN2d, SUVd, CPd, Ed, B1d */
void V92Up::rn_tx_start()
{
	DSP_DBG("v92: Rd\n");
	tx.rd_req = 1;
	rn_sent = 1;
	if (!rn) {
		rn = 1;
		rn_at = n;
	}
	fmode = V92F_TRN;
	fcount = 0;
	rn_tx_init = 1;
	ack_sent = 0;
	cpd_sent = 0;
	cpd_new = 1;
	tstep = 8;
	/* the analogue modem's SUVu, CPu and E2u of this one are to come */
	if (rx == V92R_DATA || rx == V92R_RN_RU || rx == V92R_RN_WAIT)
		got_suvu = got_cpu = got_ack = got_e2u = 0;
	rn_rxd = 0;
}

/* a fast parameter exchange (9.9/V.92), ours or in answer to RM, RM':
   Rf, Rf-bar, then SUVd, CPd, SUVd', Ed and B1d in the data mode
   modulation, from cleared scrambler, signs and shaping filter */
void V92Up::fpe_tx_start()
{
	DSP_DBG("v92: Rf\n");
	tx.rf_req = 1;
	fpe = fpe_rf = 1;
	fpe_at = n;
	fpe_req = 0;
	rn_rxd = 0;
	if (!rn) {
		rn = 1;
		rn_at = n;
	}
	/* (should the design fail, its CPd has the coefficients we have) */
	if (fpe_slip)
		pf_slip_design();
	fpe_design(fpe_drn);
	fpe_drn = 0;
	fpe_tx_init = 1;
	fmode = V92F_SEQ;
	fcount = 0;
	ack_sent = cpd_sent = 0;
	if (fpe_rx < 3)
		got_suvu = got_cpu = got_ack = got_e2u = 0;
	make_seq(0, 0);
	tstep = 8;
}

/* the CPd of a fast parameter exchange: the precoder stays, the rate
   comes from the data mode mse (drn: that one, a test) */
void V92Up::fpe_design(int drn)
{
	/* (the signal has unit power, as in the training's fit) */
	double e2 = dmse, sb = 0, msa = 0;
	int d, M = 8, lc = 0, j, k;

	if (pf && fpe_slip) {
		/* the design for the slip: its prefilter and precoder at
		   the rate and points we have (a change of little more than
		   a percent: as many points, the power and the trellis
		   frame's classes as before) */
		cpd_fpe = cpd;
		for (j = 0; j < V92_PF_B; j++)
			cpd_fpe.coef[1][j] = pf_p1[j];
		for (j = 0; j < V92_PF_W; j++)
			cpd_fpe.coef[2][j] = pf_z2[j];
		cpd_fpe.g = pf_g;
		cpd_fpe.ack = 0;
		DSP_DBG("v92: FPE CPd: %d bit/s, the prefilter for the "
			"slip\n", (cpd.drn + 17) * 8000 / 6);
		return;
	}
	if (pf) {
		/* the prefilter and precoder stay: the next rate up, if the
		   design has it (or the one asked for) */
		d = drn ? drn : cpd.drn + 1;
		if (d > 19 || (up_mask && !((up_mask >> (d - 1)) & 1)) ||
		    !pf_rate(d))
			d = cpd.drn;
		/* the analogue modem sends SUVu', E2u and FB1u on the new
		   points already, with the old moduli: the points of a
		   lower rate (further apart) would take its signal beyond
		   its power, and the old moduli need as many as before (on
		   the trellis frame's 4th symbol it has no point for a
		   class beyond them). Not down, then (a renegotiation) */
		if (d < cpd.drn) {
			DSP_DBG("v92: FPE: with the prefilter not down: at "
				"%d bit/s\n", (cpd.drn + 17) * 8000 / 6);
			d = cpd.drn;
		}
		cpd_fpe = cpd;
		int gap = pf_cpd(&cpd_fpe, d);
		cpd_fpe.ack = 0;
		DSP_DBG("v92: FPE CPd: %d bit/s, M %d, %d points (gap %d)\n",
			(d + 17) * 8000 / 6, cpd_fpe.M[0], 2 * cpd_fpe.lc[0],
			gap);
		return;
	}

	if (b_frozen)
		for (j = 0; j < cpd.len[1]; j++)
			sb += fabs(cpd.coef[1][j]) / 16384.0;
	for (d = 19; d >= 1; d--) {
		int K = 2 * (d + 17);
		double ms0 = 0, ms3;
		if (up_mask && !((up_mask >> (d - 1)) & 1))
			continue;
		M = (int)ceil(pow(2.0, K / 12.0) - 1e-9);
		if (b_frozen) {
			ms0 = (double)M * M / 3;
			ms3 = 4.0 * M * M / 3;
		} else {
			for (k = 0; k < M; k++) {
				int a = 2 * k + 1, bb = 2 * (M - k) - 1;
				ms0 += (a < bb ? a * a : bb * bb);
			}
			ms0 /= M;
			ms3 = (4.0 * M * M - 1) / 3;
		}
		msa = (9 * ms0 + 3 * ms3) / 12;
		lc = (int)ceil(M * (1 + sb)) + 2;
		if (lc < 2 * M)
			lc = 2 * M;
		lc += lc & 1;
		if (lc > V92_LC_MAX)
			continue;
		if (drn ? d == drn : msa * e2 * margin_used <= 1)
			break;
	}
	if (d < 1) {
		d = 1;
		M = 8;
		msa = 37;
		lc = 2 * M;
	}
	cpd_fpe = cpd;
	cpd_fpe.drn = d;
	for (j = 0; j < 12; j++)
		cpd_fpe.M[j] = M;
	cpd_fpe.lc[0] = lc;
	for (j = 0; j < lc; j++)
		cpd_fpe.pts[0][j] = (2 * j + 1) * 16;
	cpd_fpe.g = 1 / (16 * sqrt(msa));
	cpd_fpe.ack = 0;
	DSP_DBG("v92: FPE CPd: %d bit/s, M %d, %d points (data mse %.5f)\n",
		(d + 17) * 8000 / 6, M, 2 * lc, e2);
}

/* RM and RM' in data mode (Tables 25 and 26/V.92: the largest and the
   smallest class, in pairs), and the SUVu after them; 1 for a frame
   that carries no bits */
int V92Up::fpe_frame(const V92Cpd *c)
{
	static const char pat[11] = { 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0 };
	int j, rm = 0, rmp = 0, near = 0;

	/* 9 of the 11 classes: the largest and the smallest are next to
	   each other, a decision error turns one into the other (and data
	   has 9 of them in place 1 in M^9) */
	for (j = 0; j < 11; j++) {
		int t = pat[j] ? c->M[j] - 1 : 0,
			dd = (dk[j] - t + c->M[j] + 1) % c->M[j];
		rm += dk[j] == t;
		rmp += dk[j] == (pat[j] ? 0 : c->M[j] - 1);
		near += dd <= 2;
	}
	/* with the prefilter: RM's line signal is a large one of period
	   4, and the analogue modem's timing slip takes its points a whole
	   one off, the same way in every frame (data, at random, a little
	   each): all 11 within one of RM's classes (data has them 1 in
	   (M/3)^11, two frames running 1 in (M/3)^22) */
	rm = rm >= 9 || (pf_on && near == 11);
	rmp = rmp >= 9;
	switch (fpe_rx) {
	case 0:
		if (rx != V92R_DATA || dframes <= 48)
			return 0;
		fpe_rm = rm ? fpe_rm + 1 : 0;
		if (fpe_rm >= 2) {
			DSP_DBG("v92: RM at symbol %ld\n", sym);
			/* SUVu follows 384T of RM and 24T of RM'
			   (9.9.2.2.2/V.92), and the analogue modem starts
			   its transmitter anew for it */
			fpe_suv = ddn - 24 + 408;
			if (pf_on)
				pf_rst = sym - (dsn - 1 - fpe_suv);
			fpe_rx = 1;
			fpe = 1;
			if (!rn) {
				rn = 1;
				rn_at = n;
			}
		}
		return rm;
	case 1:
		/* with the prefilter RM' is lost in the long precoder's
		   change: by the time */
		if (pf_on) {
			if (ddn - 12 < fpe_suv)
				return 1;
			goto suvu;
		}
		if (rmp) {
			DSP_DBG("v92: RM' at symbol %ld\n", sym);
			fpe_rx = 2;
		}
		return 1;
	case 2:
		if (rmp)
			return 1;
	suvu:
		/* SUVu: the analogue modem's scrambler and differential
		   encoder start from zero */
		DSP_DBG("v92: FPE: SUVu from symbol %ld\n", sym);
		fpe_rx = 3;
		fpe_eo = fpe_en = 0;
		fpe_early = 0;
		dscr = 0;
		dprev = 0;
		fr.init(V92F_CP);
		got_suvu = got_cpu = got_ack = got_e2u = 0;
		frame_zero = 0;
		return 0;
	}
	return 0;
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

/* the nearest point of each 2D subset to (u, v), in grid units (the
   odd coordinates 2 eta + 1, up to lim) */
static void subset_pts(float u, float v, int lim, float dist[8], int pt[8])
{
	int x0 = 2 * (int)floorf(u / 2) + 1, y0 = 2 * (int)floorf(v / 2) + 1;
	int dx, dy, i;

	if (x0 > lim) x0 = lim;
	if (x0 < -lim) x0 = -lim;
	if (y0 > lim) y0 = lim;
	if (y0 < -lim) y0 = -lim;
	for (i = 0; i < 8; i++) {
		dist[i] = 1e30f;
		pt[i] = 0;
	}
	for (dx = -6; dx <= 6; dx += 2)
		for (dy = -6; dy <= 6; dy += 2) {
			int x = x0 + dx, y = y0 + dy, l;
			float dd;
			if (x < -lim || x > lim || y < -lim || y > lim)
				continue;
			l = v34_subset_label(x, y);
			dd = (u - x) * (u - x) + (v - y) * (v - y);
			if (dd < dist[l]) {
				dist[l] = dd;
				pt[l] = (x & 0xFFFF) | y << 16;
			}
		}
}

/* a trellis frame (tu[]) through the Viterbi decoder of the 16, 32 or 64
   state code (6.4.4/V.92: V.34's, with the 4D symbol of two pairs of
   symbols); the decided symbols after the decoding delay */
void V92Up::trellis_frame()
{
	float da[8], db[8], gm[32], m2[64], best;
	int ca[8], cb[8], ga[32], gb[32], l0, l1, g, st, y, t = vt, bs, k;
	int lim = 2 * cpd.lc[0] - 1;
	int states = 16 << cpd.trellis, ny = states == 16 ? 4 : 16;

	subset_pts(tu[0], tu[1], lim, da, ca);
	subset_pts(tu[2], tu[3], lim, db, cb);
	for (g = 0; g < 2 * ny; g++)
		gm[g] = 1e30f;
	for (l0 = 0; l0 < 8; l0++)
		for (l1 = 0; l1 < 8; l1++) {
			float m = da[l0] + db[l1];
			/* the encoder input (Y1 Y2, or Y1 to Y4), and Y0 */
			g = (v34_conv_in(l0, l1) & (ny - 1)) +
				((l0 ^ l1) & 1) * ny;
			if (m < gm[g]) {
				gm[g] = m;
				ga[g] = ca[l0];
				gb[g] = cb[l1];
			}
		}
	for (st = 0; st < states; st++)
		m2[st] = 1e30f;
	for (st = 0; st < states; st++) {
		int y0 = st & 1;
		if (vmetric[st] >= 1e29f)
			continue;
		for (y = 0; y < ny; y++) {
			int ns = v34_trellis_next(states, st, y);
			float m = vmetric[st] + gm[y + y0 * ny];
			if (m < m2[ns]) {
				m2[ns] = m;
				vprev[t][ns] = (unsigned char)st;
				vpa[t][ns] = ga[y + y0 * ny];
				vpb[t][ns] = gb[y + y0 * ny];
			}
		}
	}
	best = m2[0];
	bs = 0;
	for (st = 1; st < states; st++)
		if (m2[st] < best) {
			best = m2[st];
			bs = st;
		}
	for (st = 0; st < states; st++)
		vmetric[st] = m2[st] - best;
	vt = (t + 1) % V92_VT;
	if (++vn < V92_VT)
		return;
	st = bs;
	for (k = 0; k < V92_VT - 1; k++) {
		st = vprev[t][st];
		t = (t + V92_VT - 1) % V92_VT;
	}
	/* the odd coordinates back to indices */
	decided(((short)(vpa[t][st] & 0xFFFF) - 1) / 2);
	decided(((vpa[t][st] >> 16) - 1) / 2);
	decided(((short)(vpb[t][st] & 0xFFFF) - 1) / 2);
	decided(((vpb[t][st] >> 16) - 1) / 2);
}

void V92Up::data_symbol(float y)
{
	/* a fast parameter exchange: B1u on the new CPd (with the
	   prefilter on it already, and the analogue modem's transmitter
	   starts anew) */
	if (fpe_rx == 4 && dsn == fpe_b1) {
		if (!fpe_early) {
			cpd_old = cpd;
			cpd = cpd_fpe;
			fpe_sw = 1;
		}
		DSP_DBG("v92: FPE: B1u at symbol %ld\n", sym);
	}
	dsn++;
	int i = (int)(sym % 12), lc = cpd.lc[0];
	/* in grid units: a point is (2 eta + 1) * 16 * G, or with the
	   prefilter the G.711 levels, at 2 eta + 1 and linear between */
	float u = pf_on ? pf_grid(&cpd, y) : (float)(y / (16 * cpd.g)), t,
		err, eg;
	int eta = (int)floorf(u / 2);

	/* the nearest point (for the equalizer, and without trellis
	   decoding the decision) */
	if (eta < -lc)
		eta = -lc;
	if (eta > lc - 1)
		eta = lc - 1;
	t = pf_on ? pf_point(&cpd, eta) : (float)((2 * eta + 1) * 16 * cpd.g);
	err = t - y;
	eg = u - (2 * eta + 1);
	dmse += 0.005 * (err * err - dmse);
	dmse_g += 0.005 * (eg * eg - dmse_g);
	/* with the prefilter: has the analogue modem taken up our new
	   CPd? (the grid mse on it, against the old one's) */
	if (pf_on && fpe_rx == 3 && cpd_sent && !fpe_early) {
		float un = pf_grid(&cpd_fpe, y);
		int en = (int)floorf(un / 2);
		if (en < -cpd_fpe.lc[0])
			en = -cpd_fpe.lc[0];
		if (en > cpd_fpe.lc[0] - 1)
			en = cpd_fpe.lc[0] - 1;
		un -= 2 * en + 1;
		fpe_eo += 0.05 * (eg * eg - fpe_eo);
		fpe_en += 0.05 * (un * un - fpe_en);
		if (fpe_en < 0.05 && fpe_eo > 4 * fpe_en) {
			DSP_DBG("v92: FPE: the analogue modem on the new CPd "
				"at symbol %ld (grid mse %.3f, %.3f on the "
				"old)\n", sym, fpe_en, fpe_eo);
			/* its points, that is: the frames keep the old
			   moduli until B1u (its transmitter takes up the
			   rest of the CPd there) */
			fpe_early = 1;
			cpd_old = cpd;
			cpd = cpd_fpe;
			fpe_sw = 1;
			fpe_b1 = LONG_MAX;
		}
	}
	/* decisions near the middle between two points */
	if (fabsf(eg) > 0.6f)
		dfar++;
	/* with the prefilter: the analogue modem's timing slips (the
	   residual's fit has it), and the grid mse (over a second) is up
	   by 0.02 on what it was 3 s into the design: a design for the slip
	   in a fast parameter exchange at the same rate, well before the
	   decisions suffer (DSP_V92_SLIP=0: none, the renegotiation below
	   once they do) */
	if (pf_on) {
		sl_gm += (eg * eg - sl_gm) / 8000;
		if (slip_ok && !fpe_none && connected && !rn && !fpe_req &&
		    !pf_new && sl_base > 0 && sl_gm > sl_base + 0.02 &&
		    sl_n % 400 == 0) {
			double g, t, x;
			if (!pf_slip_fit(&g, &t, &x) &&
			    fabs(t) >= V92_SLIP_MIN && x >= 0.1) {
				DSP_DBG("v92: slip %+.5f (gain %+.4f, %.0f %% "
					"of the residual), grid mse %.4f against "
					"%.4f: fast parameter exchange\n", t, g,
					100 * x, sl_gm, sl_base);
				fpe_slip = fpe_req = 1;
			}
		}
	}
	/* the margin against the one our rate was chosen with: half of it
	   for 0.5 s, renegotiate down; 2.5 times for 10 s (and a minute
	   since the last), up (9.8/V.92) */
	if (connected && !rn && dframes > 48) {
		double q = pf_on ? 1 / (dmse_g + 1e-9) :
			256 * cpd.g * cpd.g / (dmse + 1e-9);
		/* with the prefilter the grid mse creeps up (the analogue
		   modem's transmitter slips against the network's clock,
		   and the network's codec no longer gets its levels), a
		   while without errors: down (a new design for the timing
		   as it is now) only when that reaches decisions (rms 0.3
		   of the half distance) */
		double lo = margin_used / 2;
		if (pf_on && lo > 1 / 0.09)
			lo = 1 / 0.09;
		rn_low = q < lo ? rn_low + 1 : 0;
		rn_high = q > margin_used * 2.5 ? rn_high + 1 : 0;
		if (rn_low >= 4000 ||
		    (rn_high >= 80000 && n - rn_last >= 8000L * 60 &&
		     cpd.drn < 19)) {
			DSP_DBG("v92: margin %.1f against %.1f: %s\n", q,
				margin_used, rn_low ? "renegotiate down" :
				"fast parameter exchange up");
			/* down: the line changed, train anew; up: the
			   same line with less noise, the rate alone (with
			   the prefilter if its design has the next rate) */
			if (rn_low || fpe_none)
				rn_req = 1;
			else if (!pf_on || pf_rate(cpd.drn + 1))
				fpe_req = 1;
			rn_low = rn_high = 0;
		}
	}
	/* faster than in training: the analogue modem's signal wanders a
	   little (with A-law, slower steps let the mse double in 10 s) */
	adapt(sym, err, 0.002f);
	if (pf_on) {
		/* the precoder's output, from the decision (for a new
		   start of the analogue modem's transmitter) */
		double xh = t, lim = 4.0 * cpd.pts[0][cpd.lc[0] - 1];
		for (int k = 0; k < V92_PF_B && sym - 1 - k >= pf_s0; k++)
			xh -= pf_rb[k] * pf_xr[(sym - 1 - k) & (V92_PF_XR - 1)];
		pf_xr[sym & (V92_PF_XR - 1)] =
			xh > lim ? lim : xh < -lim ? -lim : xh;
		pf_slip_sample();
	}
	/* with precoding, the transmitter has taken the data symbols' own
	   interference away: only that of those before B1u is left */
	push(dh, b_frozen ? 0 : t);
	if (!trellis_on) {
		decided(eta);
		return;
	}
	tu[i % 4] = u;
	if (i % 4 == 3)
		trellis_frame();
}

/* a decided symbol, eta, of data frame interval vi */
void V92Up::decided(int eta)
{
	long idx = ddn++;
	if (fpe_sw && idx >= fpe_b1)
		fpe_sw = 0;
	/* until B1u of a fast parameter exchange: the CPd before it */
	const V92Cpd *c = fpe_sw ? &cpd_old : &cpd;
	int i = vi, M = c->M[i], K;

	vi = (vi + 1) % 12;
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
		int j, nb = 2 * (c->drn + 17), sgn;
		unsigned char bits[80];
		/* RM and RM' carry no bits */
		if (fpe_frame(c))
			return;
		R.set(0);
		Mt.set(1);
		for (j = 11; j >= 0; j--)
			R.muladd(c->M[j], dk[j]);
		for (j = 0; j < 12; j++)
			Mt.muladd(c->M[j], 0);
		/* Mt - 1 */
		R2.set(1);
		R2.rsub(Mt);
		if (dprev)
			R.rsub(R2);
		/* R > (Mt - 1) / 2, as 2R > Mt - 1 */
		{
			Big tt = R;
			tt.muladd(2, 0);
			sgn = tt.cmp(R2) > 0;
		}
		dprev ^= sgn;
		for (j = 0; j < nb; j++)
			bits[j] = (unsigned char)gpa_descr(&dscr, R.bit(j));
		if (fpe_rx == 3) {
			/* SUVu, CPu, SUVu', then E2u: a frame of zeros */
			int z = 0;
			for (j = 0; j < nb; j++) {
				frame_bits(bits[j]);
				z += !bits[j];
			}
			if (z == nb && got_ack) {
				fpe_rx = 4;
				fpe_fb = 0;
				derrs = 0;
				/* 48 frames of FB1u, then B1u (and with the
				   prefilter, the analogue modem's transmitter
				   anew) */
				fpe_b1 = idx + 1 + 48 * 12;
				if (pf_on) {
					pf_rst = sym - (dsn - 1 - fpe_b1);
					pf_new_at = pf_rst;
				}
				DSP_DBG("v92: FPE: E2u\n");
			}
			return;
		}
		if (fpe_rx == 4) {
			/* FB1u: ones, at the old parameters */
			for (j = 0; j < nb; j++)
				derrs += !bits[j];
			if (++fpe_fb == 48) {
				DSP_DBG("v92: FPE: FB1u, %ld zeros in %d bits\n",
					derrs, 48 * nb);
				fpe_rx = 5;
				dframes = derrs = 0;
				rn_rxd = 1;
			}
			return;
		}
		dframes++;
		if (dframes <= 48) {
			/* B1u: ones */
			int z = 0;
			for (j = 0; j < nb; j++)
				z += !bits[j];
			derrs += z;
			b1z[dframes - 1] = (unsigned char)z;
			if (dframes == 48) {
				char zs[200];
				int q, o = 0;
				for (q = 0; q < 48; q++)
					o += sprintf(zs + o, " %d", b1z[q]);
				DSP_DBG("v92: B1u zeros by frame:%s\n", zs);
				DSP_DBG("v92: B1u: %ld zeros in %d bits, mse "
					"%.4f\n", derrs, 48 * nb, dmse);
				/* from frame 4 on: after a renegotiation the
				   original library's precoder starts from
				   the data before it */
				for (j = 0, z = 0; j < 4; j++)
					z += b1z[j];
				b1_bad = (derrs - z) * 20 > 44 * nb;
				if (fpe_rx == 5)
					fpe_rx = 0;
			}
		} else
			put_bits(ctx, bits, nb);
		if ((dframes & 1023) == 0) {
			/* the feed forward taps' centre of gravity: the
			   analogue modem's symbol timing against ours */
			double c = 0, w = 0;
			for (j = 0; j < V92_EQ_F; j++) {
				c += j * f[j] * f[j];
				w += f[j] * f[j];
			}
			DSP_DBG("v92: data frame %ld, mse %.4f, %ld uncertain, "
				"taps at %.3f\n", dframes, dmse, dfar, c / w);
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
/* with precoding (no decision feedback in the receiver) and the trellis
   code decoded: on netsim's u-law line 36000 bit/s go without errors at
   an mse that asks for 9.3 (16 state code), 38667 make a few at 5.8 (half
   as many with the 64 state code) */
#define V92_MARGIN_CODED 8
/* the precoder from our feedback taps shrunk by gamma^j (the points the
   analogue modem needs reach M (1 + sum |b|), see design()); the feed
   forward taps fitted to the last decisions with that feedback, by least
   squares.  Returns -1 if there is no fit. */
#define V92_FIT         1536
int V92Up::precoder_fit(double gamma, double *bg, double *fg, double *e2)
{
	static double A[V92_EQ_F * V92_EQ_F];
	double v[V92_EQ_F], phi[V92_EQ_F], g = 1;
	int i, j;
	long s0 = sym - V92_FIT, sn;

	for (j = 0; j < V92_EQ_B; j++) {
		g *= gamma;
		bg[j] = b[j] * g;
	}
	memset(A, 0, sizeof(A));
	memset(v, 0, sizeof(v));
	for (sn = s0; sn < sym; sn++) {
		long base = eqL + sn - V92_EQ_PRE;
		double t = dring[sn & (V92_DR - 1)];
		for (j = 0; j < V92_EQ_B; j++)
			t += bg[j] * dring[(sn - 1 - j) & (V92_DR - 1)];
		for (i = 0; i < V92_EQ_F; i++)
			phi[i] = at(buf, base + i);
		for (i = 0; i < V92_EQ_F; i++) {
			for (j = 0; j < V92_EQ_F; j++)
				A[i * V92_EQ_F + j] += phi[i] * phi[j];
			v[i] += phi[i] * t;
		}
	}
	for (i = 0; i < V92_EQ_F; i++)
		A[i * V92_EQ_F + i] *= 1 + 1e-6;
	if (solve(A, v, fg, V92_EQ_F))
		return -1;
	*e2 = 0;
	for (sn = s0; sn < sym; sn++) {
		long base = eqL + sn - V92_EQ_PRE;
		double t = dring[sn & (V92_DR - 1)], y = 0;
		for (j = 0; j < V92_EQ_B; j++)
			t += bg[j] * dring[(sn - 1 - j) & (V92_DR - 1)];
		for (i = 0; i < V92_EQ_F; i++)
			y += fg[i] * at(buf, base + i);
		*e2 += (y - t) * (y - t);
	}
	*e2 /= V92_FIT;
	return 0;
}

void V92Up::design()
{
	static const double gammas[] = { 1, 0.97, 0.94, 0.9, 0.85, 0.8,
					 0.7, 0.6, 0.5 };
#define NG      (int)(sizeof(gammas) / sizeof(gammas[0]))
	double bgs[NG][V92_EQ_B], fgs[NG][V92_EQ_F], e2s[NG], sbs[NG];
	int ng = 0, drn, M = 8, i, j, lc = 0, best = -1;
	double msa = 0;
	const char *fix = getenv("DSP_V92_UP_DRN");
	char fixb[16];

	if (rn_drn) {
		snprintf(fixb, sizeof(fixb), "%d", rn_drn);
		fix = fixb;
		rn_drn = 0;
	}
	int pre = !(getenv("DSP_V92_PRECODE") &&
		    !atoi(getenv("DSP_V92_PRECODE")));
	double margin;

	trellis_on = !(getenv("DSP_V92_TRELLIS") &&
		       !atoi(getenv("DSP_V92_TRELLIS")));
	/* the prefilter, if it works out (DSP_V92_PREFILTER=0: not) */
	if (rx == V92R_TRN2 && sym - e2u_sym > V92_PF_FIT + 32 &&
	    pf_design(fix))
		return;

	/* the precoders to choose from: our feedback taps, shrunk or not
	   (the more, the fewer points the analogue modem needs, but the
	   more of the channel's interference is left to the feed forward
	   taps), each with its fit; without precoding, the training's */
	if (pre && rx == V92R_TRN2 && sym - e2u_sym > V92_FIT + 32)
		for (i = 0; i < NG; i++) {
			if (precoder_fit(gammas[i], bgs[ng], fgs[ng],
					 &e2s[ng]))
				continue;
			sbs[ng] = 0;
			for (j = 0; j < V92_EQ_B; j++)
				sbs[ng] += fabs(bgs[ng][j]);
			DSP_DBG("v92: precoder %.2f: mse %.5f, sum |b| %.2f\n",
				gammas[i], e2s[ng], sbs[ng]);
			ng++;
		}
	if (!ng)
		pre = 0;
	margin = getenv("DSP_V92_MARGIN") ? atof(getenv("DSP_V92_MARGIN")) :
		pre && trellis_on ? V92_MARGIN_CODED : V92_MARGIN;
	margin_used = margin;
	for (drn = 19; drn >= 1 && best < 0; drn--) {
		int K = 2 * (drn + 17), k, g;
		double ms3 = 0, ms0 = 0;
		if (up_mask && !((up_mask >> (drn - 1)) & 1))
			continue;
		M = (int)ceil(pow(2.0, K / 12.0) - 1e-9);
		if (pre) {
			/* the precoder's output: uniform over M points (2M
			   on the trellis frame's 4th symbol) */
			ms0 = (double)M * M / 3;
			ms3 = 4.0 * M * M / 3;
		} else {
			/* least power of a class (points 2j + 1), and all
			   2M points on the trellis frame's 4th symbol */
			for (k = 0; k < M; k++) {
				int a = 2 * k + 1, bb = 2 * (M - k) - 1;
				ms0 += (a < bb ? a * a : bb * bb);
			}
			ms0 /= M;
			ms3 = (4.0 * M * M - 1) / 3;
		}
		msa = (9 * ms0 + 3 * ms3) / 12;
		for (g = 0; g < (pre ? ng : 1); g++) {
			double e2 = pre ? e2s[g] : mse_trn;
			/* the points: with precoding the 4th symbol's reach
			   M (1 + sum |b|); 2M at least, and an even number
			   of positive ones (the original library takes one
			   point too many on the 4th symbol otherwise) */
			int l = (int)ceil(M * (1 + (pre ? sbs[g] : 0))) + 2;
			if (l < 2 * M)
				l = 2 * M;
			l += l & 1;
			if (l > V92_LC_MAX)
				continue;
			if (fix ? drn != atoi(fix) : msa * e2 * margin > 1)
				continue;
			if (best < 0 || (pre && e2 < e2s[best])) {
				best = g;
				lc = l;
			}
		}
	}
	drn++;
	if (best < 0) {
		/* the slowest, without precoding */
		drn = 1;
		M = 8;
		msa = 37;
		lc = 2 * M;
		pre = 0;
		best = 0;
	}
	memset(&cpd, 0, sizeof(cpd));
	cpd.drn = drn;
	for (i = 0; i < 12; i++)
		cpd.M[i] = M;
	/* precoder: our decision feedback (the receiver keeps its feed
	   forward part); 4 taps at least in each section, as the original
	   library's filters have a multiple of 4 taps (and fail on none) */
	cpd.len[0] = 4;
	cpd.len[1] = pre ? V92_EQ_B : 4;
	cpd.len[2] = 4;
	cpd.len[3] = 4;
	cpd.coef[2][0] = 32767;
	if (pre) {
		memcpy(f, fgs[best], sizeof(f));
		mse_trn = (float)e2s[best];
		for (j = 0; j < V92_EQ_B; j++) {
			long q = lrint(-bgs[best][j] * 16384);
			q = q > 32767 ? 32767 : q < -32768 ? -32768 : q;
			cpd.coef[1][j] = (short)q;
			b[j] = -q / 16384.0;
		}
		b_frozen = 1;
	}
	cpd.ncon = 1;
	cpd.lc[0] = lc;
	for (j = 0; j < lc; j++)
		cpd.pts[0][j] = (2 * j + 1) * 16;
	cpd.g = 1 / (16 * sqrt(msa));
	/* the 64 state code (DSP_V92_TRELLIS=16 or 32 for another, 0 for
	   none decoded): at 38667 bit/s it makes half the errors of the
	   16 state one */
	cpd.trellis = 2;
	if (getenv("DSP_V92_TRELLIS"))
		cpd.trellis = atoi(getenv("DSP_V92_TRELLIS")) == 16 ? 0 :
			atoi(getenv("DSP_V92_TRELLIS")) == 32 ? 1 : 2;
	DSP_DBG("v92: CPd: %d bit/s, M %d, %d points, G %.5f (mse %.5f)%s, "
		"%d state trellis%s\n", (drn + 17) * 8000 / 6, M, 2 * lc,
		cpd.g, mse_trn, pre ? ", precoding" : "", 16 << cpd.trellis,
		trellis_on ? "" : " (not decoded)");
	if (pre)
		DSP_DBG("v92: precoder p1 %d %d %d %d %d %d %d %d %d %d %d %d "
			"%d %d %d %d (%.2f)\n", cpd.coef[1][0],
			cpd.coef[1][1], cpd.coef[1][2], cpd.coef[1][3],
			cpd.coef[1][4], cpd.coef[1][5], cpd.coef[1][6],
			cpd.coef[1][7], cpd.coef[1][8], cpd.coef[1][9],
			cpd.coef[1][10], cpd.coef[1][11], cpd.coef[1][12],
			cpd.coef[1][13], cpd.coef[1][14], cpd.coef[1][15],
			gammas[best]);
#undef NG
}

/*
 *    the prefilter design (6.4.2/V.92)
 *
 *    The channel from the analogue modem's transmitter to the network's
 *    samples, ch, by least squares on TRN2u (the prefilter and the
 *    precoder do not run yet then).  The prefilter, G z2 = w, makes
 *    ch * w a response with its main tap 1 at a delay d, the precoder's
 *    taps after it, and as little as can be elsewhere (with some |w|^2
 *    for the noise); the precoder takes those taps away, and what the
 *    network samples is the point itself.  So the points are G.711
 *    levels, the quantizer does not add to them, and the receiver reads
 *    the level at d.  The power, E (w * x)^2 <= 1 (as G says), sets how
 *    far out the points go, and so their least gap; the rate is the
 *    highest at which a simulation through ch and G.711, with the noise
 *    the fit found beyond G.711's own, makes (almost) no errors.
 */

/* the channel: received sample at eqL + n = sum ch[j + PRE] x(n - j),
   j = -V92_CH_PRE .. V92_CH_POST, from TRN2u's decisions; the fit's
   error (*e2) and the part of it G.711's quantization explains (*q2) */
int V92Up::ch_fit(double *e2, double *q2)
{
	static double A[V92_CH * V92_CH];
	double v[V92_CH], phi[V92_CH];
	long s0 = sym - V92_PF_FIT + V92_CH_POST, s1 = sym - 1 - V92_CH_PRE, sn;
	long cnt = 0;
	int i, j;

	memset(A, 0, sizeof(A));
	memset(v, 0, sizeof(v));
	for (sn = s0; sn <= s1; sn++) {
		double r = at(buf, eqL + sn);
		for (j = 0; j < V92_CH; j++)
			phi[j] = dring[(sn + V92_CH_PRE - j) & (V92_DR - 1)];
		for (i = 0; i < V92_CH; i++) {
			for (j = 0; j < V92_CH; j++)
				A[i * V92_CH + j] += phi[i] * phi[j];
			v[i] += phi[i] * r;
		}
	}
	for (i = 0; i < V92_CH; i++)
		A[i * V92_CH + i] *= 1 + 1e-6;
	if (solve(A, v, ch, V92_CH))
		return -1;
	*e2 = *q2 = 0;
	for (sn = s0; sn <= s1; sn++, cnt++) {
		double r = at(buf, eqL + sn), y = 0, a = fabs(r);
		int lo = 0, hi = 127, k;
		for (j = 0; j < V92_CH; j++)
			y += ch[j] * dring[(sn + V92_CH_PRE - j) & (V92_DR - 1)];
		*e2 += (r - y) * (r - y);
		/* the level's step */
		while (lo < hi) {
			int m = (lo + hi) / 2;
			if (pf_lev[m] < a)
				lo = m + 1;
			else
				hi = m;
		}
		k = lo;
		double st = k < 127 ? pf_lev[k + 1] - pf_lev[k] :
			pf_lev[k] - pf_lev[k - 1];
		if (k > 0 && pf_lev[k] - pf_lev[k - 1] < st)
			st = pf_lev[k] - pf_lev[k - 1];
		*q2 += st * st / 12;
	}
	*e2 /= cnt;
	*q2 /= cnt;
	return 0;
}

/* the prefilter for ch: w, the delay d, the precoder's taps; *resid the
   interference left (against the main tap's power) */
int V92Up::pf_filter(double n0, double *resid, int dfix)
{
	enum { K = V92_CH + V92_PF_W - 1 };
	static double G[V92_PF_W * V92_PF_W], A[V92_PF_W * V92_PF_W];
	double w[V92_PF_W], rhs[V92_PF_W], c[K], best = -1, hh = 0;
	int i, j, k, d;

	for (j = 0; j < V92_CH; j++)
		hh += ch[j] * ch[j];
	/* H(k, i) = ch[k - i]: G = H'H, Toeplitz */
	for (i = 0; i < V92_PF_W; i++)
		for (j = 0; j < V92_PF_W; j++) {
			double t = 0;
			int l = abs(i - j);
			for (k = 0; k + l < V92_CH; k++)
				t += ch[k] * ch[k + l];
			G[i * V92_PF_W + j] = t;
		}
	for (d = dfix < 0 ? 0 : dfix; d <= (dfix < 0 ? 40 : dfix); d++) {
		int kd = d + V92_CH_PRE;
		double J = 0, ww = 0, r = 0;
		if (kd + V92_PF_B >= K)
			break;
		memcpy(A, G, sizeof(A));
		/* the main tap and the precoder's are free */
		for (k = kd; k <= kd + V92_PF_B; k++)
			for (i = 0; i < V92_PF_W; i++) {
				double hi_ = k - i >= 0 && k - i < V92_CH ?
					ch[k - i] : 0;
				if (hi_ == 0)
					continue;
				for (j = 0; j < V92_PF_W; j++)
					if (k - j >= 0 && k - j < V92_CH)
						A[i * V92_PF_W + j] -=
							hi_ * ch[k - j];
			}
		for (i = 0; i < V92_PF_W; i++) {
			A[i * V92_PF_W + i] += n0 * hh;
			rhs[i] = kd - i >= 0 && kd - i < V92_CH ? ch[kd - i] : 0;
		}
		if (solve(A, rhs, w, V92_PF_W))
			continue;
		for (i = 0; i < V92_PF_W; i++)
			r += (kd - i >= 0 && kd - i < V92_CH ? ch[kd - i] : 0) *
				w[i];
		if (fabs(r) < 1e-12)
			continue;
		for (i = 0; i < V92_PF_W; i++) {
			w[i] /= r;
			ww += w[i] * w[i];
		}
		for (k = 0; k < K; k++) {
			c[k] = 0;
			for (i = 0; i < V92_PF_W; i++)
				if (k - i >= 0 && k - i < V92_CH)
					c[k] += ch[k - i] * w[i];
			if (k < kd || k > kd + V92_PF_B)
				J += c[k] * c[k];
		}
		*resid = J;
		J += n0 * hh * ww;
		if (best < 0 || J < best) {
			best = J;
			pf_d = d;
			memcpy(pf_w, w, sizeof(w));
			for (k = 0; k < V92_PF_B; k++)
				pf_b[k] = c[kd + 1 + k];
		}
	}
	if (best < 0)
		return -1;
	/* what is left with the chosen one */
	{
		int kd = pf_d + V92_CH_PRE;
		double J = 0;
		for (k = 0; k < K; k++) {
			double t = 0;
			for (i = 0; i < V92_PF_W; i++)
				if (k - i >= 0 && k - i < V92_CH)
					t += ch[k - i] * pf_w[i];
			if (k < kd || k > kd + V92_PF_B)
				J += t * t;
		}
		*resid = J;
	}
	return 0;
}

/* lc positive points: G.711 levels, each gap at least gap (the first
   from 0 at least half of it); 0 if the levels run out */
int V92Up::pf_points(int gap, int lc, short *pts)
{
	int k, n = 0;
	for (k = 0; k < 128 && n < lc; k++) {
		int v = pf_lev[k];
		if ((n == 0 && 2 * v >= gap && v > 0) ||
		    (n > 0 && v - pts[n - 1] >= gap))
			pts[n++] = (short)v;
	}
	return n == lc;
}

/* a value (in the points' units) in grid units: the point eta at
   2 eta + 1, linear in between */
float V92Up::pf_grid(const V92Cpd *c, float r) const
{
	const int *p = c->pts[0];
	int lc = c->lc[0], lo = 0, hi = lc - 1;
	float a = fabsf(r), u;

	if (a < p[0])
		u = a / p[0];
	else if (a >= p[lc - 1])
		u = 2 * lc - 1 + 2 * (a - p[lc - 1]) /
			(float)(lc > 1 ? p[lc - 1] - p[lc - 2] : p[0]);
	else {
		while (hi - lo > 1) {
			int m = (lo + hi) / 2;
			if (p[m] <= a)
				lo = m;
			else
				hi = m;
		}
		u = 2 * lo + 1 + 2 * (a - p[lo]) / (float)(p[hi] - p[lo]);
	}
	return r < 0 ? -u : u;
}

float V92Up::pf_point(const V92Cpd *c, int eta) const
{
	return eta >= 0 ? (float)c->pts[0][eta] : -(float)c->pts[0][-eta - 1];
}

/* nsym symbols of the design (modulus M, the points) through ch and
   G.711: the transmitter's power (E (w * x)^2, 1 the most), the grid
   mse; the symbol errors */
int V92Up::pf_sim(int M, const short *pts, int lc, int nsym, double *pw,
		  double *mseg)
{
	enum { NS = 4096 };
	static double xs[NS], ts[NS];
	static int want[NS];
	V92Cpd c;
	unsigned rnd = 12345;
	int n, k, errs = 0, cnt = 0;
	double p = 0;

	if (nsym > NS)
		nsym = NS;
	c.lc[0] = lc;
	for (k = 0; k < lc; k++)
		c.pts[0][k] = pts[k];
	for (n = 0; n < nsym; n++) {
		double fb = 0, bestv = 0;
		int K, par = 0, best = 0, found = 0, eta;
		for (k = 0; k < V92_PF_B && n - 1 - k >= 0; k++)
			fb += pf_b[k] * xs[n - 1 - k];
		rnd = rnd * 1103515245 + 12345;
		K = (int)((rnd >> 8) % (unsigned)M);
		rnd = rnd * 1103515245 + 12345;
		par = (rnd >> 16) & 1;
		/* the class's point nearest the feedback: the least output */
		for (eta = -lc; eta < lc; eta++) {
			int in = n % 4 == 3 ?
				((eta - 2 * K - par) % (2 * M) == 0) :
				((eta - K) % M == 0);
			double v = pf_point(&c, eta) - fb;
			if (in && (!found || fabs(v) < fabs(bestv))) {
				best = eta;
				bestv = v;
				found = 1;
			}
		}
		xs[n] = bestv;
		want[n] = best;
	}
	for (n = 0; n < nsym; n++) {
		double t = 0;
		for (k = 0; k < V92_PF_W && n - k >= 0; k++)
			t += pf_w[k] * xs[n - k];
		ts[n] = t;
		p += t * t;
	}
	*pw = p / nsym;
	*mseg = 0;
	for (n = 64; n < nsym - 64; n++) {
		long m = n + pf_d;
		double r = 0, a, lev;
		int j, lo = 0, hi = 127, eta;
		float u;
		for (j = -V92_CH_PRE; j <= V92_CH_POST; j++)
			if (m - j >= 0 && m - j < nsym)
				r += ch[j + V92_CH_PRE] * ts[m - j];
		if (pf_noise > 0) {
			double u1, u2;
			rnd = rnd * 1103515245 + 12345;
			u1 = ((rnd >> 8) + 1.0) / 16777217.0;
			rnd = rnd * 1103515245 + 12345;
			u2 = ((rnd >> 8) + 1.0) / 16777217.0;
			r += pf_noise * sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
		}
		/* our echo (Gaussian here) comes in before the codec and is
		   taken away after it: what is left of it is the codec's
		   quantization of the sum */
		double ec = 0;
		if (pf_echo > 0) {
			double u1, u2;
			rnd = rnd * 1103515245 + 12345;
			u1 = ((rnd >> 8) + 1.0) / 16777217.0;
			rnd = rnd * 1103515245 + 12345;
			u2 = ((rnd >> 8) + 1.0) / 16777217.0;
			ec = pf_echo * sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
			r += ec;
		}
		/* G.711: the nearest level */
		a = fabs(r);
		while (lo < hi) {
			int mm = (lo + hi) / 2;
			if (pf_lev[mm] < a)
				lo = mm + 1;
			else
				hi = mm;
		}
		lev = pf_lev[lo];
		if (lo > 0 && a - pf_lev[lo - 1] < lev - a)
			lev = pf_lev[lo - 1];
		/* (with what the canceller leaves of it) */
		if (pf_echo_res > 0) {
			double u1, u2;
			rnd = rnd * 1103515245 + 12345;
			u1 = ((rnd >> 8) + 1.0) / 16777217.0;
			rnd = rnd * 1103515245 + 12345;
			u2 = ((rnd >> 8) + 1.0) / 16777217.0;
			ec -= pf_echo_res * sqrt(-2 * log(u1)) *
				cos(2 * M_PI * u2);
		}
		u = pf_grid(&c, (float)(((r < 0 ? -lev : lev) - ec) / pf_cd));
		eta = (int)floorf(u / 2);
		if (eta < -lc)
			eta = -lc;
		if (eta > lc - 1)
			eta = lc - 1;
		*mseg += (u - (2 * eta + 1)) * (u - (2 * eta + 1));
		cnt++;
		if (n % 4 == 3 ? (eta - want[n]) % (2 * M) != 0 :
		    (eta - want[n]) % M != 0)
			errs++;
	}
	*mseg /= cnt;
	return errs;
}

/* whether drn can be had with the design: its points' least gap and
   the grid mse in pf_gap[], pf_mseg[] */
int V92Up::pf_rate(int drn)
{
	int K = 2 * (drn + 17), M, lc, lo = 8, hi = 2048, errs;
	double sb = 0, pw, mg;
	short pts[V92_LC_MAX];

	pf_gap[drn] = 0;
	M = (int)ceil(pow(2.0, K / 12.0) - 1e-9);
	for (int k = 0; k < V92_PF_B; k++)
		sb += fabs(pf_b[k]);
	/* the trellis frame's 4th symbol: classes of 2M */
	lc = (int)ceil(M * (1 + sb)) + 2;
	lc += lc & 1;
	if (lc > V92_LC_MAX)
		return 0;
	/* the widest gap within the power */
	if (!pf_points(lo, lc, pts))
		return 0;
	pf_sim(M, pts, lc, 1000, &pw, &mg);
	if (pw > 1)
		return 0;
	while (hi - lo > 8) {
		int m = (lo + hi) / 2;
		if (pf_points(m, lc, pts) &&
		    (pf_sim(M, pts, lc, 1000, &pw, &mg), pw <= 1))
			lo = m;
		else
			hi = m;
	}
	pf_points(lo, lc, pts);
	errs = pf_sim(M, pts, lc, 4000, &pw, &mg);
	DSP_TRACE("v92: prefilter %d bit/s: gap %d, %d points, power %.2f, "
		  "%d errors, grid mse %.4f\n", (drn + 17) * 8000 / 6, lo,
		  2 * lc, pw, errs, mg);
	/* a few in 4000 the trellis code takes care of */
	if (errs > 2)
		return 0;
	pf_gap[drn] = (short)lo;
	pf_mseg[drn] = mg;
	return 1;
}

/* the received sample of symbol k (relative to the channel's main tap),
   less what the plain channel still has of the symbols before B1u (in
   TRN2u's modulation) */
double V92Up::pf_sample(long k)
{
	double r = at(buf, pf_eqL + k);
	int j = (int)(k - pf_s0 + 1);
	if (j < -V92_CH_PRE)
		j = -V92_CH_PRE;
	for (; j <= V92_CH_POST; j++)
		r -= ch[j + V92_CH_PRE] * dring[(k - j) & (V92_DR - 1)];
	return r;
}

/* the analogue modem's transmitter starts anew at symbol r: what the
   line still has of the signal before it is like TRN2u's before B1u,
   its line signal rebuilt from the decisions */
void V92Up::pf_reset(long r)
{
	for (long m = r - V92_CH_POST - V92_PF_F; m < r; m++) {
		double v = 0;
		for (int i = 0; i < V92_PF_W && m - i >= pf_s0; i++)
			v += pf_rw[i] * pf_xr[(m - i) & (V92_PF_XR - 1)];
		dring[m & (V92_DR - 1)] = v;
	}
	pf_s0 = r;
	pf_rst = -1;
	DSP_DBG("v92: the analogue modem's transmitter anew at symbol %ld\n",
		r);
}

/* the receiver takes up the design: its prefilter, precoder and main tap,
   the few taps at the delay from the main tap, and the slip's fit from
   nothing (against this channel) */
void V92Up::pf_rx_take()
{
	memcpy(pf_rw, pf_w, sizeof(pf_rw));
	memcpy(pf_rb, pf_b, sizeof(pf_rb));
	pf_rcd = pf_cd;
	memset(pf_f, 0, sizeof(pf_f));
	pf_f[V92_PF_F / 2] = 1 / pf_rcd;
	pf_new = 0;
	/* the channel's derivative: band limited, d/dt of the sinc
	   interpolation of its taps */
	for (int j = 0; j < V92_CH; j++) {
		double d = 0;
		for (int m = 0; m < V92_CH; m++)
			if (m != j)
				d += ch[m] * ((j - m) & 1 ? -1.0 : 1.0) / (j - m);
		pf_dch[j] = d;
	}
	sl_uu = sl_ud = sl_dd = sl_ue = sl_de = sl_ee = 0;
	sl_n = 0;
	sl_base = 0;
	sl_gm = 0.02;
}

/* a data symbol decided (its precoder output in pf_xr): the line signal
   (through the prefilter), and the received sample a channel's length
   back against what the channel makes of it.  Forgetting over a second,
   the residual's fit to the channel's output (a gain) and to its
   derivative's (a slip of the analogue modem's timing) */
void V92Up::pf_slip_sample()
{
	double v = 0, u = 0, d = 0, e, k;
	const double lam = 1 - 1.0 / 8000;
	int i, j;

	for (i = 0; i < V92_PF_W && sym - i >= pf_s0; i++)
		v += pf_rw[i] * pf_xr[(sym - i) & (V92_PF_XR - 1)];
	pf_ln[sym & (V92_PF_XR - 1)] = v;
	/* the line signal the sample has is all from pf_s0 on, and the
	   decisions are the data's */
	if (sym - V92_CH + 1 < pf_s0 || rn || fpe_rx || pf_new)
		return;
	for (j = 0; j < V92_CH; j++) {
		double l = pf_ln[(sym - j) & (V92_PF_XR - 1)];
		u += ch[j] * l;
		d += pf_dch[j] * l;
	}
	e = pf_sample(sym - V92_CH_PRE) - u;
	/* the network's codec passes a small change of its input only as
	   far as the noise spreads it across its decision thresholds: the
	   change, as it comes out of the codec */
	k = pf_qslope(u);
	u *= k;
	d *= k;
	sl_uu = lam * sl_uu + u * u;
	sl_ud = lam * sl_ud + u * d;
	sl_dd = lam * sl_dd + d * d;
	sl_ue = lam * sl_ue + u * e;
	sl_de = lam * sl_de + d * e;
	sl_ee = lam * sl_ee + e * e;
	sl_n++;
	if (sl_n == 16000)
		pf_slip_fit(&sl_g1, &sl_tau1, &sl_x1), sl_t1 = sym;
	if (sl_n == 24000)
		sl_base = sl_gm;
	if (sl_n % 8000 == 0) {
		double g, t, x;
		if (!pf_slip_fit(&g, &t, &x))
			DSP_TRACE("v92: slip: gain %+.4f, timing %+.5f, residual "
				"%.1f rms, %.0f %% of it explained, grid mse "
				"%.4f\n", g, t, sqrt(sl_ee * (1 - lam)),
				100 * x, dmse_g);
	}
}

/* the slope of the network codec's mean output at its input v, with the
   noise (pf_noise rms, Gaussian, and our echo) before it: its steps at
   the thresholds between the levels, each in as far as the noise
   reaches it */
double V92Up::pf_qslope(double v) const
{
	double a = fabs(v), k = 0,
		sg = sqrt(pf_noise * pf_noise + pf_echo * pf_echo);
	if (sg < 0.5)
		sg = 0.5;
	int j;

	/* the thresholds within 5 sigma on either side (and the one at
	   zero, between -L0 and L0) */
	for (j = 0; j < 127; j++) {
		double b = (pf_lev[j] + pf_lev[j + 1]) / 2.0,
			z = (b - a) / sg;
		if (fabs(z) < 5)
			k += (pf_lev[j + 1] - pf_lev[j]) * exp(-z * z / 2);
		z = (b + a) / sg;
		if (z < 5)
			k += (pf_lev[j + 1] - pf_lev[j]) * exp(-z * z / 2);
	}
	if (pf_lev[0] > 0 && a / sg < 5)
		k += 2 * pf_lev[0] * exp(-a * a / (2 * sg * sg));
	k /= sg * sqrt(2 * M_PI);
	return k > 1.5 ? 1.5 : k;
}

/* the fit: the channel's gain and the slip (in samples), and how much of
   the residual's power they take away */
int V92Up::pf_slip_fit(double *gain, double *tau, double *expl)
{
	double det = sl_uu * sl_dd - sl_ud * sl_ud;
	if (sl_n < 8000 || det <= 0 || sl_ee <= 0)
		return -1;
	*gain = (sl_ue * sl_dd - sl_de * sl_ud) / det;
	*tau = (sl_de * sl_uu - sl_ue * sl_ud) / det;
	*expl = (*gain * sl_ue + *tau * sl_de) / sl_ee;
	return 0;
}

/* the design for the slip: the channel as it is now (the fit's gain and
   slip on it), the prefilter for it at the same delay, and its
   precoder; the receiver keeps the old ones until B1u */
int V92Up::pf_slip_design()
{
	double g, t, x, hh = 0, resid, sb = 0;
	double ch0[V92_CH];
	int j;

	if (pf_slip_fit(&g, &t, &x))
		return -1;
	/* the fit is a second behind (its forgetting), and B1u some 3000
	   symbols ahead: the slip then, at its rate since 2 s into the
	   design */
	if (sym - sl_t1 > 8000)
		t += (t - sl_tau1) / (sym - sl_t1) * (8000 + 3000);
	memcpy(ch0, ch, sizeof(ch));
	for (j = 0; j < V92_CH; j++) {
		ch[j] = (1 + g) * ch[j] + t * pf_dch[j];
		hh += ch[j] * ch[j];
	}
	if (pf_filter(1e-3 + pf_noise * pf_noise / hh, &resid, pf_d) ||
	    pf_quant()) {
		memcpy(ch, ch0, sizeof(ch));
		memcpy(pf_w, pf_rw, sizeof(pf_w));
		memcpy(pf_b, pf_rb, sizeof(pf_b));
		pf_cd = pf_rcd;
		return -1;
	}
	for (j = 0; j < V92_PF_B; j++)
		sb += fabs(pf_b[j]);
	pf_new = 1;
	pf_new_at = -1;
	DSP_DBG("v92: the prefilter for a slip of %+.5f (gain %+.4f, %.0f %% "
		"of the residual): delay %d, interference %.1f dB, sum |b| "
		"%.2f, G %.6f\n", t, g, 100 * x, pf_d, 10 * log10(resid), sb,
		pf_g);
	return 0;
}

/* with the prefilter: the margin to hold the line to is what B1u and
   the first data had (the simulation's mse is only a guide) */
void V92Up::pf_margin()
{
	if (!pf_on)
		return;
	double m = dmse_g > pf_mseg[cpd.drn] ? dmse_g : pf_mseg[cpd.drn];
	margin_used = m > 0 ? 1 / m : 1e3;
	DSP_DBG("v92: grid mse %.4f (simulated %.4f): margin %.1f\n", dmse_g,
		pf_mseg[cpd.drn], margin_used);
}

/* CPd for drn with the prefilter */
int V92Up::pf_cpd(V92Cpd *c, int drn)
{
	int K = 2 * (drn + 17), M, lc, j;
	double sb = 0;
	short pts[V92_LC_MAX];

	M = (int)ceil(pow(2.0, K / 12.0) - 1e-9);
	for (j = 0; j < V92_PF_B; j++)
		sb += fabs(pf_b[j]);
	lc = (int)ceil(M * (1 + sb)) + 2;
	lc += lc & 1;
	int gap = pf_gap[drn];
	pf_points(gap, lc, pts);
	int ack = c->ack;
	memset(c, 0, sizeof(*c));
	c->ack = ack;
	c->drn = drn;
	for (j = 0; j < 12; j++)
		c->M[j] = M;
	/* z1 and p2 unused, 4 taps (the original library's sections are
	   of a multiple of 4 taps, and none fails) */
	c->len[0] = 4;
	c->len[1] = V92_PF_B;
	c->len[2] = V92_PF_W;
	c->len[3] = 4;
	for (j = 0; j < V92_PF_B; j++)
		c->coef[1][j] = pf_p1[j];
	for (j = 0; j < V92_PF_W; j++)
		c->coef[2][j] = pf_z2[j];
	c->g = pf_g;
	c->ncon = 1;
	c->lc[0] = lc;
	for (j = 0; j < lc; j++)
		c->pts[0][j] = pts[j];
	c->trellis = 2;
	if (getenv("DSP_V92_TRELLIS"))
		c->trellis = atoi(getenv("DSP_V92_TRELLIS")) == 16 ? 0 :
			atoi(getenv("DSP_V92_TRELLIS")) == 32 ? 1 : 2;
	return gap;
}

/* the prefilter and precoder as CPd says them: G (4 G in Q0.16), z2
   (Q0.15), p1 (Q1.14), and pf_w, pf_b, pf_cd as they come out of them */
int V92Up::pf_quant()
{
	double mx = 0;
	int i, j, g4;

	for (i = 0; i < V92_PF_W; i++)
		if (fabs(pf_w[i]) > mx)
			mx = fabs(pf_w[i]);
	g4 = (int)ceil(4 * mx * 65536 * 1.0001);
	if (g4 < 1 || g4 > 65535)
		return -1;
	pf_g = g4 / (4 * 65536.0);
	for (i = 0; i < V92_PF_W; i++) {
		long q = lrint(pf_w[i] / pf_g * 32768);
		q = q > 32767 ? 32767 : q < -32768 ? -32768 : q;
		pf_z2[i] = (short)q;
		pf_w[i] = pf_g * q / 32768;
	}
	/* the response with them, and the precoder for it */
	int kd = pf_d + V92_CH_PRE;
	double c[V92_CH + V92_PF_W];
	for (int k = 0; k < V92_CH + V92_PF_W - 1; k++) {
		c[k] = 0;
		for (i = 0; i < V92_PF_W; i++)
			if (k - i >= 0 && k - i < V92_CH)
				c[k] += ch[k - i] * pf_w[i];
	}
	pf_cd = c[kd];
	for (j = 0; j < V92_PF_B; j++) {
		long q = lrint(-c[kd + 1 + j] / pf_cd * 16384);
		q = q > 32767 ? 32767 : q < -32768 ? -32768 : q;
		pf_p1[j] = (short)q;
		pf_b[j] = -q / 16384.0;
	}
	return 0;
}

/* the design with the prefilter; 0 if there is none (the receiver's feed
   forward taps then) */
int V92Up::pf_design(const char *fix)
{
	double e2, q2, hh = 0, resid, sb = 0;
	int i, j, drn;

	pf = 0;
	if (!pf_ok || ia.lmax < V92_PF_W ||
	    ia.ltot < 8 + V92_PF_B + V92_PF_W)
		return 0;
	for (i = 0; i < 128; i++)
		pf_lev[i] = (short)v90_ucode_linear(alaw, i);
	for (i = 1; i < 128; i++)
		for (j = i; j > 0 && pf_lev[j] < pf_lev[j - 1]; j--) {
			short t = pf_lev[j];
			pf_lev[j] = pf_lev[j - 1];
			pf_lev[j - 1] = t;
		}
	if (ch_fit(&e2, &q2))
		return 0;
	for (j = 0; j < V92_CH; j++)
		hh += ch[j] * ch[j];
	/* the noise beyond G.711's, and the channel estimate's own error
	   (as much again per tap as the fit's error over its symbols) */
	pf_noise = sqrt((e2 > q2 ? e2 - q2 : 0) +
			q2 * V92_CH / (V92_PF_FIT - V92_CH));
	if (pf_filter(1e-3 + pf_noise * pf_noise / hh, &resid))
		return 0;
	if (pf_quant())
		return 0;
	for (j = 0; j < V92_PF_B; j++)
		sb += fabs(pf_b[j]);
	DSP_DBG("v92: channel: SNR %.1f dB (G.711's alone %.1f dB, noise "
		"%.1f rms, our echo %.0f rms); prefilter delay %d, interference %.1f dB, "
		"sum |b| %.2f, G %.6f\n", 10 * log10(hh / e2),
		10 * log10(hh / q2), pf_noise, pf_echo, pf_d,
		10 * log10(resid), sb,
		pf_g);
	for (drn = 19; drn >= 1; drn--) {
		if (up_mask && !((up_mask >> (drn - 1)) & 1))
			continue;
		if (fix && drn != atoi(fix))
			continue;
		if (pf_rate(drn))
			break;
	}
	if (drn < 1)
		return 0;
	pf_cpd(&cpd, drn);
	pf = 1;
	margin_used = pf_mseg[drn] > 0 ? 1 / pf_mseg[drn] : 1e3;
	DSP_DBG("v92: CPd: %d bit/s, M %d, %d points (gap %d, to %d), G %.6f, "
		"prefilter of %d taps, precoder of %d, %d state trellis\n",
		(drn + 17) * 8000 / 6, cpd.M[0], 2 * cpd.lc[0], pf_gap[drn],
		cpd.pts[0][cpd.lc[0] - 1], cpd.g, V92_PF_W, V92_PF_B,
		16 << cpd.trellis);
	return 1;
}

void V92Up::make_seq(int cpd_, int ack)
{
	if (cpd_ && fpe) {
		cpd_fpe.ack = ack;
		seq_len = cpd_fpe.build(seq, D);
		cpd_sent++;
		cpd_at = 0;
	} else if (cpd_) {
		if (!cpd.drn || cpd_new)
			design();
		cpd_new = 0;
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

	if (v->rn_tx_init) {
		/* after Rd: TRN2d and the rest on CPt's constellations */
		v->rn_tx_init = 0;
		v->D = v->cpt.drn + 8;
		t->enc.init(&v->cpt, v->D, t->lin);
	}
	if (v->fpe_tx_init) {
		/* after Rf: the data mode modulation, from cleared
		   scrambler, signs and shaping filter */
		v->fpe_tx_init = 0;
		v->D = v->cpu.drn + 20;
		t->enc.init(&v->cpu, v->D, t->lin);
	}
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
		} else if (v->got_suvu && !v->rn_silence &&
			   (!v->cpd_sent || (!v->got_ack && v->cpd_at > 400)) &&
			   /* the prefilter's design wants more of TRN2u (and
			      SUVu and CPu, which come in its modulation) */
			   (v->cpd_sent || !v->pf_ok || v->rx != V92R_TRN2 ||
			    v->sym - v->e2u_sym > V92_PF_FIT + 32))
			v->make_seq(1, v->got_cpu);
		else
			/* with a silent period asked for: SUVd' (no CP
			   before it) */
			v->make_seq(0, v->rn_silence ? 1 : v->got_cpu);
		break;
	case V92F_ED:
		memset(bits, 0, v->D);
		if (v->fcount >= 2) {
			*last = 1;
			v->fcount = 0;
			if (v->rn_silence) {
				/* Ed, then silence (9.8.1.1.3) */
				v->fmode = V92F_SIL;
				t->sil_req = 1;
			} else
				v->fmode = V92F_B1;
		}
		break;
	case V92F_SIL:
		memset(bits, 0, v->D);
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
			connect_n = rn_last = n;
			pf_margin();
			DSP_DBG("v92: connect %d/%d\n",
				(cpu.drn + 20) * 8000 / 6,
				(cpd.drn + 17) * 8000 / 6);
			return V92_CONNECT;
		}
		if (!connected)
			break;
		/* test: DSP_V92_RENEG=<ms>[:<drn>], our own renegotiation
		   that far into data (to that rate) */
		if (!rn_test_at && getenv("DSP_V92_RENEG")) {
			int ms = 0, d_ = 0;
			sscanf(getenv("DSP_V92_RENEG"), "%d:%d", &ms, &d_);
			rn_test_at = connect_n + 8L * ms;
			rn_drn = d_;
		}
		if (rn_test_at > 0 && n >= rn_test_at && !renegs &&
		    rx == V92R_DATA) {
			rn_test_at = -1;
			rn_req = 1;
		}
		/* test: DSP_V92_FPE=<ms>[:<drn>], our own fast parameter
		   exchange that far into data (to that rate) */
		if (!fpe_test_at && getenv("DSP_V92_FPE")) {
			int ms = 0, d_ = 0;
			sscanf(getenv("DSP_V92_FPE"), "%d:%d", &ms, &d_);
			fpe_test_at = connect_n + 8L * ms;
			fpe_drn = d_;
		}
		if (fpe_test_at > 0 && n >= fpe_test_at && !fpes &&
		    rx == V92R_DATA && !rn) {
			fpe_test_at = -1;
			fpe_req = 1;
		}
		/* a fast parameter exchange: the analogue modem's (after
		   RM'), or ours */
		if ((fpe_rx >= 2 || fpe_req) && !fpe_rf && rx == V92R_DATA) {
			fpe_tx_start();
			break;
		}
		/* the analogue modem's (after its Ru-bar), or ours */
		if (rx == V92R_RN_WAIT || (rn_req && rx == V92R_DATA))
			rn_tx_start();
		break;
	case 8:
		/* renegotiation: as in phase 4 */
		if (!rn_sent && rx == V92R_RN_WAIT)
			rn_tx_start();
		/* after the silence: Rt (training constellations), Rt-bar,
		   SUVd and phase 4 (9.8.1.1.4) */
		if (fmode == V92F_SIL && rn_suv2 && tx.seg == V90_SILENCE &&
		    tx.sym % 6 == 0) {
			int k;
			DSP_DBG("v92: Rt\n");
			for (k = 0; k < 6; k++)
				tx.rd_code[k] = tx.enc.ucode[k][0];
			tx.segment(V90_RD);
			rn_silence = rn_wait_suv2 = rn_suv2 = 0;
			rn_tx_init = 1;
			fmode = V92F_SEQ;
			fcount = 0;
			ack_sent = cpd_sent = 0;
			cpd_new = 1;
			got_suvu = 1;
			got_cpu = got_ack = got_e2u = 0;
			make_seq(0, 0);
		}
		if (b1_bad) {
			DSP_DBG("v92: B1u does not come through\n");
			return V92_FAIL;
		}
		if (fpe && !fpe_rf && fpe_rx >= 2 && rx == V92R_DATA)
			fpe_tx_start();
		/* our Rf with no RM a second after it (the HSF modem has
		   none): a renegotiation instead, and from now on */
		if (fpe && fpe_rf && !fpe_rx && rx == V92R_DATA &&
		    n - fpe_at > 8000) {
			DSP_DBG("v92: no RM: no fast parameter exchange, a "
				"renegotiation instead\n");
			fpe_none = 1;
			fpe = fpe_rf = fpe_req = fpe_slip = fpe_tx_init = 0;
			fpe_sw = fpe_early = 0;
			pf_new = 0;
			rn_tx_start();
		}
		if (fmode == V92F_DATA && rx == V92R_DATA && rn_rxd &&
		    dframes > 48) {
			if (fpe) {
				fpes++;
				DSP_DBG("v92: fast parameter exchange %d "
					"done\n", fpes);
			} else
				renegs++;
			int slip_fpe = fpe_slip;
			if (fpe_slip)
				slips++;
			fpe = fpe_rf = fpe_req = fpe_slip = 0;
			rn = rn_req = rn_sent = 0;
			rn_low = rn_high = 0;
			rn_last = n;
			tstep = 7;
			DSP_DBG("v92: renegotiated %d/%d\n",
				(cpu.drn + 20) * 8000 / 6,
				(cpd.drn + 17) * 8000 / 6);
			/* (after one for the slip, the margin of the
			   design) */
			if (!slip_fpe)
				pf_margin();
			return V92_RENEG;
		}
		if (n - rn_at > 8000L * 25) {
			DSP_DBG("v92: renegotiation timeout (rx %d)\n", rx);
			return V92_FAIL;
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
	pf_ok = !(getenv("DSP_V92_PREFILTER") &&
		  !atoi(getenv("DSP_V92_PREFILTER")));
	slip_ok = !(getenv("DSP_V92_SLIP") && !atoi(getenv("DSP_V92_SLIP")));
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
