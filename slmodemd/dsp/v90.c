/*
 *
 *    v90.c - V.90 digital modem (ITU-T V.90): the PCM transmitter
 *
 *    The digital modem's signals from phase 3 on are G.711 codewords at
 *    8000 Hz, given here by their linear values (the exact values of the
 *    codewords, so that the network's encoder gives them back).  Codewords
 *    are named by their Ucodes (Table 1/V.90): 0 is the smallest
 *    magnitude, 127 the largest.
 *
 *    Phase 3:
 *    Sd       64 times +W +0 +W -W -0 -W (W: Ucode 16 + UINFO), Sd-bar 8
 *             times the opposite; its first symbol is data frame
 *             interval 0
 *    TRN1d    UINFO with the signs of scrambled ones (GPC, cleared first)
 *    Jd, Jd'  bits scrambled and coded differentially into the signs of
 *             UINFO
 *    DIL      the segments the analogue modem asks for in Ja
 *
 */

#include <string.h>

#include "v90.h"

/* G.711 decoders */
static int ulaw2lin(int u)
{
	int t;
	u = ~u & 0xFF;
	t = ((u & 15) << 3) + 0x84;
	t <<= (u & 0x70) >> 4;
	return (u & 0x80) ? 0x84 - t : t - 0x84;
}

static int alaw2lin(int a)
{
	int t, seg;
	a ^= 0x55;
	t = (a & 15) << 4;
	seg = (a & 0x70) >> 4;
	if (seg == 0)
		t += 8;
	else if (seg == 1)
		t += 0x108;
	else
		t = (t + 0x108) << (seg - 1);
	return (a & 0x80) ? t : -t;
}

/* Table 1/V.90: mu-law 0xFF - Ucode, A-law Ucode ^ 0xD5 for the positive
   codewords; the sign bit is the MSB */
int v90_ucode_octet(int alaw, int ucode, int positive)
{
	int c = alaw ? (ucode ^ 0x55) | 0x80 : 0xFF - ucode;
	return positive ? c : c & 0x7F;
}

int v90_ucode_linear(int alaw, int ucode)
{
	int c = v90_ucode_octet(alaw, ucode, 1);
	return alaw ? alaw2lin(c) : ulaw2lin(c);
}

static int scramble(unsigned *reg, int bit)
{
	/* GPC: 1 + x^-18 + x^-23 */
	int out = (bit ^ (*reg >> 17) ^ (*reg >> 22)) & 1;
	*reg = (*reg << 1) | out;
	return out;
}

static void put_bits(unsigned char *b, int *n, unsigned val, int nbits)
{
	int i;
	for (i = 0; i < nbits; i++)
		b[(*n)++] = (val >> i) & 1;
}

/* Jd (Table 13/V.90): all the rates from 28 000 to 56 000 bit/s, CP, E
   and SCR on 4 points, look-ahead up to 1 */
static void make_jd(struct v90_tx *t)
{
	unsigned crc = 0xFFFF;
	int n = 0, i;

	put_bits(t->bits, &n, 0x1FFFF, 17);
	put_bits(t->bits, &n, 0, 1);
	put_bits(t->bits, &n, 0xFFFF, 16);      /* 28 000 to 48 000 */
	put_bits(t->bits, &n, 0, 1);
	put_bits(t->bits, &n, 0x7F, 6);         /* 49 333 to 56 000 */
	put_bits(t->bits, &n, 0, 6);
	put_bits(t->bits, &n, 0, 1);            /* 4 points in training */
	put_bits(t->bits, &n, 0, 1);            /* and in renegotiation */
	put_bits(t->bits, &n, 1, 2);            /* look-ahead */
	put_bits(t->bits, &n, 0, 1);
	for (i = 18; i < 51; i += 17)
		crc = v34_crc_bits(t->bits + i, 16, crc);
	for (i = 15; i >= 0; i--)
		t->bits[n++] = (crc >> i) & 1;
	put_bits(t->bits, &n, 0, 4);
	t->nbits = n;
}

void v90_tx_init(struct v90_tx *t, int alaw, int uinfo)
{
	int i;
	memset(t, 0, sizeof(*t));
	t->alaw = alaw;
	for (i = 0; i < 128; i++)
		t->lin[i] = v90_ucode_linear(alaw, i);
	t->uinfo = uinfo;
	t->seg = V90_SILENCE;
	t->sign = 1;
}

