/*
 *
 *    dp_v32.c - ITU V.32 / V.32bis data pump (4800 to 14400 bit/s)
 *
 *    2400 baud QAM on an 1800 Hz carrier in both directions, separated
 *    by an echo canceller.  4800 bit/s is differentially coded QPSK,
 *    7200 to 14400 bit/s use the 8 state trellis code.
 *
 *    Start up sequence (as sent by the original library):
 *
 *    answerer: ANS, AC, CA (on AA), AC (on CC), silence,
 *              S S-bar TRN R1 ... (until the caller's S), silence,
 *              S S-bar TRN R3 ... (after R2, until E), E, data
 *    caller:   AA (after ANS), CC (64 symbols after AC -> CA),
 *              silence (on CA -> AC),
 *              S S-bar TRN R2 ... (after R1, until R3), E, data
 *
 *    TRN is 256 symbols of A/C (one scrambled bit per symbol) followed
 *    by A/B/C/D (two bits per symbol).  The rate words R1, R2, R3 and
 *    E are sent scrambled with the 4800 bit/s coding, most significant
 *    bit first.  The caller scrambles with 1 + x^-18 + x^-23, the
 *    answerer with 1 + x^-5 + x^-23.
 *
 *    When the remote modem does not do V.32 the pump falls back to
 *    V.22bis (answerer: no AA after the answer tone, caller: V.22
 *    unscrambled ones instead of AC).
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qam.h"

#define V32_BAUD        2400
#define V32_ALPHA       0.25f
#define V32_SPAN        6
#define V32_EQ_LEN      32
#define V32_POWER       40.0f   /* mean power of all the constellations */
#define V32_TRN_LEN     2048    /* 4 point TRN symbols we send */
#define V32_EC_LEN      128
/* the original library sends all V.32 signals at about 2000 rms; its
   fixed point receiver does not cope with much more */
#define V32_TX_AMPLITUDE (2000.0f * 1.41421f)

/* rate word bits, B0 is the most significant bit */
#define RB_HEADER       0xF000  /* 0000: R1/R2/R3, 1111: E */
#define RB_SYNC         0x0111
#define RB_B4           0x0800
#define RB_4800         0x0400
#define RB_9600         0x0200
#define RB_TCM          0x0080
#define RB_7200         0x0040
#define RB_12000        0x0020
#define RB_14400        0x0008
#define RB_RATES        (RB_4800 | RB_9600 | RB_TCM | RB_7200 | RB_12000 | RB_14400)

struct v32_rate {
	int bps;
	unsigned bits;
	int nbits;
	const cplx *points;
};

static const struct v32_rate v32_rates[] = {
	{ 14400, RB_TCM | RB_14400, 6, v32_points_14400 },
	{ 12000, RB_TCM | RB_12000, 5, v32_points_12000 },
	{ 9600, RB_TCM | RB_9600, 4, v32_points_9600 },
	{ 7200, RB_TCM | RB_7200, 3, v32_points_7200 },
	{ 4800, RB_4800, 2, v32_points_4800 },
};
#define V32_NRATES (int)(sizeof(v32_rates) / sizeof(v32_rates[0]))

/* 4800 bit/s differential code: next point index from previous index
   and dibit (first bit in bit 0); points indexed as v32_points_4800 */
static const unsigned char v32_4800_enc[4][4] = {
	{ 2, 3, 0, 1 }, { 0, 2, 1, 3 }, { 3, 1, 2, 0 }, { 1, 0, 3, 2 },
};
/* A B C D -> index in v32_points_4800 */
static const int abcd_to_4800[4] = { 0, 2, 3, 1 };
/* TRN dibit (first bit << 1 | second bit) -> A B C D */
static const int trn_dibit_abcd[4] = { 0, 1, 3, 2 };

enum v32_txm {
	TXM_SILENCE,
	TXM_ANS,
	TXM_AA,
	TXM_CC,
	TXM_AC,
	TXM_S,
	TXM_SBAR,
	TXM_TRN2,
	TXM_TRN4,
	TXM_R,
	TXM_E,
	TXM_DATA,
};

enum v32_state {
	/* answerer */
	A_SILENCE,
	A_ANS,
	A_GAP,
	A_AC,
	A_CA,
	A_AC2,
	A_QUIET,
	A_TRAIN1,               /* S S-bar TRN */
	A_R1,
	A_WAIT_R2,
	A_TRAIN2,
	A_R3,
	A_E,
	/* caller */
	C_AA,
	C_AA_DELAY,
	C_CC,
	C_WAIT_R1,
	C_TRAIN,
	C_R2,
	C_E,
	/* both */
	V32_DATA,
};

/* receiver phases */
enum v32_rxp {
	RX_IDLE,                /* waiting for S */
	RX_S,
	RX_TRN,                 /* S-bar seen: training, looking for R */
	RX_R,                   /* R seen, looking for E (or the next R) */
	RX_DATA,
};

/* narrow band tone tracker for AA / AC and their phase reversals */
struct tone_trk {
	struct osc lo;
	cplx lp1, lp2;
	float a;
	cplx hist[128];
	int pos, lag;
	int on;
	long on_count;
	int hold;
};

