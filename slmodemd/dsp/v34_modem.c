/*
 *
 *    v34_modem.c - V.34 primary channel: the phase 3 and 4 training
 *                  signals (S, S-bar, PP, TRN, J, J', MP, E) and the
 *                  receiver that trains on them
 *
 *    As the original library sends them:
 *    S        alternating 4 point training symbols at 45 and 135 degrees
 *    S-bar    the same, reversed
 *    PP       6 * the 48 symbol sequence of 10.1.3.6/V.34
 *    TRN      scrambled ones, two bits per symbol, absolute mapping
 *    J, J', MP, E  scrambled, differentially coded with two bits per
 *             symbol (J = 0x8990, J' = 0x899F, least significant bit
 *             first; E = 20 ones)
 *
 *    The caller scrambles with 1 + x^-18 + x^-23, the answerer with
 *    1 + x^-5 + x^-23.
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "v34.h"
#include "v90.h"

#define V34_SRATE       9600
#define V34_ALPHA       0.1f
#define V34_SPAN        16
#define V34_EQ_LEN      48
/* line level of the training and data signals, as the original library */
#define V34_PRIMARY_RMS 2790.0f

#define J_4POINT        0x8990
#define J_DASH          0x899F

static int scramble(unsigned *reg, int tap, int bit)
{
	int out = (bit ^ (*reg >> (tap - 1)) ^ (*reg >> 22)) & 1;
	*reg = (*reg << 1) | out;
	return out;
}

static int descramble(unsigned *reg, int tap, int bit)
{
	int out = (bit ^ (*reg >> (tap - 1)) ^ (*reg >> 22)) & 1;
	*reg = (*reg << 1) | (bit & 1);
	return out;
}

static unsigned rev16(unsigned w)
{
	unsigned r = 0;
	int i;
	for (i = 0; i < 16; i++)
		r |= ((w >> i) & 1) << (15 - i);
	return r;
}


/*
 *    MP
 */

static void put_bits(unsigned char *b, int *n, unsigned val, int nbits)
{
	int i;
	for (i = 0; i < nbits; i++)
		b[(*n)++] = (val >> i) & 1;
}

/* the 16 bit fields between the start bits are covered by the CRC */
static unsigned mp_crc(const unsigned char *b, int len)
{
	unsigned crc = 0xFFFF;
	int i;
	for (i = 18; i < len - 18; i += 17)
		crc = v34_crc_bits(b + i, 16, crc);
	return crc;
}

static int mp_build(const struct v34_mp *mp, unsigned char *b)
{
	int n = 0, i, len = mp->type ? 188 : 88;
	unsigned crc;

	put_bits(b, &n, 0x1FFFF, 17);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, mp->type, 1);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, mp->rate_ca, 4);
	put_bits(b, &n, mp->rate_ac, 4);
	put_bits(b, &n, mp->aux, 1);
	put_bits(b, &n, mp->trellis, 2);
	put_bits(b, &n, mp->nonlinear, 1);
	put_bits(b, &n, mp->expanded, 1);
	put_bits(b, &n, mp->ack, 1);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, mp->rate_mask, 15);
	put_bits(b, &n, mp->asym, 1);
	if (mp->type)
		for (i = 0; i < 6; i++) {
			put_bits(b, &n, 0, 1);
			put_bits(b, &n, mp->h[i], 16);
		}
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, 0, 16);
	put_bits(b, &n, 0, 1);
	crc = mp_crc(b, len);
	for (i = 15; i >= 0; i--)
		b[n++] = (crc >> i) & 1;
	while (n < len)
		b[n++] = 0;
	return len;
}

static int field(const unsigned char *b, int first, int nbits)
{
	int i, v = 0;
	for (i = nbits - 1; i >= 0; i--)
		v = (v << 1) | b[first + i];
	return v;
}

/* returns 0 when the frame is good */
static int mp_parse(struct v34_mp *mp, const unsigned char *b, int len)
{
	unsigned crc = mp_crc(b, len);
	int i;
	for (i = 0; i < 16; i++)
		if (b[len - 17 + (len == 188 ? 0 : -2) + i] != ((crc >> (15 - i)) & 1))
			return -1;
	mp->type = b[18];
	mp->rate_ca = field(b, 20, 4);
	mp->rate_ac = field(b, 24, 4);
	mp->aux = b[28];
	mp->trellis = field(b, 29, 2);
	mp->nonlinear = b[31];
	mp->expanded = b[32];
	mp->ack = b[33];
	mp->rate_mask = field(b, 35, 14);
	mp->asym = b[50];
	for (i = 0; i < 6; i++)
		mp->h[i] = mp->type ? (short)field(b, 52 + 17 * i, 16) : 0;
	return 0;
}


/*
 *    transmitter
 */

static cplx tx_point(int idx)
{
	return v34_trn4[idx & 3];
}

static int tx_dibit(struct v34_tx *t, int b0, int b1)
{
	b0 = scramble(&t->scr, t->tap, b0);
	b1 = scramble(&t->scr, t->tap, b1);
	return b0 | (b1 << 1);
}

