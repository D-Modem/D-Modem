/*
 *
 *    v91.cpp - ITU-T V.91 (05/99): a digital modem on a 4-wire digital
 *              connection, PCM both ways
 *
 *    Both modems run the same start-up after V.8 (8.2.1), here always
 *    asking for the default DIL (Table 5/V.91):
 *
 *    Ez     24 times -Ucode 66
 *    INFO   62 bits, coded differentially into the signs of Ucode 66,
 *           repeated; INFO' (bit 28) once the remote's INFO is in
 *    Eu     12 zeros as INFO: the default DIL follows, its first symbol
 *           is data frame interval 0
 *    J      when the remote asks for a DIL of its own (Table 12/V.90),
 *           then PHIL (scrambled ones) and Em (12 scrambled zeros) as
 *           Eu: we send the DIL it asked for
 *    DIL    as 8.4.1/V.90
 *    SCR    scrambled ones, until the remote's DIL has been analysed
 *    CP     our constellations (Table 4/V.91, the layout of CP in
 *           V.90), CP' once the remote's is in, then Es (12 scrambled
 *           zeros) and B1 (two data frames of ones)
 *    data   the V.90 encoder with Sr = 0 and S = 6: K bits through the
 *           modulus encoder, 6 sign bits coded differentially
 *
 *    The receiver learns from the remote's DIL which codes come through,
 *    and how (robbed bits, a pad or another G.711 law on the way show up
 *    there): those that arrive distinguishable, both signs, are the
 *    constellation it asks for in CP, and the received values decode
 *    data by their nearest DIL value.  Transparent mode is never asked
 *    for nor granted: through a 16 bit linear interface (pjsua's) the
 *    mu-law codes +0 and -0 are one.
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "v91.h"

namespace dsp {

#define INFO_UCODE      66      /* INFO, J, PHIL, SCR, CP: its sign */
#define MS(ms)          ((long)(ms) * 8)        /* symbols */

/* scrambles a bit; returns the scrambled bit */
static int scramble(unsigned *reg, int bit)
{
	/* GPC: 1 + x^-18 + x^-23 */
	int out = (bit ^ (*reg >> 17) ^ (*reg >> 22)) & 1;
	*reg = (*reg << 1) | out;
	return out;
}

/* the descrambler of scramble(): the register keeps the received bits;
   returns the descrambled bit */
static int descramble(unsigned *reg, int bit)
{
	int out = (bit ^ (*reg >> 17) ^ (*reg >> 22)) & 1;
	*reg = (*reg << 1) | (bit & 1);
	return out;
}

/* appends the low nbits of val to b at *n (one bit per byte), least
   significant bit first */
static void pbits(unsigned char *b, int *n, unsigned val, int nbits)
{
	int i;
	for (i = 0; i < nbits; i++)
		b[(*n)++] = (val >> i) & 1;
}

/* Table 5/V.91 */
static void default_dil(V90Dil *d)
{
	int i;
	memset(d, 0, sizeof(*d));
	d->n = 125;
	d->lsp = d->ltp = 12;
	for (i = 0; i < 12; i++) {
		d->sp[i] = (0x0FC0 >> i) & 1;
		d->tp[i] = (0x0FFF >> i) & 1;
	}
	for (i = 0; i < 8; i++) {
		d->h[i] = 1;
		d->ref[i] = 0;
	}
	/* 124 0 123 1 122 2 ... 62 */
	for (i = 0; i < 125; i++)
		d->ucode[i] = (unsigned char)(i & 1 ? i / 2 : 124 - i / 2);
}

/*
 *    the data mode encoder and decoder
 */

/* the encoder for the constellations of cp (the remote's CP) and D bits
   per data frame, lin_: Ucode -> linear; returns -1 when they cannot
   carry K = D - 6 bits */