struct v32_dp {
	struct dp dp;
	struct modem *modem;
	struct dp_runtime *rt;
	int srate;
	int caller;
	int automode;
	enum v32_state state;
	long count, total;      /* samples */
	long timeout, carrier_loss;
	unsigned caps;          /* rate bits we offer */
	unsigned r2_caps;       /* caller: rates in R2 */

	/* transmitter */
	struct qam_tx tx;
	enum v32_txm txm;
	long tx_syms;           /* symbols sent in the current tx state */
	int tx_ac_phase;
	int tx_reverse;         /* repeat one AC symbol: phase reversal */
	unsigned tx_scr;
	int tx_scr_tap;
	int tx_4800;            /* 4800 coding state (v32_points_4800 index) */
	int tx_last_abcd;
	unsigned tx_word;
	int tx_word_bit;
	struct trellis_enc tx_trellis;
	const struct v32_rate *tx_rate;
	struct osc ans;
	float last_tx;

	/* receiver */
	struct echo_canceller ec;
	float ec_mu;
	struct qam_rx rx;
	struct tone_trk aa, ac, usb1, ans2100;
	int carrier;
	long carrier_run;
	enum v32_rxp rxp;
	int run_s, last_sgn, s_miss;
	cplx last_z;
	unsigned rx_scr;
	int rx_scr_tap;
	int rx_4800;
	unsigned rx_bits;       /* last 32 descrambled bits */
	int rx_nbits;
	unsigned rx_word, rx_r_word;
	int rx_word_align;
	float rx_mse;
	int rx_e_seen;
	const struct v32_rate *rx_rate;
	struct viterbi vit;
	int rx_dbits;           /* data bits per symbol */

	/* events from the receiver to the handshake */
	int ev_aa, ev_ac, ev_rev, ev_s, ev_r, ev_e, ev_usb1;
	int retraining;         /* a retrain in data mode (no new CONNECT) */
	int retrains;
	long retrain_at;        /* test: our own retrain this far (samples)
				   into the data, $DSP_V32_RETRAIN ms */
	unsigned r_word;        /* last R word received */
};


/*
 *    helpers
 */

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

static int nearest_abcd(cplx z)
{
	int k, best = 0;
	float bd = 1e30f;
	for (k = 0; k < 4; k++) {
		float d = cplx_pow(cplx_sub(z, v32_abcd[k]));
		if (d < bd) {
			bd = d;
			best = k;
		}
	}
	return best;
}

static int nearest_point(const cplx *pts, int n, cplx z)
{
	int k, best = 0;
	float bd = 1e30f;
	for (k = 0; k < n; k++) {
		float d = cplx_pow(cplx_sub(z, pts[k]));
		if (d < bd) {
			bd = d;
			best = k;
		}
	}
	return best;
}

static const struct v32_rate *rate_of_word(unsigned w)
{
	int i;
	for (i = 0; i < V32_NRATES; i++)
		if ((w & v32_rates[i].bits) == v32_rates[i].bits)
			return &v32_rates[i];
	return NULL;
}

static unsigned final_word(const struct v32_rate *r)
{
	return RB_B4 | RB_SYNC | r->bits;
}

static void tone_init(struct tone_trk *t, float freq, int srate)
{
	memset(t, 0, sizeof(*t));
	osc_set_freq(&t->lo, -freq, srate);
	t->a = 1.0f / (0.0015f * srate);
	t->lag = srate / 100;   /* compare with 10 ms ago */
	if (t->lag > 127)
		t->lag = 127;
}

/* returns 1 on a phase reversal; the tone has to carry a fair part of
   the total input power (in_power), noise alone does not turn it on */
static int tone_run(struct tone_trk *t, float x, float in_power, int srate)
{
	cplx v = cplx_scale(osc_next_cplx(&t->lo), x), old;
	float p, po;
	int rev = 0;

	t->lp1.re += t->a * (v.re - t->lp1.re);
	t->lp1.im += t->a * (v.im - t->lp1.im);
	t->lp2.re += t->a * (t->lp1.re - t->lp2.re);
	t->lp2.im += t->a * (t->lp1.im - t->lp2.im);
	old = t->hist[(t->pos - t->lag) & 127];
	t->hist[t->pos++ & 127] = t->lp2;
	p = cplx_pow(t->lp2);
	po = cplx_pow(old);
	/* tone amplitude A gives |lp|^2 = A^2 / 4 */
	if (p > 120.0f * 120.0f / 4 && 2 * p > 0.15f * in_power)
		t->on_count = t->on_count > 0 ? t->on_count + 1 : 1;
	else
		t->on_count = t->on_count < 0 ? t->on_count - 1 : -1;
	/* 20 ms to switch on or off: a phase reversal dips the level */
	if (t->on_count > srate / 50)
		t->on = 1;
	else if (t->on_count < -srate / 50)
		t->on = 0;
	if (t->hold > 0)
		t->hold--;
	else if (t->on && po > p * 0.25f && p > po * 0.25f) {
		cplx c = cplx_mulc(t->lp2, old);
		if (c.re < -0.7f * sqrtf(p * po)) {
			rev = 1;
			t->hold = srate / 20;
		}
	}
	return rev;
}