static cplx tx_symbol(void *ctx)
{
	struct v34_tx *t = ctx;
	cplx z = { 0, 0 };
	int k, w;

	switch (t->seg) {
	case V34_TX_SILENCE:
		break;
	case V34_TX_S:
	case V34_TX_SBAR:
		/* 45 and 135 degrees; S-bar continues the alternation reversed */
		t->idx = (t->n & 1) ? 1 : 2;
		z = tx_point(t->idx);
		if (t->seg == V34_TX_SBAR)
			z = cplx_scale(z, -1);
		break;
	case V34_TX_PP:
		z = cplx_scale(v34_pp(t->n), 1.41421f);
		break;
	case V34_TX_TRN:
		t->idx = tx_dibit(t, 1, 1);
		z = tx_point(t->idx);
		break;
	case V34_TX_J:
	case V34_TX_JP:
		w = t->seg == V34_TX_J ? J_4POINT : J_DASH;
		k = 2 * (t->n % 8);
		t->idx = (t->idx + tx_dibit(t, (w >> k) & 1, (w >> (k + 1)) & 1)) & 3;
		z = tx_point(t->idx);
		break;
	case V34_TX_MP:
		if (t->pos >= t->nbits) {
			if (t->nbits)
				t->mp_sent++;
			t->mp.ack = t->ack_mp;
			t->nbits = mp_build(&t->mp, t->bits);
			t->pos = 0;
		}
		k = tx_dibit(t, t->bits[t->pos], t->bits[t->pos + 1]);
		t->pos += 2;
		t->idx = (t->idx + k) & 3;
		z = tx_point(t->idx);
		break;
	case V34_TX_DATA:
		if (t->data_on) {
			z = v34_dtx_symbol(&t->dtx, &t->scr, t->tap);
			break;
		}
		/* fall through */
	case V34_TX_E:
		t->idx = (t->idx + tx_dibit(t, 1, 1)) & 3;
		z = tx_point(t->idx);
		break;
	}
	t->n++;
	/* J' and E are single sequences */
	if (t->seg == V34_TX_JP && t->n == 8)
		v34_tx_segment(t, V34_TX_TRN);
	else if (t->seg == V34_TX_E && t->n == 10)
		v34_tx_segment(t, V34_TX_DATA);
	return z;
}

void v34_tx_init(struct v34_tx *t, int baud, int high, int caller)
{
	memset(t, 0, sizeof(*t));
	t->baud = baud;
	qam_tx_init(&t->qam, V34_SRATE, v34_carrier(baud, high),
		    v34_symbol_rate(baud), V34_ALPHA, V34_SPAN, 1.41421f,
		    V34_PRIMARY_RMS * 1.41421f);
	t->qam.get_symbol = tx_symbol;
	t->qam.ctx = t;
	t->tap = caller ? 18 : 5;
	t->seg = V34_TX_SILENCE;
}

void v34_tx_segment(struct v34_tx *t, enum v34_txseg seg)
{
	DSP_DBG("v34 tx: segment %d -> %d after %ld symbols\n", t->seg, seg, t->n);
	t->seg = seg;
	t->n = 0;
	if (seg == V34_TX_MP)
		t->nbits = t->pos = 0;
	/* TRN starts with the scrambler cleared, the remote may train with
	   a copy of it */
	if (seg == V34_TX_TRN)
		t->scr = 0;
}

void v34_tx_data(struct v34_tx *t, const struct v34_dparm *p,
		 void (*get_bits)(void *, unsigned char *, int), void *ctx)
{
	v34_dtx_init(&t->dtx, p);
	t->dtx.get_bits = get_bits;
	t->dtx.ctx = ctx;
	t->data_on = 1;
}

float v34_tx_sample(struct v34_tx *t)
{
	return qam_tx_sample(&t->qam);
}


/*
 *    receiver
 */

int v34_rx_init(struct v34_rx *r, int baud, int high, int caller)
{
	memset(r, 0, sizeof(*r));
	r->baud = baud;
	r->tap = caller ? 5 : 18;
	r->stage = V34_RX_IDLE;
	r->ls_r = calloc(V34_EQ_LEN * V34_EQ_LEN, sizeof(*r->ls_r));
	r->ls_p = calloc(V34_EQ_LEN, sizeof(*r->ls_p));
	if (!r->ls_r || !r->ls_p)
		return -1;
	return qam_rx_init(&r->qam, V34_SRATE, v34_carrier(baud, high),
			   v34_symbol_rate(baud), V34_ALPHA, V34_SPAN,
			   V34_EQ_LEN, 2.0f);
}

/* the receiver of a V.90 digital modem: Ja instead of J */
int v34_rx_v90(struct v34_rx *r)
{
	r->v90 = 1;
	r->jabits = calloc(V90_JA_MAX, 1);
	return r->jabits ? 0 : -1;
}

void v34_rx_free(struct v34_rx *r)
{
	free(r->jabits);
	r->jabits = NULL;
	free(r->ls_r);
	free(r->ls_p);
	r->ls_r = r->ls_p = NULL;
	qam_rx_free(&r->qam);
}

static void rx_set_stage(struct v34_rx *r, enum v34_rxstage s)
{
	DSP_DBG("v34 rx: stage %d -> %d at symbol %ld\n", r->stage, s, r->sym);
	r->stage = s;
	r->n = 0;
	r->ls_n = 0;
	r->ja_n = 0;            /* no frame across a new signal */
}

static int nearest4(cplx z)
{
	return z.re < 0 ? (z.im < 0 ? 0 : 1) : (z.im < 0 ? 3 : 2);
}