void v90_tx_segment(struct v90_tx *t, enum v90_seg seg)
{
	t->seg = seg;
	t->n = 0;
	switch (seg) {
	case V90_SD:
		t->sym = 0;
		break;
	case V90_TRN1D:
		t->scr = 0;
		break;
	case V90_JD:
		make_jd(t);
		t->pos = 0;
		t->jd_end = 0;
		t->jd_frames = 0;
		break;
	case V90_JDP:
		t->pos = 0;
		break;
	case V90_RI:
		t->ri_end = 0;
		break;
	case V90_ENC:
		t->opos = 6;
		break;
	case V90_DIL:
		t->dil_seg = t->dil_k = 0;
		t->dil_end = 0;
		t->dil_rounds = 0;
		break;
	default:
		break;
	}
}

void v90_tx_jd_end(struct v90_tx *t)
{
	t->jd_end = 1;
}

void v90_tx_dil_end(struct v90_tx *t)
{
	t->dil_end = 1;
}

void v90_tx_ri_end(struct v90_tx *t)
{
	t->ri_end = 1;
}

/* a bit, scrambled and coded into the sign of UINFO */
static int diff_bit(struct v90_tx *t, int bit)
{
	t->sign ^= scramble(&t->scr, bit);
	return t->sign ? t->lin[t->uinfo] : -t->lin[t->uinfo];
}

static int dil_sample(struct v90_tx *t)
{
	const struct v90_dil *d = &t->dil;
	int u = d->ucode[t->dil_seg], c = u >> 4, len = (d->h[c] + 1) * 6;
	int k = t->dil_k, x;

	x = d->tp[k % d->ltp] ? t->lin[u] : t->lin[d->ref[c]];
	if (!d->sp[k % d->lsp])
		x = -x;
	if (++t->dil_k >= len) {
		t->dil_k = 0;
		if (++t->dil_seg >= d->n) {
			t->dil_seg = 0;
			t->dil_rounds++;
		}
		if (t->dil_end) {
			t->seg = V90_SILENCE;
			t->next = 1;
		}
	}
	return x;
}

int v90_tx_sample(struct v90_tx *t)
{
	static const signed char sd[6] = { 1, 0, 1, -1, 0, -1 };
	int x = 0, w = t->lin[(16 + t->uinfo) & 127], z = t->lin[0];

	switch (t->seg) {
	case V90_SILENCE:
		break;
	case V90_SD:
	case V90_SDBAR: {
		int k = t->sym % 6, s = sd[k] ? sd[k] : (k < 3 ? 1 : -1);
		x = (sd[k] ? w : z) * (t->seg == V90_SD ? s : -s);
		if (t->seg == V90_SD && t->n + 1 == 384)
			v90_tx_segment(t, V90_SDBAR);
		else if (t->seg == V90_SDBAR && t->n + 1 == 48)
			v90_tx_segment(t, V90_TRN1D);
		else
			t->n++;
		t->sym++;
		return x;
	}
	case V90_TRN1D:
		t->sign = scramble(&t->scr, 1);
		x = t->sign ? t->lin[t->uinfo] : -t->lin[t->uinfo];
		if (t->trn1d_len && t->n + 1 >= t->trn1d_len &&
		    (t->sym + 1) % 6 == 0) {
			v90_tx_segment(t, V90_JD);
			t->n--;
		}
		break;
	case V90_JD:
		x = diff_bit(t, t->bits[t->pos]);
		if (++t->pos == t->nbits) {
			t->pos = 0;
			t->jd_frames++;
			if (t->jd_end) {
				v90_tx_segment(t, V90_JDP);
				t->n--;
			}
		}
		break;
	case V90_JDP:
		x = diff_bit(t, 0);
		if (++t->pos == 12) {
			if (t->dil.n)
				v90_tx_segment(t, V90_DIL);
			else {
				t->seg = V90_SILENCE;
				t->next = 1;
			}
			t->n--;
		}
		break;
	case V90_DIL:
		x = dil_sample(t);
		break;
	case V90_RI:
	case V90_RIBAR: {
		/* + + + - - - of UINFO, Ri-bar the opposite */
		int k = (int)(t->sym % 6), pos = k < 3;
		if (t->seg == V90_RIBAR)
			pos = !pos;
		x = pos ? t->lin[t->uinfo] : -t->lin[t->uinfo];
		if (t->seg == V90_RI && t->ri_end && k == 5) {
			v90_tx_segment(t, V90_RIBAR);
			t->n--;
		} else if (t->seg == V90_RIBAR && t->n + 1 == 24) {
			v90_tx_segment(t, V90_ENC);
			t->n--;
		}
		break;
	}
	case V90_ENC:
		if (t->opos >= 6) {
			/* frames go in ahead of the shaper's decisions */
			while (!v90_enc_pop(&t->enc, t->out)) {
				unsigned char bits[48];
				int last = 0;
				t->get_frame(t->ctx, t, bits, &last);
				v90_enc_push(&t->enc, bits, last);
			}
			t->opos = 0;
		}
		x = t->out[t->opos++];
		break;
	}
	t->n++;
	t->sym++;
	return x;
}