/*
 *    transmitter (runs at the symbol rate)
 */

static void v32_set_tx(struct v32_dp *v, enum v32_txm m)
{
	v->txm = m;
	v->tx_syms = 0;
}

static cplx abcd_point(struct v32_dp *v, int k)
{
	v->tx_last_abcd = k;
	return v32_abcd[k];
}

/* B1: the data mode starts with scrambled binary ones for 128 symbols
   (V.32bis 5.4), the remote's receiver may train on them */
#define V32_B1_SYMS 128

static cplx v32_tx_word_symbol(struct v32_dp *v, unsigned word, int data)
{
	int b[2], k, q;
	for (k = 0; k < 2; k++) {
		int bit;
		if (data) {
			u8 x = 1;
			if (v->tx_syms >= V32_B1_SYMS)
				modem_get_bits(v->modem, 1, &x, 1);
			bit = x & 1;
		} else {
			bit = (word >> (15 - v->tx_word_bit)) & 1;
			v->tx_word_bit = (v->tx_word_bit + 1) & 15;
		}
		b[k] = scramble(&v->tx_scr, v->tx_scr_tap, bit);
	}
	q = b[0] | b[1] << 1;
	v->tx_4800 = v32_4800_enc[v->tx_4800][q];
	return v32_points_4800[v->tx_4800];
}

static cplx v32_tx_data_symbol(struct v32_dp *v)
{
	const struct v32_rate *r = v->tx_rate;
	int q = 0, k, idx;
	u8 bits[6];

	if (r->nbits == 2)
		return v32_tx_word_symbol(v, 0, 1);
	if (v->tx_syms < V32_B1_SYMS)
		memset(bits, 1, r->nbits);
	else
		modem_get_bits(v->modem, 1, bits, r->nbits);
	for (k = 0; k < r->nbits; k++)
		q |= scramble(&v->tx_scr, v->tx_scr_tap, bits[k] & 1) << k;
	idx = trellis_encode(&v->tx_trellis, q, r->nbits);
	return r->points[idx];
}

static cplx v32_get_symbol(void *ctx)
{
	struct v32_dp *v = ctx;
	cplx z = { 0, 0 };
	int b1, b2;

	switch (v->txm) {
	case TXM_SILENCE:
	case TXM_ANS:
		break;
	case TXM_AA:
		z = abcd_point(v, 0);
		break;
	case TXM_CC:
		z = abcd_point(v, 2);
		break;
	case TXM_AC:
		if (v->tx_reverse)
			v->tx_reverse = 0;
		else
			v->tx_ac_phase ^= 1;
		z = abcd_point(v, v->tx_ac_phase ? 2 : 0);
		break;
	case TXM_S:
		z = abcd_point(v, v->tx_syms & 1);
		break;
	case TXM_SBAR:
		z = abcd_point(v, 2 + (v->tx_syms & 1));
		break;
	case TXM_TRN2:
		z = abcd_point(v, scramble(&v->tx_scr, v->tx_scr_tap, 1) ? 2 : 0);
		break;
	case TXM_TRN4:
		b1 = scramble(&v->tx_scr, v->tx_scr_tap, 1);
		b2 = scramble(&v->tx_scr, v->tx_scr_tap, 1);
		z = abcd_point(v, trn_dibit_abcd[b1 << 1 | b2]);
		v->tx_4800 = abcd_to_4800[v->tx_last_abcd];
		break;
	case TXM_R:
	case TXM_E:
		z = v32_tx_word_symbol(v, v->tx_word, 0);
		break;
	case TXM_DATA:
		z = v32_tx_data_symbol(v);
		break;
	}
	v->tx_syms++;
	return z;
}


/*
 *    receiver
 */

static void v32_rx_reset(struct v32_dp *v)
{
	qam_rx_reset(&v->rx);
	v->rxp = RX_IDLE;
	v->run_s = 0;
	v->rx_nbits = 0;
	v->rx_e_seen = 0;
}