/* Ja (V.90): DIL descriptors in MP style frames, repeated */
static void rx_ja_bit(struct v34_rx *r, int bit)
{
	r->jabits[r->ja_n++] = bit;
	if (r->ja_n == 50) {
		r->ja_len = v90_dil_length(r->jabits, r->ja_n);
		if (r->ja_len > V90_JA_MAX)
			r->ja_n = 0;
	}
	if (r->ja_len > 0 && r->ja_n == r->ja_len) {
		r->ja_n = 0;
		if (v90_dil_parse(&r->dil, r->jabits, r->ja_len) == 0) {
			if (!r->ev_ja) {
				int i;
				DSP_DBG("v34 rx: Ja at symbol %ld: DIL N %d, "
					"LSP %d, LTP %d\n", r->sym, r->dil.n,
					r->dil.lsp, r->dil.ltp);
				for (i = 0; i < 8; i++)
					DSP_DBG("v34 rx: DIL H%d %d REF%d %d\n",
						i + 1, r->dil.h[i], i + 1,
						r->dil.ref[i]);
				for (i = 0; i < r->dil.n; i += 16)
					DSP_DBG("v34 rx: DIL Ucodes %d: %d %d %d %d "
						"%d %d %d %d %d %d %d %d %d %d "
						"%d %d\n", i,
						r->dil.ucode[i], r->dil.ucode[i + 1],
						r->dil.ucode[i + 2], r->dil.ucode[i + 3],
						r->dil.ucode[i + 4], r->dil.ucode[i + 5],
						r->dil.ucode[i + 6], r->dil.ucode[i + 7],
						r->dil.ucode[i + 8], r->dil.ucode[i + 9],
						r->dil.ucode[i + 10], r->dil.ucode[i + 11],
						r->dil.ucode[i + 12], r->dil.ucode[i + 13],
						r->dil.ucode[i + 14], r->dil.ucode[i + 15]);
				for (i = 0; i < r->dil.lsp; i += 40) {
					char sp[41], tp[41];
					int k;
					for (k = 0; k < 40; k++) {
						sp[k] = '0' + r->dil.sp[i + k];
						tp[k] = '0' + r->dil.tp[i + k];
					}
					sp[40] = tp[40] = 0;
					DSP_DBG("v34 rx: DIL SP %s TP %s\n", sp, tp);
				}
			}
			r->ev_ja = 1;
		} else
			DSP_DBG("v34 rx: Ja with bad CRC\n");
	}
}

/* CP (V.90): MP style frames of a length given by the number of
   constellations they carry */
static void rx_cp_bit(struct v34_rx *r, int bit)
{
	if (r->ja_n < V90_JA_MAX)
		r->jabits[r->ja_n++] = bit;
	if (r->ja_n == 129) {
		r->ja_len = v90_cp_length(r->jabits, r->ja_n);
		if (r->ja_len <= 0 || r->ja_len > (int)sizeof(r->cpraw))
			r->ja_n = 0;
	}
	if (r->ja_len > 0 && r->ja_n == r->ja_len) {
		struct v90_cp cp;
		r->ja_n = 0;
		if (v90_cp_parse(&cp, r->jabits, r->ja_len) == 0) {
			if (!(cp.cp ? r->ev_cp : r->ev_cpt) ||
			    (cp.ack && !r->ev_cpack))
				DSP_DBG("v34 rx: CP%s%s at symbol %ld: drn %d "
					"Sr %d ld %d, %d constellations, "
					"mask %04x\n", cp.cp ? "" : "t",
					cp.ack ? "'" : "", r->sym, cp.drn,
					cp.sr, cp.ld, cp.ncon, cp.rate_mask);
			memcpy(r->cpraw, r->jabits, r->ja_len);
			if (cp.cp) {
				r->ev_cp = 1;
				if (cp.ack)
					r->ev_cpack = 1;
			} else
				r->ev_cpt = 1;
			/* E may follow CP' (after CPt comes SCR: ones) */
			r->after_mp = cp.cp;
			r->mp_end = r->sym;
			r->ones = 0;
		} else
			DSP_DBG("v34 rx: CP with bad CRC\n");
	}
}