int V91Enc::init(const V90Cp *cp, int D, const int *lin_)
{
	double prod = 1;
	int i, u;

	memset(this, 0, sizeof(*this));
	K = D - 6;
	lin = lin_;
	for (i = 0; i < 6; i++) {
		const unsigned char *m = cp->mask[cp->con[i]];
		for (u = 127; u >= 0; u--)
			if (m[u])
				ucode[i][M[i]++] = (unsigned char)u;
		if (!M[i])
			return -1;
		prod *= M[i];
	}
	if (K < 1 || K > 42 || prod < (double)(1ULL << K))
		return -1;
	return 0;
}

/* bits: the 6 sign bits, then the K bits (only the K bits with cc);
   all scrambled, the cc bits not */
void V91Enc::frame(const unsigned char *bits, const unsigned char *cc,
		   int out_[6])
{
	unsigned char d[48];
	unsigned long long r = 0;
	int S = cc ? 0 : 6, i;

	for (i = 0; i < S + K; i++)
		d[i] = (unsigned char)scramble(&scr, bits[i]);
	for (i = K - 1; i >= 0; i--)
		r = (r << 1) | d[S + i];
	for (i = 0; i < 6; i++) {
		int k = (int)(r % (unsigned)M[i]), mag;
		r /= (unsigned)M[i];
		mag = lin[ucode[i][k]];
		/* 5.4.5.1/V.90 with Sr = 0 */
		last ^= cc ? cc[i] : d[i];
		out_[i] = last ? mag : -mag;
	}
}

/* the decoder for the constellations of cp (our CP) and D bits per data
   frame; returns -1 when a constellation is empty */
int V91Dec::init(const V90Cp *cp, int D)
{
	int i, u;
	memset(this, 0, sizeof(*this));
	K = D - 6;
	for (i = 0; i < 6; i++) {
		const unsigned char *m = cp->mask[cp->con[i]];
		for (u = 127; u >= 0; u--)
			label[i][u] = m[u] ? (short)M[i]++ : -1;
		if (!M[i])
			return -1;
	}
	return 0;
}

/* u, s: the Ucodes and signs (1: positive) of the 6 symbols; frames
   that cannot be one are counted in errors */
int V91Dec::frame(const int *u, const int *s, unsigned char *bits,
		  unsigned char *cc)
{
	unsigned long long r = 0;
	unsigned char d[48];
	int S = cc ? 0 : 6, bad = 0, i;

	for (i = 5; i >= 0; i--) {
		int l = label[i][u[i] & 127];
		if (l < 0) {
			bad = 1;
			l = 0;
		}
		r = r * (unsigned)M[i] + (unsigned)l;
	}
	if (r >> K)
		bad = 1;
	for (i = 0; i < 6; i++) {
		int b = s[i] ^ last;
		last = s[i];
		if (cc)
			cc[i] = (unsigned char)b;
		else
			d[i] = (unsigned char)b;
	}
	for (i = 0; i < K; i++)
		d[S + i] = (r >> i) & 1;
	for (i = 0; i < S + K; i++)
		bits[i] = (unsigned char)descramble(&scr, d[i]);
	if (bad)
		errors++;
	return bad ? -1 : S + K;
}

/* i: the data frame interval, x: the received value; by binary search
   in val[i] (no codes: the distance 1 << 20) */
int V91Map::decode(int i, int x, int *u, int *s) const
{
	int lo = 0, hi = n[i] - 1, b, d;

	if (n[i] == 0) {
		*u = *s = 0;
		return 1 << 20;
	}
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (val[i][mid] < x)
			lo = mid + 1;
		else
			hi = mid;
	}
	b = lo;
	if (lo > 0 && abs(x - val[i][lo - 1]) < abs(val[i][lo] - x))
		b = lo - 1;
	d = abs(x - val[i][b]);
	*u = ucode[i][b];
	*s = sign[i][b];
	return d;
}

/*
 *    the modem
 */

/* the modem at the start of V.91 (after V.8): A-law when alaw_, at most
   max_k_ bits through the modulus encoder (up to 41), want_cc_: we ask
   for the control channel.  Tests: DSP_V91_RETRAIN=<ms>[a|c] (our own
   retrain that long after connecting), DSP_V91_MUTE=a|c */