static void v32_rx_word_bit(struct v32_dp *v, int bit)
{
	unsigned w;

	v->rx_bits = (v->rx_bits << 1) | (bit & 1);
	if (++v->rx_nbits < 32)
		return;
	w = v->rx_bits & 0xffff;
	if ((w & RB_SYNC) != RB_SYNC)
		return;
	if ((w & RB_HEADER) == 0 && (v->rx_bits >> 16) == w &&
	    (w & RB_RATES) && v->rxp == RX_R && w == v->rx_r_word) {
		/* E has to come on the same 16 bit boundaries as R; follow
		   them, a clock offset can slip a symbol */
		v->rx_word_align = v->rx_nbits & 15;
	} else if ((w & RB_HEADER) == 0 && (v->rx_bits >> 16) == w &&
	    (w & RB_RATES) && w != v->rx_r_word) {
		v->rx_word_align = v->rx_nbits & 15;
		v->rx_r_word = w;
		v->r_word = w;
		DSP_DBG("v32: rx quality: mse %.4f (%.1f dB)\n", v->rx_mse,
			-10 * log10f(v->rx_mse + 1e-6f));
		v->ev_r = 1;
		v->rxp = RX_R;
		DSP_DBG("v32: R word %04X\n", w);
	} else if (v->rxp == RX_R && (w & RB_HEADER) == RB_HEADER &&
		   (v->rx_nbits & 15) == v->rx_word_align &&
		   rate_of_word(w) && (w & RB_RATES) == rate_of_word(w)->bits &&
		   v->rx_mse < 0.1f && !v->rx_e_seen) {
		v->rx_e_seen = 1;
		v->rx_rate = rate_of_word(w);
		v->ev_e = 1;
		v->rxp = RX_DATA;
		viterbi_init(&v->vit, v->rx_rate->points, v->rx_rate->nbits);
		/* trained: slow timing loop, its jitter hurts the dense
		   constellations */
		v->rx.kt_p = 0.004f * v->rx.sps;
		v->rx.kt_gi = 0.003f;
		DSP_DBG("v32: E word %04X, rx rate %d\n", w, v->rx_rate->bps);
	}
}

static void v32_rx_symbol(struct v32_dp *v)
{
	cplx z = v->rx.z, d, st;
	int k, q, sgn;
	float a;

	/* while we send our own training the line carries mostly our echo
	   (the echo canceller is still learning): don't listen */
	if (v->txm == TXM_S || v->txm == TXM_SBAR ||
	    v->txm == TXM_TRN2 || v->txm == TXM_TRN4) {
		v->run_s = 0;
		v->last_z = z;
		return;
	}

	/* signal quality: mean squared distance to the ABCD grid */
	if (v->rxp != RX_DATA) {
		k = nearest_abcd(z);
		v->rx_mse += 0.05f * (cplx_pow(cplx_sub(z, v32_abcd[k])) /
				      V32_POWER - v->rx_mse);
	}

	/* S: steps of +-90 degrees with alternating sign; the first
	   S-bar symbol (C) repeats the sign.  S can start over at any
	   time before data (a new training). */
	if (v->rxp != RX_DATA) {
		st = cplx_mulc(z, v->last_z);
		a = atan2f(st.im, st.re);
		sgn = a > 0 ? 1 : -1;
		if (fabsf(a) > 0.9f && fabsf(a) < 2.2f && v->carrier) {
			if (sgn != v->last_sgn) {
				if (++v->run_s >= 24 && v->rxp != RX_S) {
					/* whatever was learned on silence or
					   noise is useless: start over */
					if (v->rxp != RX_IDLE)
						qam_rx_reset(&v->rx);
					v->rxp = RX_S;
					v->ev_s = 1;
					DSP_DBG("v32: S detected\n");
				}
			} else if (v->rxp == RX_S && v->run_s >= 24) {
				/* S-bar: this symbol is C */
				qam_rx_align_phase(&v->rx, v32_abcd[2]);
				v->rxp = RX_TRN;
				v->rx_4800 = abcd_to_4800[2];
				v->rx_nbits = 0;
				v->rx_r_word = 0;
				v->run_s = 0;
				DSP_DBG("v32: S-bar detected\n");
			} else
				v->run_s = 0;
		} else
			v->run_s = 0;
		v->last_sgn = sgn;
		/* noise can hide the S to S-bar step: once S is clearly over,
		   go on with the training anyway (everything after it is
		   differential, the absolute phase is not needed) */
		if (v->rxp == RX_S) {
			if (v->run_s >= 24)
				v->s_miss = 0;
			else if (++v->s_miss >= 24) {
				v->rxp = RX_TRN;
				v->rx_nbits = 0;
				v->rx_r_word = 0;
				DSP_DBG("v32: end of S\n");
			}
		}
	}

	switch (v->rxp) {
	case RX_IDLE:
	case RX_S:
		/* keep the phase locked on the ABCD grid, no eq training on
		   the two tone S signal */
		k = nearest_abcd(z);
		qam_rx_adapt(&v->rx, v32_abcd[k], 0, 0.05f, 0);
		break;
	case RX_TRN:
	case RX_R:
		k = nearest_abcd(z);
		/* no training on noise while the remote is silent */
		if (v->rx_mse < 0.15f)
			qam_rx_adapt(&v->rx, v32_abcd[k], 0.02f, 0.05f, 0.0005f);
		else
			qam_rx_adapt(&v->rx, v32_abcd[k], 0.002f, 0.05f, 0);
		/* 4800 bit/s decoding for the R and E words */
		q = abcd_to_4800[k];
		for (k = 0; k < 4; k++)
			if (v32_4800_enc[v->rx_4800][k] == q)
				break;
		v->rx_4800 = q;
		v32_rx_word_bit(v, descramble(&v->rx_scr, v->rx_scr_tap, k & 1));
		v32_rx_word_bit(v, descramble(&v->rx_scr, v->rx_scr_tap, k >> 1));
		if (v->rxp == RX_DATA)
			v->vit.last_y12 = 0;
		break;
	case RX_DATA:
		{
			const struct v32_rate *r = v->rx_rate;
			u8 bits[6];
			int n = r->nbits;
			k = nearest_point(r->points, 1 << (n == 2 ? 2 : n + 1), z);
			d = r->points[k];
			v->rx_mse += 0.01f * (cplx_pow(cplx_sub(z, d)) / V32_POWER -
					      v->rx_mse);
			if (v->rx.symbols % 50 == 0)
				DSP_TRACE("%s data mse %.4f t %.3f tadj %.5f th %.2f dth %.5f\n",
					  v->caller ? "C" : "A", v->rx_mse, v->rx.t_next,
					  v->rx.t_adj, v->rx.theta, v->rx.dtheta);
			qam_rx_adapt(&v->rx, d, 0.004f, 0.02f, 0.0002f);
			if (n == 2) {
				for (q = 0; q < 4; q++)
					if (v32_4800_enc[v->rx_4800][q] == k)
						break;
				v->rx_4800 = k;
			} else
				q = viterbi_decode(&v->vit, z, NULL);
			if (q >= 0 && v->state == V32_DATA) {
				for (k = 0; k < n; k++)
					bits[k] = descramble(&v->rx_scr,
							     v->rx_scr_tap, q >> k);
				modem_put_bits(v->modem, 1, bits, n);
			} else if (q >= 0)
				for (k = 0; k < n; k++)
					descramble(&v->rx_scr, v->rx_scr_tap, q >> k);
		}
		break;
	}
	v->last_z = z;
}