/*
 *    the DIL descriptor (Table 12/V.90): 16 bit fields after start bits,
 *    every 17 bits from bit 17 on
 */

static int field(const unsigned char *b, int first, int nbits)
{
	int i, v = 0;
	for (i = nbits - 1; i >= 0; i--)
		v = (v << 1) | b[first + i];
	return v;
}

/* bits of a pattern of len bits from first on, past the start bits */
static void pattern(unsigned char *p, const unsigned char *b, int first,
		    int len)
{
	int i;
	for (i = 0; i < len; i++)
		p[i] = b[first + 17 * (i / 16) + i % 16];
}

static int dil_beta(const unsigned char *b, int *alpha)
{
	int lsp = field(b, 35, 7) + 1, ltp = field(b, 43, 7) + 1;
	*alpha = (lsp + 15) / 16 * 17;
	return *alpha + (ltp + 15) / 16 * 17;
}

int v90_dil_length(const unsigned char *b, int n)
{
	int alpha, beta;
	if (n < 50)
		return -1;
	beta = dil_beta(b, &alpha);
	/* up to the end of the CRC */
	return 204 + beta + (field(b, 18, 8) + 1) / 2 * 17;
}

int v90_dil_parse(struct v90_dil *d, const unsigned char *b, int len)
{
	unsigned crc = 0xFFFF;
	int alpha, beta, i, end = len - 16;

	for (i = 18; i < end - 1; i += 17)
		crc = v34_crc_bits(b + i, 16, crc);
	for (i = 0; i < 16; i++)
		if (b[end + i] != ((crc >> (15 - i)) & 1))
			return -1;
	beta = dil_beta(b, &alpha);
	d->n = field(b, 18, 8);
	d->lsp = field(b, 35, 7) + 1;
	d->ltp = field(b, 43, 7) + 1;
	pattern(d->sp, b, 52, d->lsp);
	pattern(d->tp, b, 52 + alpha, d->ltp);
	for (i = 0; i < 8; i++) {
		/* two 7 bit fields per 17 bit block */
		int o = 52 + beta + 17 * (i / 2) + 8 * (i % 2);
		d->h[i] = field(b, o, 7);
		d->ref[i] = field(b, o + 68, 7);
	}
	for (i = 0; i < d->n; i++)
		d->ucode[i] = field(b, 188 + beta + 17 * (i / 2) + 8 * (i % 2), 7);
	return 0;
}

/*
 *    CP (Table 14/V.90)
 */

static int cp_delta(const unsigned char *b)
{
	int i, maxidx = 0, gamma;
	static const int con_bit[6] = { 103, 107, 111, 115, 120, 124 };
	for (i = 0; i < 6; i++)
		if (field(b, con_bit[i], 4) > maxidx)
			maxidx = field(b, con_bit[i], 4);
	if (maxidx > 5)
		return -1;
	gamma = 136 * maxidx;
	return b[128] ? 2 * gamma + 136 : gamma;
}

int v90_cp_length(const unsigned char *b, int n)
{
	int delta;
	if (n < 129)
		return -1;
	delta = cp_delta(b);
	/* up to the end of the CRC */
	return delta < 0 ? 0 : 289 + delta;
}