void V91::init(int alaw_, int caller_, int max_k_, int want_cc_)
{
	static const char slip[] = "0111011111111010";
	unsigned crc;
	int i, n_ = 0;

	memset(this, 0, sizeof(*this));
	alaw = alaw_;
	for (i = 0; i < 128; i++)
		lin[i] = v90_ucode_linear(alaw, i);
	caller = caller_;
	max_k = max_k_ > 0 && max_k_ <= 41 ? max_k_ : 41;
	want_cc = want_cc_;
	default_dil(&rdil);
	/* the control channel sequence (Table 3/V.91): no cleardown or
	   retrain asked for, no secondary channel; the CRC over bits 0 to
	   77 */
	for (i = 0; i < 16; i++)
		ccseq[n_++] = (unsigned char)(slip[i] - '0');
	pbits(ccseq, &n_, 0, 8);
	pbits(ccseq, &n_, 0, 27);
	pbits(ccseq, &n_, 0, 27);
	crc = v34_crc_bits(ccseq, 78, 0xFFFF);
	for (i = 15; i >= 0; i--)
		ccseq[n_++] = (crc >> i) & 1;
	pbits(ccseq, &n_, 0, 2);
	if (getenv("DSP_V91_RETRAIN")) {
		const char *e = getenv("DSP_V91_RETRAIN");
		const char *side = e + strspn(e, "0123456789");
		if (!*side || *side == (caller_ ? 'c' : 'a'))
			retrain_at = MS(atoi(e));
	}
	/* test: DSP_V91_MUTE=a or c: that modem is silent (both go to
	   V.34) */
	if (getenv("DSP_V91_MUTE"))
		mute = *getenv("DSP_V91_MUTE") == (caller_ ? 'c' : 'a');
	rx = V91R_INFO;
	tx_segment(V91T_EZ);
	DSP_DBG("v91: start, %s, %s\n", caller ? "caller" : "answerer",
		alaw ? "A-law" : "mu-law");
}

/* INFO (Table 6/V.91): the default DIL, our law */
void V91::make_info(int ack)
{
	unsigned crc;
	int n_ = 0, i;

	pbits(tb, &n_, 0xF, 4);
	pbits(tb, &n_, 0x4E, 8);     /* 01110010 sent from the left */
	pbits(tb, &n_, 0, 14);
	pbits(tb, &n_, 0, 1);        /* 26: the default DIL */
	pbits(tb, &n_, want_cc, 1);  /* 27: the control channel */
	pbits(tb, &n_, ack, 1);      /* 28 */
	pbits(tb, &n_, 0, 4);
	pbits(tb, &n_, 0, 5);        /* 33:37 no maximum power */
	pbits(tb, &n_, 0, 1);        /* 38 */
	pbits(tb, &n_, alaw, 1);     /* 39 */
	pbits(tb, &n_, 0, 1);        /* 40: no transparent mode */
	pbits(tb, &n_, 0, 1);        /* 41 */
	crc = v34_crc_bits(tb + 12, n_ - 12, 0xFFFF);
	for (i = 15; i >= 0; i--)
		tb[n_++] = (crc >> i) & 1;
	pbits(tb, &n_, 0xF, 4);
	tbn = n_;
	tbpos = 0;
}