static void v32_rx_sample(struct v32_dp *v, float x)
{
	x = echo_run(&v->ec, v->last_tx, x, v->ec_mu);

	if (tone_run(&v->ac, x, v->rx.power, v->srate) && v->caller)
		v->ev_rev = 1;
	if (tone_run(&v->aa, x, v->rx.power, v->srate) && !v->caller)
		v->ev_rev = 1;
	tone_run(&v->usb1, x, v->rx.power, v->srate);
	tone_run(&v->ans2100, x, v->rx.power, v->srate);
	v->ev_ac = v->ac.on;
	v->ev_aa = v->aa.on;
	/* V.22 unscrambled ones (2250 Hz), not the 2100 Hz answer tone */
	v->ev_usb1 = v->usb1.on && !v->ac.on &&
		cplx_pow(v->usb1.lp2) > 4 * cplx_pow(v->ans2100.lp2);

	if (qam_rx_sample(&v->rx, x))
		v32_rx_symbol(v);

	if (!v->carrier && v->rx.power > tone_power(150)) {
		v->carrier = 1;
		if (v->rxp == RX_IDLE)
			qam_rx_reset(&v->rx);
	} else if (v->carrier && v->rx.power < tone_power(90)) {
		v->carrier = 0;
		if (v->rxp != RX_DATA)
			v32_rx_reset(v);
	}
	if (v->carrier)
		v->carrier_run = v->carrier_run > 0 ? v->carrier_run + 1 : 1;
	else
		v->carrier_run = v->carrier_run < 0 ? v->carrier_run - 1 : -1;
}


/*
 *    handshake
 */

static void v32_set_state(struct v32_dp *v, enum v32_state s)
{
	DSP_DBG("v32: state %d -> %d at %ld ms\n", v->state, s,
		v->total * 1000 / v->srate);
	v->state = s;
	v->count = 0;
}

static void v32_start_word(struct v32_dp *v, enum v32_txm m, unsigned w)
{
	v->tx_word = w;
	v->tx_word_bit = 0;
	v32_set_tx(v, m);
}

static int v32_connect(struct v32_dp *v)
{
	v32_set_state(v, V32_DATA);
	modem_set_param(v->modem, MDMPRM_TX_RATE, v->tx_rate->bps);
	modem_set_param(v->modem, MDMPRM_RX_RATE, v->rx_rate->bps);
	if (v->retraining) {
		/* the link goes on (V.42 knows nothing of it) */
		v->retraining = 0;
		DSP_DBG("v32: retrained, %d/%d\n", v->tx_rate->bps,
			v->rx_rate->bps);
		return DPSTAT_OK;
	}
	DSP_DBG("v32: connect %d/%d\n", v->tx_rate->bps, v->rx_rate->bps);
	return DPSTAT_CONNECT;
}

/* a retrain (V.32 5.5): the start-up again from AA / AC, the caller's
   from AA, the answerer's from AC; whoever starts it, the other modem
   answers the same way when it hears the signal */
static void v32_retrain(struct v32_dp *v)
{
	DSP_DBG("v32: retrain %d (%s)\n", ++v->retrains,
		v->caller ? "AA" : "AC");
	v->retraining = 1;
	v->automode = 0;                /* no falling back now */
	v32_rx_reset(v);
	v->ev_rev = v->ev_r = v->ev_e = v->ev_s = 0;
	v->timeout = v->total + ms2samples(15000, v->srate);
	v->tx_syms = 0;
	if (v->caller) {
		v32_set_state(v, C_AA);
		v32_set_tx(v, TXM_AA);
	} else {
		v32_set_state(v, A_AC);
		v32_set_tx(v, TXM_AC);
	}
}