static void rx_bit(struct v34_rx *r, int bit)
{
	r->sr = (r->sr << 1) | bit;
	if (r->v90 && r->ev_ja && r->stage == V34_RX_TRN) {
		if (r->ja_n) {
			rx_cp_bit(r, bit);
			return;
		}
		if ((r->sr & 0x3FFFF) == 0x3FFFE) {
			int i;
			for (i = 0; i < 17; i++)
				r->jabits[i] = 1;
			r->jabits[17] = 0;
			r->ja_n = 18;
			r->ja_len = 0;
			r->after_mp = 0;
			return;
		}
		if (r->after_mp) {
			/* E: 20 ones after CP' */
			r->ones = bit ? r->ones + 1 : 0;
			if (r->ones == 20) {
				r->ev_e = 1;
				DSP_DBG("v34 rx: E at symbol %ld\n", r->sym);
				rx_set_stage(r, V34_RX_DATA);
			}
		}
		return;
	}
	if (r->v90 && r->jabits && !r->ev_ja && r->stage == V34_RX_TRN) {
		if (r->ja_n)
			rx_ja_bit(r, bit);
		else if ((r->sr & 0x3FFFF) == 0x3FFFE) {
			int i;
			for (i = 0; i < 17; i++)
				r->jabits[i] = 1;
			r->jabits[17] = 0;
			r->ja_n = 18;
			r->ja_len = 0;
		}
		return;
	}
	if (r->mp_n) {
		r->mpbits[r->mp_n++] = bit;
		if (r->mp_n == 19)
			r->mp_len = bit ? 188 : 88;
		if (r->mp_n == r->mp_len) {
			struct v34_mp mp;
			r->mp_n = 0;
			if (mp_parse(&mp, r->mpbits, r->mp_len) == 0) {
				if (!r->ev_mp || mp.ack != r->mp.ack)
					DSP_DBG("v34 rx: MP%s type %d rates %d/%d mask %04x "
						"trellis %d expanded %d\n",
						mp.ack ? "'" : "", mp.type,
						mp.rate_ca, mp.rate_ac, mp.rate_mask,
						mp.trellis, mp.expanded);
				r->mp = mp;
				r->ev_mp = 1;
				r->after_mp = 1;
				r->mp_end = r->sym;
				r->ones = 0;
			} else
				DSP_DBG("v34 rx: MP with bad CRC\n");
		}
		return;
	}
	if (r->after_mp) {
		/* E: 20 ones after an MP sequence */
		r->ones = bit ? r->ones + 1 : 0;
		if (r->ones == 20) {
			r->ev_e = 1;
			DSP_DBG("v34 rx: E at symbol %ld (MP end %ld), "
				"mse %.5f\n", r->sym, r->mp_end, r->mse);
			rx_set_stage(r, V34_RX_DATA);
			return;
		}
	}
	if ((r->sr & 0x3FFFF) == 0x3FFFE) {
		/* MP frame sync: 17 ones and a start bit */
		int i;
		for (i = 0; i < 17; i++)
			r->mpbits[i] = 1;
		r->mpbits[17] = 0;
		r->mp_n = 18;
		r->mp_len = 0;
		r->after_mp = 0;
		return;
	}
	if ((r->sr & 0xFFFF) == rev16(J_4POINT)) {
		if (++r->j_count >= 2 && !r->ev_j) {
			r->ev_j = 1;
			DSP_DBG("v34 rx: J at symbol %ld\n", r->sym);
		}
	} else if ((r->sr & 0xFFFF) == rev16(J_DASH) && r->ev_j && !r->ev_jp) {
		r->ev_jp = 1;
		r->ls_n = 0;            /* the caller's phase 4 TRN */
		DSP_DBG("v34 rx: J' at symbol %ld\n", r->sym);
	}
}

/* the 4 point signals after S-bar: J, MP and E are coded differentially */
static void rx_diff(struct v34_rx *r, cplx z)
{
	int idx = nearest4(z), d = (idx - r->idx) & 3;
	r->idx = idx;
	rx_bit(r, descramble(&r->scr, r->tap, d & 1));
	if (r->stage == V34_RX_DATA)
		return;
	rx_bit(r, descramble(&r->scr, r->tap, d >> 1));
}

static void pp_align(struct v34_rx *r)
{
	cplx best = { 0, 0 };
	float bm = -1;
	int off, k;
	for (off = -4; off <= 4; off++) {
		cplx c = { 0, 0 };
		for (k = 0; k < 48; k++)
			c = cplx_add(c, cplx_mulc(r->pp_buf[k], v34_pp(k + 48 + off)));
		if (cplx_pow(c) > bm) {
			bm = cplx_pow(c);
			best = c;
			r->pp_off = off;
		}
	}
	/* rotate the receiver by the phase of the correlation */
	r->qam.theta += atan2f(best.im, best.re);
	DSP_DBG("v34 rx: PP offset %d\n", r->pp_off);
}

/*
 *    least squares training: on a band limited line the LMS adaptation
 *    takes far too long to reach the accuracy the dense constellations
 *    need.  Over LS_LEN symbols of TRN (known once decided) the
 *    normal equations are collected, then solved for the equalizer.
 */
static float eq_delay(const struct v34_rx *r);
static void timing_fix(struct v34_rx *r);

#define LS_LEN          1500

static void ls_collect(struct v34_rx *r, cplx d)
{
	const int n = V34_EQ_LEN;
	const cplx *x = r->qam.eq_buf;
	/* the target before the carrier rotation */
	cplx t = cplx_mulc(d, r->qam.rot);
	int i, j;
	for (i = 0; i < n; i++) {
		double xr = x[i].re, xi = -x[i].im;     /* conj(x_i) */
		lscplx *row = r->ls_r + i * n;
		for (j = i; j < n; j++) {
			row[j].re += xr * x[j].re - xi * x[j].im;
			row[j].im += xr * x[j].im + xi * x[j].re;
		}
		r->ls_p[i].re += xr * t.re - xi * t.im;
		r->ls_p[i].im += xr * t.im + xi * t.re;
	}
}