int v90_cp_parse(struct v90_cp *cp, const unsigned char *b, int len)
{
	static const int con_bit[6] = { 103, 107, 111, 115, 120, 124 };
	unsigned crc = 0xFFFF;
	int i, j, c, end = len - 16;

	for (i = 18; i < end - 1; i += 17)
		crc = v34_crc_bits(b + i, 16, crc);
	for (i = 0; i < 16; i++)
		if (b[end + i] != ((crc >> (15 - i)) & 1))
			return -1;
	memset(cp, 0, sizeof(*cp));
	cp->cp = b[19];
	cp->drn = field(b, 20, 5);
	cp->silence = b[30];
	cp->sr = field(b, 31, 2);
	cp->ack = b[33];
	cp->alaw = b[35];
	cp->rate_mask = field(b, 36, 13);
	cp->ld = field(b, 49, 2);
	cp->a1 = (signed char)field(b, 69, 8);
	cp->a2 = (signed char)field(b, 77, 8);
	cp->b1 = (signed char)field(b, 86, 8);
	cp->b2 = (signed char)field(b, 94, 8);
	for (i = 0; i < 6; i++) {
		cp->con[i] = field(b, con_bit[i], 4);
		if (cp->con[i] + 1 > cp->ncon)
			cp->ncon = cp->con[i] + 1;
	}
	for (j = 0; j < cp->ncon; j++)
		for (c = 0; c < 8; c++)
			for (i = 0; i < 16; i++)
				cp->mask[j][16 * c + i] =
					b[136 + 136 * j + 17 * c + 1 + i];
	return 0;
}

/*
 *    MP of the digital modem (Table 16/V.90)
 */

int v90_mp_build(unsigned char *b, int drn_max, int trellis, int expanded,
		 int ack, int rate_mask, const int *h)
{
	int n = 0, i, type = 0;
	unsigned crc = 0xFFFF;

	for (i = 0; i < 6; i++)
		if (h && h[i])
			type = 1;
	put_bits(b, &n, 0x1FFFF, 17);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, type, 1);
	put_bits(b, &n, 0, 5);
	put_bits(b, &n, drn_max, 4);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, trellis, 2);
	put_bits(b, &n, 0, 1);          /* no non-linear encoding */
	put_bits(b, &n, expanded, 1);
	put_bits(b, &n, ack, 1);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, rate_mask, 13); /* 4800 to 33 600 */
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, 0, 1);
	if (type)
		for (i = 0; i < 6; i++) {
			put_bits(b, &n, 0, 1);
			put_bits(b, &n, h[i], 16);
		}
	put_bits(b, &n, 0, 1);
	put_bits(b, &n, 0, 16);
	put_bits(b, &n, 0, 1);
	for (i = 18; i < n - 1; i += 17)
		crc = v34_crc_bits(b + i, 16, crc);
	for (i = 15; i >= 0; i--)
		b[n++] = (crc >> i) & 1;
	b[n++] = 0;
	return n;
}

/*
 *    the data mode encoder (5.4/V.90)
 */

int v90_enc_init(struct v90_enc *e, const struct v90_cp *cp, int D,
		 const int *lin)
{
	double prod = 1;
	int i, u;

	memset(e, 0, sizeof(*e));
	e->sr = cp->sr;
	e->ld = cp->ld;
	e->S = 6 - cp->sr;
	e->K = D - e->S;
	e->lin = lin;
	for (i = 0; i < 6; i++) {
		const unsigned char *m = cp->mask[cp->con[i]];
		/* label 0 is the largest code */
		for (u = 127; u >= 0; u--)
			if (m[u])
				e->ucode[i][e->M[i]++] = (unsigned char)u;
		if (!e->M[i])
			return -1;
		prod *= e->M[i];
	}
	if (e->K < 1 || e->K > 63 || prod < (double)(1ULL << e->K))
		return -1;
	e->a1 = cp->a1 / 64.0;
	e->a2 = cp->a2 / 64.0;
	e->b1 = cp->b1 / 64.0;
	e->b2 = cp->b2 / 64.0;
	return 0;
}

/* the shaping filter F(z) over a frame with the given signs, from the
   state f[3] (updated); returns the sum of v^2 */
static double shape_metric(const struct v90_enc *e, const int *mag, int n,
			   int signs, double *f)
{
	double x1 = f[0], y1 = f[1], v1 = f[2], w = 0;
	int i;
	for (i = 0; i < n; i++) {
		double x = signs >> i & 1 ? mag[i] : -mag[i];
		double y = x - e->b1 * x1 + e->a1 * y1;
		double v = y - e->b2 * y1 + e->a2 * v1;
		w += v * v;
		x1 = x;
		y1 = y;
		v1 = v;
	}
	f[0] = x1;
	f[1] = y1;
	f[2] = v1;
	return w;
}

/* the sign inversion of rule r (0, 1) from state Q: A, B from state 0;
   C, D from state 1; the next state is r */
static int rule_mask(int fs, int Q, int r)
{
	int all = (1 << fs) - 1, even = fs == 6 ? 0x15 : fs == 3 ? 5 : 1;
	if (Q == 0)
		return r ? all : 0;
	return r ? all & ~even : even;
}