static int v32_fallback(struct v32_dp *v)
{
	DSP_DBG("v32: remote is not V.32, falling back to V.22bis\n");
	if (v->rt && !v->caller)
		v->rt->ans_sent = 1;
	modem_set_param(v->modem, MDMPRM_DP_REQUESTED, DP_V22BIS);
	return DPSTAT_CHANGEDP;
}

/* the rates our receiver can take, judged by the decision error on
   the training signal (4 points) */
static unsigned v32_quality_caps(struct v32_dp *v)
{
	static const float min_snr[] = { 25, 22, 19, 16, 0 };
	float snr = -10 * log10f(v->rx_mse + 1e-6f);
	unsigned caps = 0;
	int i;
	for (i = 0; i < V32_NRATES; i++)
		if (snr >= min_snr[i])
			caps |= v32_rates[i].bits;
	/* TCM is shared by the trellis rates */
	if (caps & (RB_7200 | RB_9600 | RB_12000 | RB_14400))
		caps |= RB_TCM;
	return v->caps & (caps | RB_4800);
}

/* the best common rate of our capabilities and the remote's word */
static const struct v32_rate *v32_choose(struct v32_dp *v, unsigned w)
{
	return rate_of_word(w & v32_quality_caps(v));
}

/* called once per transmitted symbol, before the symbol is made */
static int v32_handshake(struct v32_dp *v)
{
	long sym = v->tx_syms;
	const struct v32_rate *r;

	switch (v->state) {
	/* answering modem */
	case A_SILENCE:
		if (v->count >= ms2samples(1000, v->srate)) {
			v32_set_state(v, A_ANS);
			v32_set_tx(v, TXM_ANS);
		}
		break;
	case A_ANS:
		if (v->count >= ms2samples(3300, v->srate)) {
			v32_set_state(v, A_GAP);
			v32_set_tx(v, TXM_SILENCE);
		}
		break;
	case A_GAP:
		if (v->count >= ms2samples(75, v->srate)) {
			v32_set_state(v, A_AC);
			v32_set_tx(v, TXM_AC);
		}
		break;
	case A_AC:
		/* AC for at least 128 symbols and until AA is heard */
		if (sym >= 128 && v->ev_aa) {
			v32_set_state(v, A_CA);
			v->tx_reverse = 1;
			v->ev_rev = 0;
		} else if (v->automode && v->count >= ms2samples(3000, v->srate))
			return v32_fallback(v);
		break;
	case A_CA:
		/* the caller answers with CC */
		if (v->ev_rev) {
			v->ev_rev = 0;
			v32_set_state(v, A_AC2);
			v->tx_reverse = 1;
			v->tx_syms = 0;
		} else if (v->count >= ms2samples(2000, v->srate))
			return DPSTAT_ERROR;
		break;
	case A_AC2:
		if (sym >= 64) {
			v32_set_state(v, A_QUIET);
			v32_set_tx(v, TXM_SILENCE);
		}
		break;
	case A_QUIET:
		if (sym >= 16) {
			v32_set_state(v, A_TRAIN1);
			v32_set_tx(v, TXM_S);
			v->ev_s = 0;
		}
		break;
	case A_TRAIN1:
	case A_TRAIN2:
	case C_TRAIN:
		/* S, S-bar, TRN; our echo canceller learns while the remote
		   is silent */
		if (v->txm == TXM_S && sym >= 256)
			v32_set_tx(v, TXM_SBAR);
		else if (v->txm == TXM_SBAR && sym >= 16) {
			v32_set_tx(v, TXM_TRN2);
			if (v->state != A_TRAIN2)
				v->ec_mu = 0.05f;
			/* TRN starts from a fixed scrambler state (as found
			   in recordings of the original library, whose
			   receiver trains on the exact sequence) */
			v->tx_scr = v->caller ? 0x673800 : 0x0FF800;
		}
		else if (v->txm == TXM_TRN2 && sym >= 256) {
			v32_set_tx(v, TXM_TRN4);
			/* and so does the 4 point part */
			v->tx_scr = v->caller ? 0x221837 : 0x683C9F;
			if (v->state != A_TRAIN2)
				v->ec_mu = 0.05f;
		} else if (v->txm == TXM_TRN4 && sym >= V32_TRN_LEN) {
			v->ec_mu = 0;
			if (v->state == A_TRAIN1) {
				v32_set_state(v, A_R1);
				v32_start_word(v, TXM_R, RB_B4 | RB_SYNC | v->caps);
			} else if (v->state == A_TRAIN2) {
				v32_set_state(v, A_R3);
				v32_start_word(v, TXM_R, final_word(v->tx_rate));
				v->ev_e = 0;
			} else {
				v32_set_state(v, C_R2);
				v32_start_word(v, TXM_R, RB_B4 | RB_SYNC |
					       (v->r_word & v->r2_caps));
				v->ev_r = 0;
			}
		}
		break;
	case A_R1:
		/* R1 until the caller starts its training */
		if (v->ev_s && v->tx_word_bit == 0) {
			v32_set_state(v, A_WAIT_R2);
			v32_set_tx(v, TXM_SILENCE);
			v->ev_r = 0;
		}
		break;
	case A_WAIT_R2:
		if (v->ev_r) {
			v->ev_r = 0;
			r = v32_choose(v, v->r_word);
			if (!r) {
				DSP_DBG("v32: no common rate in %04X\n", v->r_word);
				return DPSTAT_ERROR;
			}
			v->tx_rate = r;
			v32_set_state(v, A_TRAIN2);
			v32_set_tx(v, TXM_S);
		}
		break;
	case A_R3:
		if (v->ev_e && v->tx_word_bit == 0) {
			v32_set_state(v, A_E);
			v32_start_word(v, TXM_E, final_word(v->tx_rate) | RB_HEADER);
		}
		break;
	case A_E:
	case C_E:
		if (v->txm == TXM_E && sym >= 8) {
			v32_set_tx(v, TXM_DATA);
			v->tx_trellis.diff = 0;
			v->tx_trellis.conv = 0;
		}
		if (v->txm == TXM_DATA && v->rxp == RX_DATA)
			return v32_connect(v);
		break;

	/* calling modem */
	case C_AA:
		if (v->ev_rev && v->ev_ac) {
			v->ev_rev = 0;
			v32_set_state(v, C_AA_DELAY);
			v->tx_syms = 0;
		} else if (v->automode && v->ev_usb1 &&
			   v->usb1.on_count > ms2samples(100, v->srate))
			return v32_fallback(v);
		break;
	case C_AA_DELAY:
		/* CC goes out 64 symbols after the reversal; our detector
		   sees it about 26 symbols late.  The answerer measures the
		   round trip delay from this, and the original library fails
		   its training when the estimate is too long. */
		if (sym >= 38) {
			v32_set_state(v, C_CC);
			v32_set_tx(v, TXM_CC);
			v->ev_rev = 0;
		}
		break;
	case C_CC:
		if (v->ev_rev) {
			v32_set_state(v, C_WAIT_R1);
			v32_set_tx(v, TXM_SILENCE);
			v->ev_r = 0;
		} else if (v->count >= ms2samples(2000, v->srate))
			return DPSTAT_ERROR;
		break;
	case C_WAIT_R1:
		if (v->ev_r) {
			v->ev_r = 0;
			v->r2_caps = v32_quality_caps(v);
			DSP_DBG("v32: rates we can receive: %04X\n", v->r2_caps);
			v32_set_state(v, C_TRAIN);
			v32_set_tx(v, TXM_S);
		}
		break;
	case C_R2:
		/* R2 until R3 arrives */
		if (v->ev_r && v->tx_word_bit == 0) {
			v->ev_r = 0;
			r = v32_choose(v, v->r_word);
			if (!r || (v->r_word & RB_RATES) != r->bits) {
				/* not a final rate word yet (R1 repeated) */
				break;
			}
			v->tx_rate = r;
			v32_set_state(v, C_E);
			v32_start_word(v, TXM_E, final_word(r) | RB_HEADER);
		}
		break;

	case V32_DATA:
		/* the remote retrains: AA from a caller, AC from an
		   answerer (scrambled data has no such tone) */
		if ((!v->caller && v->ev_aa && !v->ev_ac) ||
		    (v->caller && v->ev_ac))
			v32_retrain(v);
		else if (v->retrain_at && v->count >= v->retrain_at &&
			 !v->retrains) {
			v32_retrain(v);
		}
		break;
	}
	return DPSTAT_OK;
}