/* R w = p by Cholesky (R Hermitian, upper triangle collected) */
static int ls_solve(struct v34_rx *r)
{
	const int n = V34_EQ_LEN;
	lscplx *a = r->ls_r, *p = r->ls_p, w[V34_EQ_LEN];
	double load = 0;
	int i, j, k;

	for (i = 0; i < n; i++)
		load += a[i * n + i].re;
	load = 1e-4 * load / n;
	for (i = 0; i < n; i++) {
		a[i * n + i].re += load;
		for (j = 0; j < i; j++) {       /* lower triangle */
			a[i * n + j].re = a[j * n + i].re;
			a[i * n + j].im = -a[j * n + i].im;
		}
	}
	/* a = L L^H, L in the lower triangle */
	for (j = 0; j < n; j++) {
		double s = a[j * n + j].re;
		for (k = 0; k < j; k++)
			s -= a[j * n + k].re * a[j * n + k].re +
			     a[j * n + k].im * a[j * n + k].im;
		if (s <= 0)
			return -1;
		s = sqrt(s);
		a[j * n + j].re = s;
		a[j * n + j].im = 0;
		for (i = j + 1; i < n; i++) {
			double re = a[i * n + j].re, im = a[i * n + j].im;
			for (k = 0; k < j; k++) {
				/* - L_ik conj(L_jk) */
				re -= a[i * n + k].re * a[j * n + k].re +
				      a[i * n + k].im * a[j * n + k].im;
				im -= a[i * n + k].im * a[j * n + k].re -
				      a[i * n + k].re * a[j * n + k].im;
			}
			a[i * n + j].re = re / s;
			a[i * n + j].im = im / s;
		}
	}
	/* L y = p */
	for (i = 0; i < n; i++) {
		double re = p[i].re, im = p[i].im;
		for (k = 0; k < i; k++) {
			re -= a[i * n + k].re * w[k].re - a[i * n + k].im * w[k].im;
			im -= a[i * n + k].re * w[k].im + a[i * n + k].im * w[k].re;
		}
		w[i].re = re / a[i * n + i].re;
		w[i].im = im / a[i * n + i].re;
	}
	/* L^H w = y */
	for (i = n - 1; i >= 0; i--) {
		double re = w[i].re, im = w[i].im;
		for (k = i + 1; k < n; k++) {
			/* conj(L_ki) w_k */
			re -= a[k * n + i].re * w[k].re + a[k * n + i].im * w[k].im;
			im -= a[k * n + i].re * w[k].im - a[k * n + i].im * w[k].re;
		}
		w[i].re = re / a[i * n + i].re;
		w[i].im = im / a[i * n + i].re;
	}
	for (i = 0; i < n; i++)
		r->qam.eq[i] = cplx_make((float)w[i].re, (float)w[i].im);
	return 0;
}

/* phase 3: two solutions LS_GAP symbols apart measure the sample clock
   offset (the timing is fixed from the first on); phase 4: one more */
#define LS_GAP          1600

static void ls_window(struct v34_rx *r, cplx d, long start)
{
	if (r->ls_n == start) {
		memset(r->ls_r, 0, V34_EQ_LEN * V34_EQ_LEN * sizeof(*r->ls_r));
		memset(r->ls_p, 0, V34_EQ_LEN * sizeof(*r->ls_p));
	}
	if (r->ls_n >= start && r->ls_n < start + LS_LEN)
		ls_collect(r, d);
	if (r->ls_n == start + LS_LEN) {
		float before = r->mse;
		long sym = r->qam.symbols - LS_LEN / 2;
		if (ls_solve(r))
			return;
		qam_rx_recompute(&r->qam);
		r->have_ls = 1;
		/* the error of the new solution from here on */
		memset(r->err_r, 0, sizeof(r->err_r));
		memset(r->err_h, 0, sizeof(r->err_h));
		r->err_n = 0;
		r->c0 = eq_delay(r);
		/* the sample clock offset from the delays of successive
		   solutions: the input comes later when the equalizer delay
		   goes down */
		if (r->ls_sym)
			r->t_freq -= (r->c0 - r->ls_d) / (sym - r->ls_sym);
		r->ls_d = r->c0;
		r->ls_sym = sym;
		DSP_DBG("v34 rx: least squares equalizer (mse was %.5f), "
			"delay %.3f, clock %.2e\n", before, r->c0, r->t_freq);
	}
}

static void ls_train(struct v34_rx *r, cplx d)
{
	r->ls_n++;
	if (!r->phase4_ls) {
		if (r->ls_n == 800 && !r->timing_fixed)
			timing_fix(r);
		if (r->timing_fixed) {
			ls_window(r, d, 800);
			ls_window(r, d, 800 + LS_GAP);
			if (r->ls_n == 800 + LS_GAP + LS_LEN)
				r->phase4_ls = 1;
		}
	} else
		ls_window(r, d, 1200);
}

/* the delay of the equalizer, in samples: the phase slope of its
   response between -baud/4 and +baud/4 */
static float eq_delay(const struct v34_rx *r)
{
	cplx hp = { 0, 0 }, hm = { 0, 0 }, c;
	float w = 2 * (float)M_PI * 0.25f / 2;  /* per T/2 tap */
	int i;
	for (i = 0; i < r->qam.eq_len; i++) {
		c = cplx_make(cosf(w * i), sinf(w * i));
		hp = cplx_add(hp, cplx_mulc(r->qam.eq[i], c));
		hm = cplx_add(hm, cplx_mul(r->qam.eq[i], c));
	}
	/* phase(H(f)) - phase(H(-f)) = -2 pi 2f delay */
	return -atan2f(cplx_mulc(hp, hm).im, cplx_mulc(hp, hm).re) /
		(2 * (float)M_PI * 0.5f) * r->qam.sps;
}

/* trained: from now on the timing follows the equalizer, the
   Gardner detector is too noisy for the dense constellations */
static void timing_fix(struct v34_rx *r)
{
	r->timing_fixed = 1;
	r->c0 = eq_delay(r);
	r->t_freq = 0;
}