/* CP (Table 4/V.91) from cp_tx, filled to whole data frames */
void V91::make_cp(int ack)
{
	unsigned crc;
	int n_ = 0, i, j, c;

	memset(tb, 0, sizeof(tb));
	pbits(tb, &n_, 0x1FFFF, 17);
	pbits(tb, &n_, 0, 1);
	pbits(tb, &n_, 0, 1);        /* 18: no transparent mode */
	pbits(tb, &n_, 1, 1);        /* 19 */
	pbits(tb, &n_, our_drn, 5);
	pbits(tb, &n_, 0, 8);
	pbits(tb, &n_, ack, 1);      /* 33 */
	n_ = 103;                       /* 34:102 zeros and start bits */
	for (i = 0; i < 4; i++)
		pbits(tb, &n_, cp_tx.con[i], 4);
	pbits(tb, &n_, 0, 1);
	pbits(tb, &n_, cp_tx.con[4], 4);
	pbits(tb, &n_, cp_tx.con[5], 4);
	pbits(tb, &n_, 0, 8);        /* 128:135 */
	for (j = 0; j < cp_tx.ncon; j++)
		for (c = 0; c < 8; c++) {
			pbits(tb, &n_, 0, 1);
			for (i = 0; i < 16; i++)
				tb[n_++] = cp_tx.mask[j][16 * c + i];
		}
	pbits(tb, &n_, 0, 1);
	crc = 0xFFFF;
	for (i = 18; i < n_ - 1; i += 17)
		crc = v34_crc_bits(tb + i, 16, crc);
	for (i = 15; i >= 0; i--)
		tb[n_++] = (crc >> i) & 1;
	pbits(tb, &n_, 0, 1);
	while (n_ % 6)
		tb[n_++] = 0;
	tbn = n_;
	tbpos = 0;
}

/* the next transmitter segment, from its first symbol */
void V91::tx_segment(enum v91_tx s)
{
	tx = s;
	tn = 0;
	switch (s) {
	case V91T_INFO:
		t = 0;
		make_info(0);
		info_ack_sent = 0;
		phase_start = n;
		break;
	case V91T_PHIL:
	case V91T_SCR:
		tscr = 0;
		t = 0;
		break;
	case V91T_DIL:
		dil_seg = dil_k = 0;
		if (!dil.n)
			tx_segment(V91T_SCR);
		break;
	case V91T_CP:
		make_cp(0);
		cp_ack_sent = cp_sent = 0;
		break;
	case V91T_B1:
		opos = 6;
		break;
	case V91T_DATA:
		tframes = 0;
		break;
	default:
		break;
	}
	DSP_DBG("v91: tx segment %d at %ld\n", s, n);
}

/* a bit as the sign of Ucode 66, coded differentially */
int V91::sign_sample(int bit)
{
	t ^= bit & 1;
	return t ? lin[INFO_UCODE] : -lin[INFO_UCODE];
}

/* a bit scrambled (GPC), then as sign_sample() */
int V91::scr_sample(int bit)
{
	return sign_sample(scramble(&tscr, bit));
}

/* the next sample of the DIL the remote asked for (8.4.1/V.90), then
   SCR */
int V91::dil_sample()
{
	int u = dil.ucode[dil_seg], c = u >> 4, len = (dil.h[c] + 1) * 6;
	int k = dil_k, x;

	x = dil.tp[k % dil.ltp] ? lin[u] : lin[dil.ref[c]];
	if (!dil.sp[k % dil.lsp])
		x = -x;
	if (++dil_k >= len) {
		dil_k = 0;
		if (++dil_seg >= dil.n)
			tx_segment(V91T_SCR);
	}
	return x;
}

/* the next sample we send (linear), going on from segment to segment
   as the receiver's progress allows */