static int v32_process(struct dp *dp, void *in, void *out, int cnt)
{
	struct v32_dp *v = dp->dp_data;
	s16 *src = in, *dst = out;
	int ret = DPSTAT_OK, r, i;
	float y;

	for (i = 0; i < cnt; i++) {
		/* the handshake acts on symbol boundaries */
		if (v->txm != TXM_ANS && v->tx.t + 1.0f / v->tx.sps >= 1) {
			r = v32_handshake(v);
			if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
				return r;
			if (r != DPSTAT_OK)
				ret = r;
		} else if (v->txm == TXM_ANS || v->txm == TXM_SILENCE) {
			r = v32_handshake(v);
			if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
				return r;
		}

		if (v->txm == TXM_ANS)
			y = DSP_TX_AMPLITUDE * osc_next(&v->ans);
		else
			y = qam_tx_sample(&v->tx);
		v->last_tx = y;
		dst[i] = dsp_clip16(y);

		v32_rx_sample(v, src[i]);

		if (v->state == V32_DATA) {
			if (-v->carrier_run >= v->carrier_loss) {
				DSP_DBG("v32: carrier lost\n");
				return DPSTAT_ERROR;
			}
		} else if (v->total >= v->timeout) {
			DSP_DBG("v32: handshake timeout in state %d\n", v->state);
			return DPSTAT_ERROR;
		}
		v->count++;
		v->total++;
	}
	return ret;
}