/* decision directed timing: the error against the slope of the
   equalizer output over time (from the T/2 samples), for any
   constellation; d is the decision */
static void timing_dd(struct v34_rx *r, cplx d)
{
	cplx zd = { 0, 0 }, e;
	float dt, p;
	int i;
	if (!r->timing_fixed || !r->have_ls)
		return;
	for (i = 1; i < r->qam.eq_len - 1; i++) {
		cplx x = cplx_sub(r->qam.eq_buf[i - 1], r->qam.eq_buf[i + 1]);
		zd = cplx_add(zd, cplx_mul(r->qam.eq[i], x));
	}
	/* per sample */
	zd = cplx_scale(cplx_mul(zd, r->qam.rot), 1 / r->qam.sps);
	p = cplx_pow(zd);
	if (p < 1e-6f)
		return;
	e = cplx_sub(d, r->qam.z);
	dt = (e.re * zd.re + e.im * zd.im) / p;
	if (dt > 0.2f)
		dt = 0.2f;
	else if (dt < -0.2f)
		dt = -0.2f;
	r->t_freq += 1e-5f * dt;
	r->qam.t_next += 0.01f * dt;
}


/* whether the equalizer input is at the trained level: normalized
   LMS steps grow without bounds on a fading signal */
static int rx_level_ok(struct v34_rx *r)
{
	float e = 0;
	int i;
	for (i = 0; i < r->qam.eq_len; i++)
		e += cplx_pow(r->qam.eq_buf[i]);
	if (r->qam.power < 0.5f * r->ref_power)
		return 0;
	if (r->x_ref == 0)
		r->x_ref = e;
	if (e < 0.5f * r->x_ref)
		return 0;
	r->x_ref += 0.002f * (e - r->x_ref);
	return 1;
}

static void precode_eq(struct v34_rx *r, const struct v34_dparm *p);

/* the autocorrelation of the equalizer's error, for the precoder */
static void err_collect(struct v34_rx *r, cplx e)
{
	int k;
	r->err_r[0] += cplx_pow(e);
	for (k = 1; k <= 3; k++) {
		cplx c = cplx_mulc(e, r->err_h[k - 1]);
		r->err_r[2 * k] += c.re;
		r->err_r[2 * k + 1] += c.im;
	}
	r->err_h[2] = r->err_h[1];
	r->err_h[1] = r->err_h[0];
	r->err_h[0] = e;
	r->err_n++;
}
static cplx precode_start(struct v34_rx *r, cplx z);