int V91::tx_sample()
{
	int x = 0;

	switch (tx) {
	case V91T_EZ:
		x = -lin[INFO_UCODE];
		if (++tn == 24)
			tx_segment(V91T_INFO);
		return x;
	case V91T_INFO:
		x = sign_sample(tb[tbpos]);
		if (++tbpos == tbn) {
			if (info_ack_sent) {
				/* INFO' is out: the DIL the remote asked for */
				if (peer_default) {
					default_dil(&dil);
					tx_segment(V91T_EU);
				} else if (have_j)
					tx_segment(V91T_EM);
				else
					tx_segment(V91T_PHIL);
			} else if (got_info) {
				make_info(1);
				info_ack_sent = 1;
			} else
				tbpos = 0;
		}
		return x;
	case V91T_EU:
		x = sign_sample(0);
		if (++tn == 12)
			tx_segment(V91T_DIL);
		return x;
	case V91T_PHIL:
		x = scr_sample(1);
		if (have_j)
			tx_segment(V91T_EM);
		return x;
	case V91T_EM:
		x = scr_sample(0);
		if (++tn == 12)
			tx_segment(V91T_DIL);
		return x;
	case V91T_DIL:
		return dil_sample();
	case V91T_SCR:
		x = scr_sample(1);
		if (++tn % 6 == 0 && cp_ready)
			tx_segment(V91T_CP);
		return x;
	case V91T_CP:
		x = scr_sample(tb[tbpos]);
		if (++tbpos == tbn) {
			tbpos = 0;
			cp_sent++;
			if (cp_ack_sent && (got_cp_ack || got_es))
				tx_segment(V91T_ES);
			else if (got_cp && !cp_ack_sent) {
				make_cp(1);
				cp_ack_sent = 1;
			}
		}
		return x;
	case V91T_ES:
		x = scr_sample(0);
		if (++tn == 12) {
			if (enc.init(&cp_rx, cp_rx.drn + 20, lin))
				DSP_DBG("v91: the remote's CP does not work\n");
			tx_segment(V91T_B1);
		}
		return x;
	case V91T_B1:
	case V91T_DATA:
		if (opos >= 6) {
			unsigned char b[48], c6[6];
			int cc = 0, i;
			if (tx == V91T_B1 && tn == 2)
				tx_segment(V91T_DATA);
			if (tx == V91T_B1) {
				memset(b, 1, sizeof(b));
				tn++;
			} else {
				int nb;
				cc = peer_cc && tframes % 64 == 0;
				nb = enc.K + (cc ? 0 : 6);
				memset(b, 1, nb);
				get_bits(ctx, b, nb);
				if (cc)
					for (i = 0; i < 6; i++)
						c6[i] = ccseq[(tframes / 64 * 6 + i) % 96];
				tframes++;
			}
			enc.frame(b, cc ? c6 : NULL, out);
			opos = 0;
		}
		return out[opos++];
	}
	return 0;
}

/*
 *    the receiver
 */

/* the receiver back to waiting for INFO, what it learnt of the remote's
   start-up forgotten */
void V91::rx_restart()
{
	rx = V91R_INFO;
	got_info = got_info_ack = 0;
	have_j = 0;
	jn = 0;
	got_cp = got_cp_ack = got_es = 0;
	cp_ready = 0;
	cpn = cplen = 0;
	since_info = zeros = 0;
	rd_seg = rd_k = 0;
	memset(seen, 0, sizeof(seen));
}

/* a bit (decoded differentially) into the INFO window; 1 at the end of a
   good INFO frame */
int V91::info_bit(int bit)
{
	static const unsigned char syn[8] = { 0, 1, 1, 1, 0, 0, 1, 0 };
	unsigned crc;
	int i;

	memmove(ib, ib + 1, V91_INFO_BITS - 1);
	ib[V91_INFO_BITS - 1] = (unsigned char)bit;
	since_info++;
	zeros = bit ? 0 : zeros + 1;
	for (i = 0; i < 4; i++)
		if (!ib[i] || !ib[58 + i])
			return 0;
	if (memcmp(ib + 4, syn, 8))
		return 0;
	crc = v34_crc_bits(ib + 12, 30, 0xFFFF);
	for (i = 0; i < 16; i++)
		if (ib[42 + i] != ((crc >> (15 - i)) & 1))
			return 0;
	since_info = 0;
	zeros = 0;
	return 1;
}

/* a received symbol of the DIL we asked for: the value each code and
   sign arrives as in each data frame interval (seen 2: not the same each
   time) */
