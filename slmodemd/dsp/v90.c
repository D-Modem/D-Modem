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