static void rx_symbol(struct v34_rx *r)
{
	cplx z = r->qam.z, d;
	float p, a, b, e;

	r->sym++;
	r->n++;
	if (dsplibs_debug_level >= 5 && r->sym % 500 == 0)
		DSP_DBG("v34 rx%d: %ld st %d mse %.5f d %.3f/%.3f f %.2e "
			"dth %.2e tadj %.2e\n", r->tap, r->sym, r->stage, r->mse,
			eq_delay(r), r->c0, r->t_freq, r->qam.dtheta,
			r->qam.t_adj);
	p = cplx_pow(z) + cplx_pow(r->z2) + 1e-3f;
	a = cplx_pow(cplx_sub(z, r->z2));
	b = cplx_pow(cplx_add(z, r->z2));
	r->s_score += 0.25f * (a / p - r->s_score);
	r->z2 = r->z1;
	r->z1 = z;

	switch (r->stage) {
	case V34_RX_IDLE:
		if (dsplibs_debug_level >= 7)
			DSP_DBG("v34 rx%d idle %ld z %.2f %.2f s %.3f pw %.0f/%.0f\n",
				r->tap, r->sym, z.re, z.im, r->s_score, r->qam.power,
				r->ref_power);
		/* S: the symbols repeat every other symbol (and are there:
		   silence repeats as well) */
		if (r->s_score < 0.05f && cplx_pow(z) > 0.5f &&
		    cplx_pow(r->z2) > 0.5f) {
			if (++r->s_count >= 24) {
				r->ev_s = 1;
				DSP_DBG("v34 rx: S at symbol %ld\n", r->sym);
				rx_set_stage(r, V34_RX_S);
			}
		} else
			r->s_count = 0;
		break;
	case V34_RX_S:
		/* S-bar: reversed against two symbols ago */
		if (b < 0.15f * p && cplx_pow(z) > 0.5f) {
			rx_set_stage(r, V34_RX_SBAR);
			r->sbar_count++;
		}
		break;
	case V34_RX_SBAR:
		if (r->n == 16) {
			rx_set_stage(r, r->phase4 ? V34_RX_TRN : V34_RX_PP);
			r->idx = nearest4(z);
			if (r->phase4 && r->have_good) {
				/* the same channel as in phase 3 */
				memcpy(r->qam.eq, r->good_eq, sizeof(r->qam.eq));
				r->mse = 0.05f;
			}
		}
		break;
	case V34_RX_PP:
		if (r->n <= 48) {
			/* find where PP really starts (S-bar is seen a symbol
			   or two late) and its phase */
			r->pp_buf[r->n - 1] = z;
			if (r->n == 48)
				pp_align(r);
			break;
		}
		d = cplx_scale(v34_pp(r->n - 1 + r->pp_off), 1.41421f);
		qam_rx_adapt(&r->qam, d, 0.02f, 0.05f, 0.0005f);
		if (r->n == 288 - r->pp_off) {
			rx_set_stage(r, V34_RX_TRN);
			r->phase4 = 1;
			/* the level stays, silence before phase 4 must not
			   upset the gain */
			r->ref_power = r->qam.power;
			r->qam.agc_hold = 1;
		}
		break;
	case V34_RX_TRN:
		/* V.90: S straight after TRN (scrambled TRN repeats
		   every other symbol only by chance) */
		if (r->s_in_trn && r->s_score < 0.05f) {
			if (++r->s_count >= 24) {
				r->ev_s = 1;
				DSP_DBG("v34 rx: S in TRN at symbol %ld\n", r->sym);
				rx_set_stage(r, V34_RX_S);
				break;
			}
		} else
			r->s_count = 0;
		d = v34_trn4[nearest4(z)];
		e = cplx_pow(cplx_sub(z, d)) / 2;
		r->mse += 0.01f * (e - r->mse);
		/* not on the fading end of a signal; smaller steps once
		   trained, the data constellations are dense */
		if (rx_level_ok(r)) {
			/* finer once the equalizer was solved */
			if (r->timing_fixed && r->have_ls) {
				timing_dd(r, d);
				qam_rx_adapt(&r->qam, d, 0.01f, 0.02f, 0.0002f);
			}
			else
				qam_rx_adapt(&r->qam, d, 0.01f, 0.05f, 0.001f);
			if (r->n == 1000) {
				r->qam.kt_p *= 0.2f;
				r->qam.kt_gi *= 0.2f;
			}
		}
		if (r->mse < 0.02f && (r->n & 255) == 0) {
			memcpy(r->good_eq, r->qam.eq, sizeof(r->good_eq));
			r->have_good = 1;
		}
		ls_train(r, d);
		if (r->have_ls && rx_level_ok(r))
			err_collect(r, cplx_sub(z, d));
		r->last_d[2] = r->last_d[1];
		r->last_d[1] = r->last_d[0];
		r->last_d[0] = d;
		rx_diff(r, z);
		/* E is over: the next symbol is data */
		if (r->stage == V34_RX_DATA && r->data_on && r->drx.p.precode)
			precode_eq(r, &r->drx.p);
		break;
	case V34_RX_DATA:
		if (r->data_on) {
			float g2 = r->drx.p.g * r->drx.p.g;
			if (r->drx.p.precode && r->n <= 3)
				z = precode_start(r, z);
			d = v34_drx_symbol(&r->drx, z, &r->scr, r->tap);
			if (!rx_level_ok(r))
				break;
			if (dsplibs_debug_level >= 8)
				DSP_DBG("v34 rx%d: data %ld %.5f %.5f %.5f %.5f\n",
					r->tap, r->n, z.re, z.im, d.re, d.im);
			e = cplx_pow(cplx_sub(z, d)) / g2;
			r->mse += 0.01f * (e - r->mse);
			timing_dd(r, d);
			qam_rx_adapt(&r->qam, d, 0.004f, 0.02f, 0.0002f);
			if (r->n % 2000 == 0)
				DSP_DBG("v34 rx%d: data mse %.3f\n", r->tap, r->mse);
		}
		break;
	}
}

/* with precoding the equalizer gets the response 1 + H(z): its output is
   the channel output y(n) of the remote's precoder plus whitened noise */
static void precode_eq(struct v34_rx *r, const struct v34_dparm *p)
{
	cplx eq[QAM_MAX_EQ];
	int i, k, len = r->qam.eq_len + 6;

	if (len > QAM_MAX_EQ)
		len = QAM_MAX_EQ;
	for (i = 0; i < len; i++) {
		eq[i] = i < r->qam.eq_len ? r->qam.eq[i] : cplx_make(0, 0);
		for (k = 1; k <= 3; k++) {
			cplx h = cplx_make(p->h[2 * k - 2] / 16384.0f,
					   p->h[2 * k - 1] / 16384.0f);
			if (i - 2 * k >= 0 && i - 2 * k < r->qam.eq_len)
				eq[i] = cplx_add(eq[i],
						 cplx_mul(h, r->qam.eq[i - 2 * k]));
		}
	}
	memcpy(r->qam.eq, eq, len * sizeof(cplx));
	r->qam.eq_len = len;
}

/* the precoder starts from zeros with B1: not the symbols before */
static cplx precode_start(struct v34_rx *r, cplx z)
{
	const struct v34_dparm *p = &r->drx.p;
	int k, n = (int)r->n - 1;

	for (k = n + 1; k <= 3; k++) {
		cplx h = cplx_make(p->h[2 * k - 2] / 16384.0f,
				   p->h[2 * k - 1] / 16384.0f);
		z = cplx_sub(z, cplx_mul(h, r->last_d[k - n - 1]));
	}
	return z;
}

/* the precoding coefficients to ask for (9.6.2/V.34): h(k) = -a(k), with
   a(k) the third order linear predictor of the equalizer's error, so
   that the error through 1 + H(z) is white.  Returns the gain in dB of
   the decisions' SNR (0 when there is too little data to tell). */