void V91::dil_record(int x)
{
	int u = rdil.ucode[rd_seg], c = u >> 4, len = (rdil.h[c] + 1) * 6;
	int k = rd_k, i = (int)(rk % 6), code, sg;

	code = rdil.tp[k % rdil.ltp] ? u : rdil.ref[c];
	sg = rdil.sp[k % rdil.lsp];
	if (!seen[i][code][sg]) {
		seen[i][code][sg] = 1;
		obs[i][code][sg] = (short)x;
	} else if (obs[i][code][sg] != x)
		seen[i][code][sg] = 2;  /* not the same each time */
	if (++rd_k >= len) {
		rd_k = 0;
		rd_seg++;
	}
}

/* from the remote's DIL: the codes that come through, our CP, the
   decoder; -1 if too few do */
int V91::choose()
{
	double bits = 0, prod = 1;
	int i, j, u, s, K;

	memset(&map, 0, sizeof(map));
	memset(&cp_tx, 0, sizeof(cp_tx));
	for (i = 0; i < 6; i++) {
		/* both signs seen, the same each time and different, and
		   neither value taken by another code: where codes arrive as
		   one (robbed bits, a pad), one of them */
		for (u = 127; u >= 0; u--) {
			int v, s2, clash = 0;
			map.ok[i][u] = 0;
			if (seen[i][u][0] != 1 || seen[i][u][1] != 1 ||
			    obs[i][u][0] == obs[i][u][1])
				continue;
			for (v = u + 1; v < 128 && !clash; v++)
				for (s = 0; s < 2; s++)
					for (s2 = 0; s2 < 2; s2++)
						if (map.ok[i][v] &&
						    obs[i][v][s2] == obs[i][u][s])
							clash = 1;
			map.ok[i][u] = !clash;
		}
		/* the decoding table, sorted by the received value */
		for (u = 0; u < 128; u++)
			for (s = 0; s < 2 && map.ok[i][u]; s++) {
				int k = map.n[i]++;
				while (k > 0 && map.val[i][k - 1] > obs[i][u][s]) {
					map.val[i][k] = map.val[i][k - 1];
					map.ucode[i][k] = map.ucode[i][k - 1];
					map.sign[i][k] = map.sign[i][k - 1];
					k--;
				}
				map.val[i][k] = obs[i][u][s];
				map.ucode[i][k] = (unsigned char)u;
				map.sign[i][k] = (unsigned char)s;
			}
		if (!map.n[i])
			return -1;
		bits += log2((double)(map.n[i] / 2));
		/* one constellation per distinct set */
		for (j = 0; j < cp_tx.ncon; j++)
			if (!memcmp(cp_tx.mask[j], map.ok[i], 128))
				break;
		if (j == cp_tx.ncon)
			memcpy(cp_tx.mask[cp_tx.ncon++], map.ok[i], 128);
		cp_tx.con[i] = j;
		prod *= map.n[i] / 2;
	}
	K = (int)bits;
	while (K > 0 && prod < (double)(1ULL << K))
		K--;
	DSP_DBG("v91: DIL: %d %d %d %d %d %d codes, %d constellations, K %d\n",
		map.n[0] / 2, map.n[1] / 2, map.n[2] / 2, map.n[3] / 2,
		map.n[4] / 2, map.n[5] / 2, cp_tx.ncon, K);
	if (K > max_k)
		K = max_k;
	if (K < 15)
		return -1;
	our_drn = K - 14;
	cp_tx.drn = our_drn;
	if (dec.init(&cp_tx, our_drn + 20))
		return -1;
	cp_ready = 1;
	return 0;
}