/* the least metric of the rule sequences for frames k..depth from a state */
static double best_path(const struct v90_enc *e, int k, int depth, int Q,
			int tprev, const double *f)
{
	double best = 1e300;
	int r;
	for (r = 0; r < 2; r++) {
		const struct v90_shf *s = &e->q[k];
		int sg = (s->p ^ tprev) ^ rule_mask(s->fs, Q, r);
		double st[3] = { f[0], f[1], f[2] };
		double w = shape_metric(e, s->mag, s->fs, sg, st);
		if (k < depth)
			w += best_path(e, k + 1, depth, r, sg, st);
		if (w < best)
			best = w;
	}
	return best;
}

/* decide the signs of the oldest shaping frame */
static void decide_frame(struct v90_enc *e, int depth)
{
	const struct v90_shf *s = &e->q[0];
	double f[3] = { e->x1, e->y1, e->v1 }, bw = 1e300, bst[3] = { 0, 0, 0 };
	int r, best = 0, bsg = 0;

	for (r = 0; r < 2; r++) {
		int sg = (s->p ^ e->t_prev) ^ rule_mask(s->fs, e->Q, r);
		double st[3] = { f[0], f[1], f[2] };
		double w = shape_metric(e, s->mag, s->fs, sg, st);
		if (depth > 0)
			w += best_path(e, 1, depth, r, sg, st);
		if (w < bw) {
			bw = w;
			best = r;
			bsg = sg;
			memcpy(bst, st, sizeof(st));
		}
	}
	e->Q = best;
	e->t_prev = bsg;
	e->x1 = bst[0];
	e->y1 = bst[1];
	e->v1 = bst[2];
	/* into the data frame */
	memcpy(e->mag_acc + e->nsh * s->fs, s->mag, s->fs * sizeof(int));
	e->sign_acc |= bsg << (e->nsh * s->fs);
	if (++e->nsh == e->sr) {
		int i;
		for (i = 0; i < 6; i++)
			e->outq[e->outn][i] = e->sign_acc >> i & 1 ?
				e->mag_acc[i] : -e->mag_acc[i];
		e->outn++;
		e->nsh = 0;
		e->sign_acc = 0;
	}
	memmove(e->q, e->q + 1, --e->qn * sizeof(e->q[0]));
}

void v90_enc_push(struct v90_enc *e, const unsigned char *bits, int last)
{
	unsigned char d[48];
	unsigned long long r = 0;
	int D = e->S + e->K, mag[6], i;

	for (i = 0; i < D; i++)
		d[i] = (unsigned char)scramble(&e->scr, bits[i]);
	for (i = e->K - 1; i >= 0; i--)
		r = (r << 1) | d[e->S + i];
	for (i = 0; i < 6; i++) {
		int k = (int)(r % (unsigned)e->M[i]);
		r /= (unsigned)e->M[i];
		mag[i] = e->lin[e->ucode[i][k]];
	}
	if (e->sr == 0) {
		/* 5.4.5.1: differential, no shaping */
		int l = e->last;
		for (i = 0; i < 6; i++) {
			l ^= d[i];
			e->outq[e->outn][i] = l ? mag[i] : -mag[i];
		}
		e->last = l;
		e->outn++;
		return;
	} else {
		/* shaping frames of 6, 3 or 2 symbols; p' with the odd bit
		   coded differentially (Tables 3 and 4/V.90) */
		int fs = 6 / e->sr, f, k = 0;
		for (f = 0; f < e->sr; f++) {
			struct v90_shf *s = &e->q[e->qn++];
			s->fs = fs;
			s->p = 0;
			s->last = last && f == e->sr - 1;
			memcpy(s->mag, mag + f * fs, fs * sizeof(int));
			for (i = 1; i < fs; i++) {
				int bit = d[k++];
				if (i & 1) {
					bit ^= e->last;
					e->last = bit;
				}
				s->p |= bit << i;
			}
		}
	}
	/* decide what the look-ahead allows */
	while (e->qn) {
		int depth = e->ld, j;
		for (j = 0; j < e->qn && j <= e->ld; j++)
			if (e->q[j].last) {
				depth = j;
				break;
			}
		if (depth >= e->qn)
			break;
		decide_frame(e, depth);
	}
}

int v90_enc_pop(struct v90_enc *e, int out[6])
{
	if (!e->outn)
		return 0;
	memcpy(out, e->outq[0], 6 * sizeof(int));
	memmove(e->outq, e->outq + 1, --e->outn * sizeof(e->outq[0]));
	return 1;
}