float v34_rx_precoder(const struct v34_rx *r, int h[6])
{
	typedef struct { double re, im; } dc;
	dc R[4], A[3][4], a[3];
	double eps, s;
	int i, j, k;

	memset(h, 0, 6 * sizeof(int));
	if (r->err_n < 500 || r->err_r[0] <= 0)
		return 0;
	for (k = 0; k < 4; k++) {
		R[k].re = r->err_r[2 * k] / r->err_n;
		R[k].im = k ? r->err_r[2 * k + 1] / r->err_n : 0;
	}
	/* sum over k of a(k) R(j - k) = R(j), j = 1..3; R(-m) = R(m)* */
	for (j = 0; j < 3; j++) {
		for (k = 0; k < 3; k++) {
			int m = j - k;
			A[j][k].re = R[m < 0 ? -m : m].re;
			A[j][k].im = m < 0 ? -R[-m].im : R[m].im;
		}
		A[j][3] = R[j + 1];
	}
	for (i = 0; i < 3; i++) {
		int piv = i;
		for (j = i + 1; j < 3; j++)
			if (A[j][i].re * A[j][i].re + A[j][i].im * A[j][i].im >
			    A[piv][i].re * A[piv][i].re + A[piv][i].im * A[piv][i].im)
				piv = j;
		for (k = 0; k < 4; k++) {
			dc t = A[i][k];
			A[i][k] = A[piv][k];
			A[piv][k] = t;
		}
		s = A[i][i].re * A[i][i].re + A[i][i].im * A[i][i].im;
		if (s <= 0)
			return 0;
		for (j = 0; j < 3; j++) {
			dc f;
			if (j == i)
				continue;
			/* f = A[j][i] / A[i][i] */
			f.re = (A[j][i].re * A[i][i].re + A[j][i].im * A[i][i].im) / s;
			f.im = (A[j][i].im * A[i][i].re - A[j][i].re * A[i][i].im) / s;
			for (k = i; k < 4; k++) {
				A[j][k].re -= f.re * A[i][k].re - f.im * A[i][k].im;
				A[j][k].im -= f.re * A[i][k].im + f.im * A[i][k].re;
			}
		}
	}
	for (i = 0; i < 3; i++) {
		s = A[i][i].re * A[i][i].re + A[i][i].im * A[i][i].im;
		a[i].re = (A[i][3].re * A[i][i].re + A[i][3].im * A[i][i].im) / s;
		a[i].im = (A[i][3].im * A[i][i].re - A[i][3].re * A[i][i].im) / s;
	}
	/* |y(n)| <= 255 (9.6.2/V.34) with points up to 45 and |x(n)| up to
	   47 on each axis: at most 4 for the sum of |h| over the components */
	s = 0;
	for (k = 0; k < 3; k++)
		s += fabs(a[k].re) + fabs(a[k].im);
	if (s > 4)
		for (k = 0; k < 3; k++) {
			a[k].re *= 4 / s;
			a[k].im *= 4 / s;
		}
	for (k = 0; k < 3; k++) {
		double hr = -a[k].re, hi = -a[k].im;
		hr = hr > 1.99 ? 1.99 : hr < -1.99 ? -1.99 : hr;
		hi = hi > 1.99 ? 1.99 : hi < -1.99 ? -1.99 : hi;
		h[2 * k] = (int)lrint(hr * 16384);
		h[2 * k + 1] = (int)lrint(hi * 16384);
		a[k].re = -h[2 * k] / 16384.0;
		a[k].im = -h[2 * k + 1] / 16384.0;
	}
	/* the error power through 1 + H(z) with the coefficients sent:
	   R(0) - 2 Re sum a(k)* R(k) + sum a(j) a(k)* R(k - j) */
	eps = R[0].re;
	for (k = 0; k < 3; k++)
		eps -= 2 * (a[k].re * R[k + 1].re + a[k].im * R[k + 1].im);
	for (j = 0; j < 3; j++)
		for (k = 0; k < 3; k++) {
			int m = k - j;
			dc rr = R[m < 0 ? -m : m], p;
			if (m < 0)
				rr.im = -rr.im;
			/* a(j) conj(a(k)) */
			p.re = a[j].re * a[k].re + a[j].im * a[k].im;
			p.im = a[j].im * a[k].re - a[j].re * a[k].im;
			eps += p.re * rr.re - p.im * rr.im;
		}
	if (eps <= 0)
		return 0;
	return (float)(10 * log10(R[0].re / eps));
}

void v34_rx_data(struct v34_rx *r, const struct v34_dparm *p,
		 void (*put_bits)(void *, const unsigned char *, int), void *ctx)
{
	v34_drx_init(&r->drx, p);
	r->drx.put_bits = put_bits;
	r->drx.ctx = ctx;
	r->mse = 0;
	r->data_on = 1;
}

void v34_rx_sample(struct v34_rx *r, float x)
{
	int on;
	/* the timing must not wander on the noise between the signals */
	r->qam.timing_freeze = !r->on || r->timing_fixed;
	if (!qam_rx_sample(&r->qam, x))
		return;
	/* the sample clock offset, also through the silences */
	if (r->timing_fixed)
		r->qam.t_next += r->t_freq;
	on = r->qam.power > 300.0f * 300.0f / 2 &&
		(!r->ref_power || r->qam.power > 0.25f * r->ref_power);
	if (r->on && !on && r->stage != V34_RX_IDLE && r->stage != V34_RX_DATA) {
		DSP_DBG("v34 rx%d: signal lost at symbol %ld\n", r->tap, r->sym);
		rx_set_stage(r, V34_RX_IDLE);
		r->s_count = 0;
	}
	r->on = on;
	if (on)
		rx_symbol(r);
}