/* a descrambled bit in CP; 1 when a CP frame is complete */
int V91::cp_bit(int bit)
{
	sync = (sync << 1) | (unsigned)bit;
	if (cpn == 0) {
		/* 17 ones, then the start bit */
		if ((sync & 0x3FFFF) == 0x3FFFE) {
			memset(cpb, 1, 17);
			cpb[17] = 0;
			cpn = 18;
		}
		return 0;
	}
	cpb[cpn++] = (unsigned char)bit;
	if (cpn == 129) {
		cplen = v90_cp_length(cpb, cpn);
		if (cplen <= 129 || cplen + 8 > V91_CP_MAX) {
			cpn = 0;
			return 0;
		}
		cpframe = (cplen + 1 + 5) / 6 * 6;
	}
	if (cpn > 129 && cpn == cplen) {
		V90Cp c;
		if (c.parse(cpb, cplen)) {
			DSP_DBG("v91: CP with a bad CRC\n");
			cpn = 0;
			return 0;
		}
		if (!got_cp)
			DSP_DBG("v91: CP: drn %d, %d constellations, ack %d\n",
				c.drn, c.ncon, c.ack);
		cp_rx = c;
		got_cp = 1;
		if (c.ack)
			got_cp_ack = 1;
	}
	if (cpn > 129 && cpn == cpframe) {
		cpn = 0;
		return 1;
	}
	return 0;
}

/* a symbol of B1 or data */
void V91::data_symbol(int x)
{
	int i = (int)(rk % 6);

	if (map.decode(i, x, &ru[rpos], &rsg[rpos]))
		err_count++;
	if (++rpos < 6)
		return;
	rpos = 0;
	if (rx == V91R_B1) {
		unsigned char b[48];
		dec.frame(ru, rsg, b, NULL);
		if (++bframes == 2) {
			rx = V91R_DATA;
			rframes = 0;
			DSP_DBG("v91: rx data at %ld\n", n);
		}
	} else {
		unsigned char b[48], c6[6];
		int cc = want_cc && rframes % 64 == 0, nb;
		if (dec.frame(ru, rsg, b, cc ? c6 : NULL) < 0)
			err_count++;
		nb = dec.K + (cc ? 0 : 6);
		rframes++;
		put_bits(ctx, b, nb);
	}
}

/* a received sample through the receiver's stages; returns V91_ESCAPE
   or V91_FAIL to give up, else 0 */
int V91::rx_sample(int x)
{
	int s = x > 0, bit = s ^ rs, b;

	rs = s;
	switch (rx) {
	case V91R_INFO:
		if (info_bit(bit)) {
			int ack = ib[28];
			if (!got_info)
				DSP_DBG("v91: INFO: default DIL %d, control channel"
					" %d, %s, transparent %d\n", !ib[26],
					ib[27], ib[39] ? "A-law" : "mu-law",
					ib[40]);
			got_info = 1;
			peer_default = !ib[26];
			peer_cc = ib[27];
			peer_alaw = ib[39];
			peer_transparent = ib[40] | ib[41] << 1;
			if (ack && !got_info_ack) {
				got_info_ack = 1;
				DSP_DBG("v91: INFO' at %ld\n", n);
				if (!peer_default) {
					/* J follows, scrambled from zero */
					rx = V91R_J;
					rscr = 0;
					sync = 0;
					jn = 0;
				}
			}
			if (peer_transparent & 2) {
				DSP_DBG("v91: the remote wants transparent"
					" mode or nothing\n");
				return V91_FAIL;
			}
		} else if (got_info && peer_default && since_info == 12 &&
			   zeros >= 12) {
			DSP_DBG("v91: Eu at %ld\n", n);
			rx = V91R_DIL;
			rk = 0;
			return 0;
		}
		break;
	case V91R_J:
		b = descramble(&rscr, bit);
		sync = (sync << 1) | (unsigned)b;
		if (!jn) {
			if ((sync & 0x3FFFF) == 0x3FFFE) {
				memset(jb, 1, 17);
				jb[17] = 0;
				jn = 18;
			}
			break;
		}
		if (jn < (int)sizeof(jb))
			jb[jn++] = (unsigned char)b;
		if (jn >= 50) {
			int len = v90_dil_length(jb, jn);
			if (len <= 0 || len > (int)sizeof(jb)) {
				DSP_DBG("v91: J does not make sense\n");
				return V91_ESCAPE;
			}
			if (jn == len) {
				V90Dil d;
				if (d.parse(jb, len)) {
					DSP_DBG("v91: J with a bad CRC\n");
					return V91_ESCAPE;
				}
				dil = d;
				have_j = 1;
				DSP_DBG("v91: J: a DIL of %d segments\n", d.n);
				rx = V91R_EM;
				zeros = 0;
			}
		}
		break;
	case V91R_EM:
		/* PHIL (ones), then Em */
		b = descramble(&rscr, bit);
		zeros = b ? 0 : zeros + 1;
		if (zeros == 12) {
			DSP_DBG("v91: Em at %ld\n", n);
			rx = V91R_DIL;
			rk = 0;
			return 0;
		}
		break;
	case V91R_DIL:
		dil_record(x);
		if (rd_seg >= rdil.n) {
			if (choose()) {
				DSP_DBG("v91: the DIL leaves too little\n");
				return V91_ESCAPE;
			}
			rx = V91R_CP;
			cpn = 0;
			sync = 0;
		}
		break;
	case V91R_CP:
		b = descramble(&rscr, bit);
		if (cp_bit(b)) {
			rx = V91R_ES;
			esn = 0;
		}
		break;
	case V91R_ES:
		/* after a CP frame: Es, or the next CP */
		b = descramble(&rscr, bit);
		sync = (sync << 1) | (unsigned)b;
		if (b)
			rx = V91R_CP;
		else if (++esn == 12) {
			got_es = 1;
			DSP_DBG("v91: Es at %ld (data frame interval %ld)\n",
				n, (rk + 1) % 6);
			rx = V91R_B1;
			rpos = bframes = 0;
		}
		break;
	case V91R_B1:
	case V91R_DATA:
		data_symbol(x);
		/* the remote's retrain: its INFO */
		if (info_bit(bit)) {
			DSP_DBG("v91: the remote retrains\n");
			rx_restart();
			got_info = 1;
			peer_default = !ib[26];
			peer_cc = ib[27];
			retraining = 1;
			tx_segment(V91T_INFO);
		}
		break;
	}
	if (rx >= V91R_DIL)
		rk++;
	return 0;
}