static struct dp *v32_create(struct modem *m, enum DP_ID id,
			     int caller, int srate, int max_frag,
			     struct dp_operations *op)
{
	struct v32_dp *v;
	int s7, s10, i;
	long min_rate, max_rate;

	v = calloc(1, sizeof(*v));
	if (!v)
		return NULL;
	v->dp.id = id;
	v->dp.modem = m;
	v->dp.op = op;
	v->dp.dp_data = v;
	v->modem = m;
	v->srate = srate;
	v->caller = caller;
	v->rt = (struct dp_runtime *)modem_get_param(m, MDMPRM_DPRUNTIME);
	v->automode = (id != DP_V32 && id != DP_V32BIS) ||
		(v->rt && v->rt->v32_automode);

	/* rates we offer */
	min_rate = modem_get_param(m, MDMPRM_MIN_RATE);
	max_rate = modem_get_param(m, MDMPRM_MAX_RATE);
	for (i = 0; i < V32_NRATES; i++) {
		const struct v32_rate *r = &v32_rates[i];
		if (id == DP_V32 && r->bps != 9600 && r->bps != 4800)
			continue;
		if (max_rate > 0 && r->bps > max_rate)
			continue;
		if (min_rate > 0 && r->bps < min_rate)
			continue;
		v->caps |= r->bits;
	}
	if (!v->caps)
		v->caps = RB_4800;

	qam_tx_init(&v->tx, srate, 1800, V32_BAUD, V32_ALPHA, V32_SPAN,
		    sqrtf(V32_POWER), V32_TX_AMPLITUDE);
	v->tx.get_symbol = v32_get_symbol;
	v->tx.ctx = v;
	/* symbol centres a quarter symbol off the sample grid: the
	   original library's receiver fails to train at some timing
	   phases, this one works for all its rates */
	v->tx.t = 0.25f;
	osc_set_freq(&v->ans, 2100, srate);
	if (qam_rx_init(&v->rx, srate, 1800, V32_BAUD, V32_ALPHA, V32_SPAN,
			V32_EQ_LEN, V32_POWER) ||
	    echo_init(&v->ec, V32_EC_LEN)) {
		qam_rx_free(&v->rx);
		echo_free(&v->ec);
		free(v);
		return NULL;
	}
	tone_init(&v->aa, 1800, srate);
	tone_init(&v->ac, 600, srate);
	tone_init(&v->usb1, 2250, srate);
	tone_init(&v->ans2100, 2100, srate);

	/* the caller scrambles with GPC, the answerer with GPA */
	v->tx_scr_tap = caller ? 18 : 5;
	v->rx_scr_tap = caller ? 5 : 18;
	v->tx_rate = &v32_rates[V32_NRATES - 1];
	v->rx_rate = v->tx_rate;

	s7 = modem_get_sreg(m, SREG_WAIT_CARRIER_AFTER_DIAL);
	s10 = modem_get_sreg(m, SREG_CARRIER_LOSS_DISCONNECT_TIME);
	v->timeout = (long)(s7 > 0 ? s7 : 50) * srate;
	v->carrier_loss = (long)(s10 > 0 ? s10 : 14) * srate / 10;
	if (getenv("DSP_V32_RETRAIN"))
		v->retrain_at = ms2samples(atoi(getenv("DSP_V32_RETRAIN")), srate);

	if (caller) {
		v->state = C_AA;
		v->txm = TXM_AA;
	} else if (v->rt && v->rt->ans_sent) {
		/* from V.8: the answer tone (ANSam) is already sent */
		v->rt->ans_sent = 0;
		v->state = A_GAP;
		v->txm = TXM_SILENCE;
	} else {
		v->state = A_SILENCE;
		v->txm = TXM_SILENCE;
	}
	DSP_DBG("v32: create %s (dp %d), rates %04X, srate %d\n",
		caller ? "caller" : "answerer", id, v->caps, srate);
	return &v->dp;
}

static int v32_delete(struct dp *dp)
{
	struct v32_dp *v = dp->dp_data;
	qam_rx_free(&v->rx);
	echo_free(&v->ec);
	free(v);
	return 0;
}

static int v32_hangup(struct dp *dp)
{
	return 0;
}

static struct dp_operations v32_operations = {
	.name = "V.32/V.32bis data pump",
	.create = v32_create,
	.delete = v32_delete,
	.process = v32_process,
	.hangup = v32_hangup,
};

/* the V.8/V.34 pump (dp_v34.c) hands over to this one, with the
   V.22bis fallback, when the remote modem does not do V.34 */
static const enum DP_ID v32_ids[] = { DP_V32, DP_V32BIS };

int dp_v32_init(void)
{
	unsigned i;
	for (i = 0; i < sizeof(v32_ids) / sizeof(v32_ids[0]); i++)
		modem_dp_register(v32_ids[i], &v32_operations);
	return 0;
}

void dp_v32_exit(void)
{
	unsigned i;
	for (i = 0; i < sizeof(v32_ids) / sizeof(v32_ids[0]); i++)
		modem_dp_deregister(v32_ids[i], &v32_operations);
}
