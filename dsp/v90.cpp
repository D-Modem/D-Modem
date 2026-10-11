/*
 *
 *    v90.cpp - V.90 digital modem (ITU-T V.90): the PCM transmitter
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

#include <math.h>
#include <string.h>

#include "v90.h"

namespace dsp {

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

/* G.711 encoders: mu-law from 14 bit, A-law from 13 bit linear values,
   as the ANSpcm tables of V.92 (Tables 7 to 10/V.92) come out */
static int lin2ulaw(int v)
{
	int mask = 0xFF, seg;
	if (v < 0) {
		v = -v;
		mask = 0x7F;
	}
	v = (v > 8159 ? 8159 : v) + 33;
	for (seg = 0; seg < 8 && v > (0x40 << seg) - 1; seg++)
		;
	if (seg == 8)
		return 0x7F ^ mask;
	return ((seg << 4) | ((v >> (seg + 1)) & 0xF)) ^ mask;
}

static int lin2alaw(int v)
{
	int mask = 0xD5, seg, a;
	if (v < 0) {
		v = -v;
		mask = 0x55;
	}
	for (seg = 0; seg < 8 && v > (0x20 << seg) - 1; seg++)
		;
	if (seg == 8)
		return 0x7F ^ mask;
	a = seg << 4;
	a |= seg < 2 ? (v >> 1) & 0xF : (v >> seg) & 0xF;
	return a ^ mask;
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
void V90Tx::make_jd()
{
	unsigned crc = 0xFFFF;
	int n_ = 0, i;

	put_bits(bits, &n_, 0x1FFFF, 17);
	put_bits(bits, &n_, 0, 1);
	put_bits(bits, &n_, 0xFFFF, 16);      /* 28 000 to 48 000 */
	put_bits(bits, &n_, 0, 1);
	put_bits(bits, &n_, 0x7F, 6);         /* 49 333 to 56 000 */
	put_bits(bits, &n_, 0, 6);
	put_bits(bits, &n_, 0, 1);            /* 4 points in training */
	put_bits(bits, &n_, 0, 1);            /* and in renegotiation */
	put_bits(bits, &n_, 1, 2);            /* look-ahead */
	put_bits(bits, &n_, 0, 1);
	for (i = 18; i < 51; i += 17)
		crc = v34_crc_bits(bits + i, 16, crc);
	for (i = 15; i >= 0; i--)
		bits[n_++] = (crc >> i) & 1;
	put_bits(bits, &n_, 0, 4);
	nbits = n_;
}

/* Jp (Table 22/V.92): the extension of Su, TRN2u on 4 points */
void V90Tx::make_jp()
{
	unsigned crc = 0xFFFF;
	int n_ = 0, i;

	put_bits(bits, &n_, 0x1FFFF, 17);
	put_bits(bits, &n_, 0, 1);
	put_bits(bits, &n_, jp_frac & 0xFFFF, 16);
	put_bits(bits, &n_, 0, 1);
	put_bits(bits, &n_, 0, 12);
	put_bits(bits, &n_, 1, 1);            /* Jp */
	put_bits(bits, &n_, 0, 1);            /* 4 points in training */
	put_bits(bits, &n_, 0, 1);            /* and in renegotiation */
	put_bits(bits, &n_, 0, 1);
	put_bits(bits, &n_, 0, 1);
	for (i = 18; i < 51; i += 17)
		crc = v34_crc_bits(bits + i, 16, crc);
	for (i = 15; i >= 0; i--)
		bits[n_++] = (crc >> i) & 1;
	put_bits(bits, &n_, 0, 4);
	nbits = n_;
}

int v92_uqts(int wxyz)
{
	static const signed char u[16] = { 61, 62, 63, 66, 67, 70, 71, 74,
					   75, 78, 79, 82, 83, 86, 87, -1 };
	return u[wxyz & 15];
}

/* ANSpcm (8.3.1/V.92): the 301 symbols of a 2100 Hz tone at one of the
   levels of Table 6/V.92, quantized by G.711 */
void V90Tx::anspcm_init(int level)
{
	static const short scl_u[4] = { 1334, 1000, 708, 500 };
	static const short scl_a[4] = { 667, 500, 354, 250 };
	double scl = alaw ? scl_a[level & 3] : scl_u[level & 3];
	int k;
	for (k = 0; k < 301; k++) {
		double x = scl * sqrt(2.0) * cos(2 * M_PI * k * 79 / 301 +
						  0.25 * M_PI / 301) + 0.5;
		int v = (int)floor(x);
		anspcm[k] = (short)(alaw ? alaw2lin(lin2alaw(v)) :
				    ulaw2lin(lin2ulaw(v)));
	}
}

void V90Tx::init(int alaw_, int uinfo_)
{
	int i;
	memset(this, 0, sizeof(*this));
	alaw = alaw_;
	for (i = 0; i < 128; i++)
		lin[i] = v90_ucode_linear(alaw_, i);
	uinfo = uinfo_;
	seg = V90_SILENCE;
	sign = 1;
}

void V90Tx::segment(enum v90_seg seg_)
{
	seg = seg_;
	n = 0;
	switch (seg_) {
	case V90_SD:
		sym = 0;
		break;
	case V90_TRN1D:
		scr = 0;
		break;
	case V90_JD:
		make_jd();
		pos = 0;
		jd_end = 0;
		jd_frames = 0;
		jp = 0;
		break;
	case V90_SCR:
		dil_end = 0;
		break;
	case V90_JDP:
		pos = 0;
		break;
	case V90_RI:
		ri_end = 0;
		break;
	case V90_ENC:
		opos = 6;
		break;
	case V90_DIL:
		dil_seg = dil_k = 0;
		dil_end = 0;
		dil_rounds = 0;
		break;
	case V90_QTS:
		if (fclock)
			fbase = *fclock;
		break;
	default:
		break;
	}
}

void V90Tx::end_jd()
{
	jd_end = 1;
}

void V90Tx::end_dil()
{
	dil_end = 1;
}

void V90Tx::end_ri()
{
	ri_end = 1;
}

/* a bit, scrambled and coded into the sign of UINFO */
int V90Tx::diff_bit(int bit)
{
	sign ^= scramble(&scr, bit);
	return sign ? lin[uinfo] : -lin[uinfo];
}

int V90Tx::dil_sample()
{
	const V90Dil *d = &dil;
	int u = d->ucode[dil_seg], c = u >> 4, len = (d->h[c] + 1) * 6;
	int k = dil_k, x;

	x = d->tp[k % d->ltp] ? lin[u] : lin[d->ref[c]];
	if (!d->sp[k % d->lsp])
		x = -x;
	if (++dil_k >= len) {
		dil_k = 0;
		if (++dil_seg >= d->n) {
			dil_seg = 0;
			dil_rounds++;
		}
		if (dil_end) {
			seg = V90_SILENCE;
			next = 1;
		}
	}
	return x;
}

int V90Tx::sample()
{
	static const signed char sd[6] = { 1, 0, 1, -1, 0, -1 };
	int x = 0, w = lin[(16 + uinfo) & 127], z = lin[0];

	switch (seg) {
	case V90_SILENCE:
		break;
	case V90_QTS:
	case V90_QTSBAR: {
		/* as Sd, with V of Ucode UQTS: QTS 128 times, QTS-bar 8 */
		int k = (int)(n % 6), s = sd[k] ? sd[k] : (k < 3 ? 1 : -1);
		x = (sd[k] ? lin[uqts & 127] : z) *
			(seg == V90_QTS ? s : -s);
		if (seg == V90_QTS && n + 1 == 768)
			segment(V90_QTSBAR);
		else if (seg == V90_QTSBAR && n + 1 == 48)
			segment(V90_ANSPCM);
		else
			n++;
		return x;
	}
	case V90_ANSPCM:
		/* a phase reversal every 3612 symbols */
		x = anspcm[n % 301];
		if ((n / 3612) & 1)
			x = -x;
		break;
	case V90_SD:
	case V90_SDBAR: {
		/* after QTS: Sd starts in data frame interval 0 */
		if (seg == V90_SD && n == 0 && fclock &&
		    (*fclock - fbase) % 6)
			return 0;
		int k = sym % 6, s = sd[k] ? sd[k] : (k < 3 ? 1 : -1);
		x = (sd[k] ? w : z) * (seg == V90_SD ? s : -s);
		if (seg == V90_SD && n + 1 == 384)
			segment(V90_SDBAR);
		else if (seg == V90_SDBAR && n + 1 == 48)
			segment(V90_TRN1D);
		else
			n++;
		sym++;
		return x;
	}
	case V90_TRN1D:
		sign = scramble(&scr, 1);
		x = sign ? lin[uinfo] : -lin[uinfo];
		if (trn1d_len && n + 1 >= trn1d_len &&
		    (sym + 1) % 6 == 0) {
			segment(V90_JD);
			n--;
		}
		break;
	case V90_JD:
		x = diff_bit(bits[pos]);
		if (++pos == nbits) {
			pos = 0;
			jd_frames++;
			if (jd_end && v92 && !jp) {
				/* V.92: Jp until the next end_jd() */
				make_jp();
				jp = 1;
				jd_end = 0;
			} else if (jd_end) {
				segment(V90_JDP);
				n--;
			}
		}
		break;
	case V90_JDP:
		x = diff_bit(0);
		if (++pos == 12) {
			if (dil.n)
				segment(V90_DIL);
			else if (v92)
				segment(V90_SCR);
			else {
				seg = V90_SILENCE;
				next = 1;
			}
			n--;
		}
		break;
	case V90_DIL:
		x = dil_sample();
		break;
	case V90_SCR:
		/* UINFO with the signs of scrambled ones, in whole data
		   frames */
		sign = scramble(&scr, 1);
		x = sign ? lin[uinfo] : -lin[uinfo];
		if (dil_end && (sym + 1) % 6 == 0) {
			seg = V90_SILENCE;
			next = 1;
		}
		break;
	case V90_RI:
	case V90_RIBAR: {
		/* + + + - - - of UINFO, Ri-bar the opposite */
		int k = (int)(sym % 6), pos_ = k < 3;
		if (seg == V90_RIBAR)
			pos_ = !pos_;
		x = pos_ ? lin[uinfo] : -lin[uinfo];
		if (seg == V90_RI && ri_end && k == 5) {
			segment(V90_RIBAR);
			n--;
		} else if (seg == V90_RIBAR && n + 1 == 24) {
			segment(V90_ENC);
			n--;
		}
		break;
	}
	case V90_RD:
	case V90_RDBAR: {
		/* + + + - - - of each interval's largest code (Rd: of the
		   data mode constellations, Rt: the training ones), the bar
		   the opposite */
		int k = (int)(sym % 6), pos_ = k < 3;
		if (seg == V90_RDBAR)
			pos_ = !pos_;
		x = pos_ ? lin[rd_code[k]] : -lin[rd_code[k]];
		if (seg == V90_RD && n + 1 == 384) {
			segment(V90_RDBAR);
			n--;
		} else if (seg == V90_RDBAR && n + 1 == 24) {
			segment(V90_ENC);
			n--;
		}
		break;
	}
	case V90_RF:
	case V90_RFBAR: {
		/* + + - - of each interval's largest data mode code, the
		   bar the opposite (8.8.4/V.92) */
		int k = (int)(sym % 6), pos_ = (n & 3) < 2;
		if (seg == V90_RFBAR)
			pos_ = !pos_;
		x = pos_ ? lin[rd_code[k]] : -lin[rd_code[k]];
		if (seg == V90_RF && n + 1 == 384) {
			segment(V90_RFBAR);
			n--;
		} else if (seg == V90_RFBAR && n + 1 == 24) {
			segment(V90_ENC);
			n--;
		}
		break;
	}
	case V90_ENC:
		if (opos >= 6 && rf_req) {
			/* Rf (V.92's fast parameter exchange), on the
			   boundary of a data frame */
			int k;
			rf_req = 0;
			for (k = 0; k < 6; k++)
				rd_code[k] = enc.ucode[k][0];
			/* SUVd follows Rf-bar at once: no data frames from
			   before (get_frame starts the encoder again) */
			enc.outn = enc.qn = 0;
			segment(V90_RF);
			x = lin[rd_code[(int)(sym % 6)]];
			break;
		}
		if (opos >= 6 && rd_req) {
			/* Rd (V.92's rate renegotiation), on the boundary of
			   a data frame; the encoder starts again after it */
			int k;
			rd_req = 0;
			for (k = 0; k < 6; k++)
				rd_code[k] = enc.ucode[k][0];
			segment(V90_RD);
			int k2 = (int)(sym % 6), p2 = k2 < 3;
			x = p2 ? lin[rd_code[k2]] : -lin[rd_code[k2]];
			break;
		}
		if (opos >= 6 && sil_req && !enc.pop(out)) {
			/* silence, once the last frame (with no look-ahead
			   past it) is out */
			sil_req = 0;
			segment(V90_SILENCE);
			x = 0;
			break;
		} else if (opos >= 6 && sil_req)
			opos = 0;
		if (opos >= 6) {
			/* frames go in ahead of the shaper's decisions */
			while (!enc.pop(out)) {
				unsigned char bits_[48];
				int last_ = 0;
				get_frame(ctx, this, bits_, &last_);
				enc.push(bits_, last_);
			}
			opos = 0;
		}
		x = out[opos++];
		break;
	}
	n++;
	sym++;
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

int V90Dil::parse(const unsigned char *b, int len)
{
	unsigned crc = 0xFFFF;
	int alpha, beta, i, end = len - 16;

	for (i = 18; i < end - 1; i += 17)
		crc = v34_crc_bits(b + i, 16, crc);
	for (i = 0; i < 16; i++)
		if (b[end + i] != ((crc >> (15 - i)) & 1))
			return -1;
	beta = dil_beta(b, &alpha);
	n = field(b, 18, 8);
	lsp = field(b, 35, 7) + 1;
	ltp = field(b, 43, 7) + 1;
	pattern(sp, b, 52, lsp);
	pattern(tp, b, 52 + alpha, ltp);
	for (i = 0; i < 8; i++) {
		/* two 7 bit fields per 17 bit block */
		int o = 52 + beta + 17 * (i / 2) + 8 * (i % 2);
		h[i] = field(b, o, 7);
		ref[i] = field(b, o + 68, 7);
	}
	for (i = 0; i < n; i++)
		ucode[i] = field(b, 188 + beta + 17 * (i / 2) + 8 * (i % 2), 7);
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

int V90Cp::parse(const unsigned char *b, int len)
{
	static const int con_bit[6] = { 103, 107, 111, 115, 120, 124 };
	unsigned crc = 0xFFFF;
	int i, j, c, end = len - 16;

	for (i = 18; i < end - 1; i += 17)
		crc = v34_crc_bits(b + i, 16, crc);
	for (i = 0; i < 16; i++)
		if (b[end + i] != ((crc >> (15 - i)) & 1))
			return -1;
	memset(this, 0, sizeof(*this));
	cp = b[19];
	drn = field(b, 20, 5);
	silence = b[30];
	sr = field(b, 31, 2);
	ack = b[33];
	alaw = b[35];
	rate_mask = field(b, 36, 13);
	ld = field(b, 49, 2);
	a1 = (signed char)field(b, 69, 8);
	a2 = (signed char)field(b, 77, 8);
	b1 = (signed char)field(b, 86, 8);
	b2 = (signed char)field(b, 94, 8);
	for (i = 0; i < 6; i++) {
		con[i] = field(b, con_bit[i], 4);
		if (con[i] + 1 > ncon)
			ncon = con[i] + 1;
	}
	for (j = 0; j < ncon; j++)
		for (c = 0; c < 8; c++)
			for (i = 0; i < 16; i++)
				mask[j][16 * c + i] =
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

int V90Enc::init(const V90Cp *cp, int D, const int *lin_)
{
	double prod = 1;
	int i, u;

	memset(this, 0, sizeof(*this));
	sr = cp->sr;
	ld = cp->ld;
	S = 6 - cp->sr;
	K = D - S;
	lin = lin_;
	for (i = 0; i < 6; i++) {
		const unsigned char *m = cp->mask[cp->con[i]];
		/* label 0 is the largest code */
		for (u = 127; u >= 0; u--)
			if (m[u])
				ucode[i][M[i]++] = (unsigned char)u;
		if (!M[i])
			return -1;
		prod *= M[i];
	}
	if (K < 1 || K > 63 || prod < (double)(1ULL << K))
		return -1;
	a1 = cp->a1 / 64.0;
	a2 = cp->a2 / 64.0;
	b1 = cp->b1 / 64.0;
	b2 = cp->b2 / 64.0;
	return 0;
}

/* the shaping filter F(z) over a frame with the given signs, from the
   state f[3] (updated); returns the sum of v^2 */
double V90Enc::shape_metric(const int *mag, int n, int signs, double *f) const
{
	double x1_ = f[0], y1_ = f[1], v1_ = f[2], w = 0;
	int i;
	for (i = 0; i < n; i++) {
		double x = signs >> i & 1 ? mag[i] : -mag[i];
		double y = x - b1 * x1_ + a1 * y1_;
		double v = y - b2 * y1_ + a2 * v1_;
		w += v * v;
		x1_ = x;
		y1_ = y;
		v1_ = v;
	}
	f[0] = x1_;
	f[1] = y1_;
	f[2] = v1_;
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
double V90Enc::best_path(int k, int depth, int Q_, int tprev, const double *f) const
{
	double best = 1e300;
	int r;
	for (r = 0; r < 2; r++) {
		const struct v90_shf *s = &q[k];
		int sg = (s->p ^ tprev) ^ rule_mask(s->fs, Q_, r);
		double st[3] = { f[0], f[1], f[2] };
		double w = shape_metric(s->mag, s->fs, sg, st);
		if (k < depth)
			w += best_path(k + 1, depth, r, sg, st);
		if (w < best)
			best = w;
	}
	return best;
}

/* decide the signs of the oldest shaping frame */
void V90Enc::decide_frame(int depth)
{
	const struct v90_shf *s = &q[0];
	double f[3] = { x1, y1, v1 }, bw = 1e300, bst[3] = { 0, 0, 0 };
	int r, best = 0, bsg = 0;

	for (r = 0; r < 2; r++) {
		int sg = (s->p ^ t_prev) ^ rule_mask(s->fs, Q, r);
		double st[3] = { f[0], f[1], f[2] };
		double w = shape_metric(s->mag, s->fs, sg, st);
		if (depth > 0)
			w += best_path(1, depth, r, sg, st);
		if (w < bw) {
			bw = w;
			best = r;
			bsg = sg;
			memcpy(bst, st, sizeof(st));
		}
	}
	Q = best;
	t_prev = bsg;
	x1 = bst[0];
	y1 = bst[1];
	v1 = bst[2];
	/* into the data frame */
	memcpy(mag_acc + nsh * s->fs, s->mag, s->fs * sizeof(int));
	sign_acc |= bsg << (nsh * s->fs);
	if (++nsh == sr) {
		int i;
		for (i = 0; i < 6; i++)
			outq[outn][i] = sign_acc >> i & 1 ?
				mag_acc[i] : -mag_acc[i];
		outn++;
		nsh = 0;
		sign_acc = 0;
	}
	memmove(q, q + 1, --qn * sizeof(q[0]));
}

void V90Enc::push(const unsigned char *bits, int last_)
{
	unsigned char d[48];
	unsigned long long r = 0;
	int D = S + K, mag[6], i;

	for (i = 0; i < D; i++)
		d[i] = (unsigned char)scramble(&scr, bits[i]);
	for (i = K - 1; i >= 0; i--)
		r = (r << 1) | d[S + i];
	for (i = 0; i < 6; i++) {
		int k = (int)(r % (unsigned)M[i]);
		r /= (unsigned)M[i];
		mag[i] = lin[ucode[i][k]];
	}
	if (sr == 0) {
		/* 5.4.5.1: differential, no shaping */
		int l = last;
		for (i = 0; i < 6; i++) {
			l ^= d[i];
			outq[outn][i] = l ? mag[i] : -mag[i];
		}
		last = l;
		outn++;
		return;
	} else {
		/* shaping frames of 6, 3 or 2 symbols; p' with the odd bit
		   coded differentially (Tables 3 and 4/V.90) */
		int fs = 6 / sr, f, k = 0;
		for (f = 0; f < sr; f++) {
			struct v90_shf *s = &q[qn++];
			s->fs = fs;
			s->p = 0;
			s->last = last_ && f == sr - 1;
			memcpy(s->mag, mag + f * fs, fs * sizeof(int));
			for (i = 1; i < fs; i++) {
				int bit = d[k++];
				if (i & 1) {
					bit ^= last;
					last = bit;
				}
				s->p |= bit << i;
			}
		}
	}
	/* decide what the look-ahead allows */
	while (qn) {
		int depth = ld, j;
		for (j = 0; j < qn && j <= ld; j++)
			if (q[j].last) {
				depth = j;
				break;
			}
		if (depth >= qn)
			break;
		decide_frame(depth);
	}
}

int V90Enc::pop(int out[6])
{
	if (!outn)
		return 0;
	memcpy(out, outq[0], 6 * sizeof(int));
	memmove(outq, outq + 1, --outn * sizeof(outq[0]));
	return 1;
}

} // namespace dsp