/* a retrain from our side: INFO again, the receiver restarted */
void V91::retrain(const char *why)
{
	DSP_DBG("v91: retrain: %s\n", why);
	retraining = 1;
	rx_restart();
	tx_segment(V91T_INFO);
}

/* a symbol both ways (x received, *y to send); returns V91_CONNECT the
   first time data mode is up both ways, V91_ESCAPE or V91_FAIL to give up
   (also on the timeouts of 8.3), else V91_NONE; retrains on too many
   decoding errors */
int V91::sample(int x, int *y)
{
	int r;

	*y = tx_sample();
	if (mute)
		*y = 0;
	r = rx_sample(x);
	n++;
	if (r)
		return r;
	if (tx == V91T_DATA && rx == V91R_DATA) {
		if (retraining) {
			retraining = 0;
			DSP_DBG("v91: retrained\n");
		}
		if (!connected) {
			connected = 1;
			err_window = n;
			if (retrain_at)
				retrain_at += n;
			return V91_CONNECT;
		}
		/* poor decoding: retrain */
		if (n - err_window >= MS(1000)) {
			if (err_count > MS(1000) / 20) {
				DSP_DBG("v91: %ld errors, %ld frames\n",
					err_count, dec.errors);
				retrain("decoding errors");
			}
			err_window = n;
			err_count = 0;
		}
		if (retrain_at && n >= retrain_at) {
			retrain_at = 0;
			retrain("test");
		}
		return 0;
	}
	/* 8.3: no INFO' in 1.5 s, no B1 in 10 s */
	if (!retraining && !connected &&
	    ((!got_info_ack && n - phase_start > MS(1500)) ||
	     n - phase_start > MS(10000))) {
		DSP_DBG("v91: start-up timeout (INFO' %d)\n", got_info_ack);
		return V91_ESCAPE;
	}
	if ((retraining || connected) && n - phase_start > MS(10000)) {
		DSP_DBG("v91: retrain timeout\n");
		return V91_FAIL;
	}
	return 0;
}

} // namespace dsp
