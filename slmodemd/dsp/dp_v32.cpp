/*
 *
 *    dp_v32.cpp - ITU V.32 / V.32bis data pump (4800 to 14400 bit/s)
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

#include "pump.h"
#include "qam.h"

namespace dsp {

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
struct ToneTrk {
	Osc lo;
	cplx lp1, lp2;
	float a;
	cplx hist[128];
	int pos, lag;
	int on;
	long on_count;
	int hold;

	void init(float freq, int srate);
	int run(float x, float in_power, int srate);
};

struct V32Pump : Pump {
	static constexpr const char *name = "V.32/V.32bis data pump";

	~V32Pump() { cleanup(); }

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
	QamTx tx;
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
	TrellisEnc tx_trellis;
	const struct v32_rate *tx_rate;
	Osc ans;
	float last_tx;

	/* receiver */
	EchoCanceller ec;
	float ec_mu;
	QamRx rx;
	ToneTrk aa, ac, usb1, ans2100;
	ToneTrk v21m, v23m;     /* FSK answerers' marks */
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
	Viterbi vit;
	int rx_dbits;           /* data bits per symbol */

	/* events from the receiver to the handshake */
	int ev_aa, ev_ac, ev_rev, ev_s, ev_r, ev_e, ev_usb1;
	int retraining;         /* a retrain in data mode (no new CONNECT) */
	int retrains;
	long retrain_at;        /* test: our own retrain this far (samples)
				   into the data, $DSP_V32_RETRAIN ms */
	unsigned r_word;        /* last R word received */

	cplx tx_symbol();
	void set_tx(enum v32_txm m);
	cplx tx_word_symbol(unsigned word, int data);
	cplx tx_data_symbol();
	void rx_reset();
	void rx_word_bit(int bit);
	void rx_symbol();
	void rx_sample(float x);
	void set_state(enum v32_state s);
	void start_word(enum v32_txm m, unsigned w);
	int connect();
	void retrain();
	int fallback_to(enum DP_ID id, const char *mode);
	int fallback();
	unsigned quality_caps();
	const struct v32_rate *choose(unsigned w);
	int handshake();
	cplx abcd_point(int k);
	int start(struct modem *m, enum DP_ID id, int caller_, int srate_, int max_frag);
	void cleanup();
	int process(void *in, void *out, int cnt) override;
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

void ToneTrk::init(float freq, int srate)
{
	memset(this, 0, sizeof(*this));
	lo.set_freq(-freq, srate);
	a = 1.0f / (0.0015f * srate);
	lag = srate / 100;   /* compare with 10 ms ago */
	if (lag > 127)
		lag = 127;
}

/* returns 1 on a phase reversal; the tone has to carry a fair part of
   the total input power (in_power), noise alone does not turn it on */
int ToneTrk::run(float x, float in_power, int srate)
{
	cplx v = cplx_scale(lo.next_cplx(), x), old;
	float p, po;
	int rev = 0;

	lp1.re += a * (v.re - lp1.re);
	lp1.im += a * (v.im - lp1.im);
	lp2.re += a * (lp1.re - lp2.re);
	lp2.im += a * (lp1.im - lp2.im);
	old = hist[(pos - lag) & 127];
	hist[pos++ & 127] = lp2;
	p = cplx_pow(lp2);
	po = cplx_pow(old);
	/* tone amplitude A gives |lp|^2 = A^2 / 4 */
	if (p > 120.0f * 120.0f / 4 && 2 * p > 0.15f * in_power)
		on_count = on_count > 0 ? on_count + 1 : 1;
	else
		on_count = on_count < 0 ? on_count - 1 : -1;
	/* 20 ms to switch on or off: a phase reversal dips the level */
	if (on_count > srate / 50)
		on = 1;
	else if (on_count < -srate / 50)
		on = 0;
	if (hold > 0)
		hold--;
	else if (on && po > p * 0.25f && p > po * 0.25f) {
		cplx c = cplx_mulc(lp2, old);
		if (c.re < -0.7f * sqrtf(p * po)) {
			rev = 1;
			hold = srate / 20;
		}
	}
	return rev;
}

/*
 *    transmitter (runs at the symbol rate)
 */

void V32Pump::set_tx(enum v32_txm m)
{
	txm = m;
	tx_syms = 0;
}

cplx V32Pump::abcd_point(int k)
{
	tx_last_abcd = k;
	return v32_abcd[k];
}

/* B1: the data mode starts with scrambled binary ones for 128 symbols
   (V.32bis 5.4), the remote's receiver may train on them */
#define V32_B1_SYMS 128

cplx V32Pump::tx_word_symbol(unsigned word, int data)
{
	int b[2], k, q;
	for (k = 0; k < 2; k++) {
		int bit;
		if (data) {
			u8 x = 1;
			if (tx_syms >= V32_B1_SYMS)
				modem_get_bits(modem, 1, &x, 1);
			bit = x & 1;
		} else {
			bit = (word >> (15 - tx_word_bit)) & 1;
			tx_word_bit = (tx_word_bit + 1) & 15;
		}
		b[k] = scramble(&tx_scr, tx_scr_tap, bit);
	}
	q = b[0] | b[1] << 1;
	tx_4800 = v32_4800_enc[tx_4800][q];
	return v32_points_4800[tx_4800];
}

cplx V32Pump::tx_data_symbol()
{
	const struct v32_rate *r = tx_rate;
	int q = 0, k, idx;
	u8 bits[6];

	if (r->nbits == 2)
		return tx_word_symbol(0, 1);
	if (tx_syms < V32_B1_SYMS)
		memset(bits, 1, r->nbits);
	else
		modem_get_bits(modem, 1, bits, r->nbits);
	for (k = 0; k < r->nbits; k++)
		q |= scramble(&tx_scr, tx_scr_tap, bits[k] & 1) << k;
	idx = tx_trellis.encode(q, r->nbits);
	return r->points[idx];
}

cplx V32Pump::tx_symbol()
{
	cplx z = { 0, 0 };
	int b1, b2;

	switch (txm) {
	case TXM_SILENCE:
	case TXM_ANS:
		break;
	case TXM_AA:
		z = abcd_point(0);
		break;
	case TXM_CC:
		z = abcd_point(2);
		break;
	case TXM_AC:
		if (tx_reverse)
			tx_reverse = 0;
		else
			tx_ac_phase ^= 1;
		z = abcd_point(tx_ac_phase ? 2 : 0);
		break;
	case TXM_S:
		z = abcd_point(tx_syms & 1);
		break;
	case TXM_SBAR:
		z = abcd_point(2 + (tx_syms & 1));
		break;
	case TXM_TRN2:
		z = abcd_point(scramble(&tx_scr, tx_scr_tap, 1) ? 2 : 0);
		break;
	case TXM_TRN4:
		b1 = scramble(&tx_scr, tx_scr_tap, 1);
		b2 = scramble(&tx_scr, tx_scr_tap, 1);
		z = abcd_point(trn_dibit_abcd[b1 << 1 | b2]);
		tx_4800 = abcd_to_4800[tx_last_abcd];
		break;
	case TXM_R:
	case TXM_E:
		z = tx_word_symbol(tx_word, 0);
		break;
	case TXM_DATA:
		z = tx_data_symbol();
		break;
	}
	tx_syms++;
	return z;
}

static cplx v32_get_symbol(void *ctx)
{
	return static_cast<V32Pump*>(ctx)->tx_symbol();
}

/*
 *    receiver
 */

void V32Pump::rx_reset()
{
	rx.reset();
	rxp = RX_IDLE;
	run_s = 0;
	rx_nbits = 0;
	rx_e_seen = 0;
}

void V32Pump::rx_word_bit(int bit)
{
	unsigned w;

	rx_bits = (rx_bits << 1) | (bit & 1);
	if (++rx_nbits < 32)
		return;
	w = rx_bits & 0xffff;
	if ((w & RB_SYNC) != RB_SYNC)
		return;
	if ((w & RB_HEADER) == 0 && (rx_bits >> 16) == w &&
	    (w & RB_RATES) && rxp == RX_R && w == rx_r_word) {
		/* E has to come on the same 16 bit boundaries as R; follow
		   them, a clock offset can slip a symbol */
		rx_word_align = rx_nbits & 15;
	} else if ((w & RB_HEADER) == 0 && (rx_bits >> 16) == w &&
	    (w & RB_RATES) && w != rx_r_word) {
		rx_word_align = rx_nbits & 15;
		rx_r_word = w;
		r_word = w;
		DSP_DBG("v32: rx quality: mse %.4f (%.1f dB)\n", rx_mse,
			-10 * log10f(rx_mse + 1e-6f));
		ev_r = 1;
		rxp = RX_R;
		DSP_DBG("v32: R word %04X\n", w);
	} else if (rxp == RX_R && (w & RB_HEADER) == RB_HEADER &&
		   (rx_nbits & 15) == rx_word_align &&
		   rate_of_word(w) && (w & RB_RATES) == rate_of_word(w)->bits &&
		   rx_mse < 0.1f && !rx_e_seen) {
		rx_e_seen = 1;
		rx_rate = rate_of_word(w);
		ev_e = 1;
		rxp = RX_DATA;
		vit.init(rx_rate->points, rx_rate->nbits);
		/* trained: slow timing loop, its jitter hurts the dense
		   constellations */
		rx.kt_p = 0.004f * rx.sps;
		rx.kt_gi = 0.003f;
		DSP_DBG("v32: E word %04X, rx rate %d\n", w, rx_rate->bps);
	}
}

void V32Pump::rx_symbol()
{
	cplx z = rx.z, d, st;
	int k, q, sgn;
	float a;

	/* while we send our own training the line carries mostly our echo
	   (the echo canceller is still learning): don't listen */
	if (txm == TXM_S || txm == TXM_SBAR ||
	    txm == TXM_TRN2 || txm == TXM_TRN4) {
		run_s = 0;
		last_z = z;
		return;
	}

	/* signal quality: mean squared distance to the ABCD grid */
	if (rxp != RX_DATA) {
		k = nearest_abcd(z);
		rx_mse += 0.05f * (cplx_pow(cplx_sub(z, v32_abcd[k])) /
				      V32_POWER - rx_mse);
	}

	/* S: steps of +-90 degrees with alternating sign; the first
	   S-bar symbol (C) repeats the sign.  S can start over at any
	   time before data (a new training). */
	if (rxp != RX_DATA) {
		st = cplx_mulc(z, last_z);
		a = atan2f(st.im, st.re);
		sgn = a > 0 ? 1 : -1;
		if (fabsf(a) > 0.9f && fabsf(a) < 2.2f && carrier) {
			if (sgn != last_sgn) {
				if (++run_s >= 24 && rxp != RX_S) {
					/* whatever was learned on silence or
					   noise is useless: start over */
					if (rxp != RX_IDLE)
						rx.reset();
					rxp = RX_S;
					ev_s = 1;
					DSP_DBG("v32: S detected\n");
				}
			} else if (rxp == RX_S && run_s >= 24) {
				/* S-bar: this symbol is C */
				rx.align_phase(v32_abcd[2]);
				rxp = RX_TRN;
				rx_4800 = abcd_to_4800[2];
				rx_nbits = 0;
				rx_r_word = 0;
				run_s = 0;
				DSP_DBG("v32: S-bar detected\n");
			} else
				run_s = 0;
		} else
			run_s = 0;
		last_sgn = sgn;
		/* noise can hide the S to S-bar step: once S is clearly over,
		   go on with the training anyway (everything after it is
		   differential, the absolute phase is not needed) */
		if (rxp == RX_S) {
			if (run_s >= 24)
				s_miss = 0;
			else if (++s_miss >= 24) {
				rxp = RX_TRN;
				rx_nbits = 0;
				rx_r_word = 0;
				DSP_DBG("v32: end of S\n");
			}
		}
	}

	switch (rxp) {
	case RX_IDLE:
	case RX_S:
		/* keep the phase locked on the ABCD grid, no eq training on
		   the two tone S signal */
		k = nearest_abcd(z);
		rx.adapt(v32_abcd[k], 0, 0.05f, 0);
		break;
	case RX_TRN:
	case RX_R:
		k = nearest_abcd(z);
		/* no training on noise while the remote is silent */
		if (rx_mse < 0.15f)
			rx.adapt(v32_abcd[k], 0.02f, 0.05f, 0.0005f);
		else
			rx.adapt(v32_abcd[k], 0.002f, 0.05f, 0);
		/* 4800 bit/s decoding for the R and E words */
		q = abcd_to_4800[k];
		for (k = 0; k < 4; k++)
			if (v32_4800_enc[rx_4800][k] == q)
				break;
		rx_4800 = q;
		rx_word_bit(descramble(&rx_scr, rx_scr_tap, k & 1));
		rx_word_bit(descramble(&rx_scr, rx_scr_tap, k >> 1));
		if (rxp == RX_DATA)
			vit.last_y12 = 0;
		break;
	case RX_DATA:
		{
			const struct v32_rate *r = rx_rate;
			u8 bits[6];
			int n = r->nbits;
			k = nearest_point(r->points, 1 << (n == 2 ? 2 : n + 1), z);
			d = r->points[k];
			rx_mse += 0.01f * (cplx_pow(cplx_sub(z, d)) / V32_POWER -
					      rx_mse);
			if (rx.symbols % 50 == 0)
				DSP_TRACE("%s data mse %.4f t %.3f tadj %.5f th %.2f dth %.5f\n",
					  caller ? "C" : "A", rx_mse, rx.t_next,
					  rx.t_adj, rx.theta, rx.dtheta);
			rx.adapt(d, 0.004f, 0.02f, 0.0002f);
			if (n == 2) {
				for (q = 0; q < 4; q++)
					if (v32_4800_enc[rx_4800][q] == k)
						break;
				rx_4800 = k;
			} else
				q = vit.decode(z, NULL);
			if (q >= 0 && state == V32_DATA) {
				for (k = 0; k < n; k++)
					bits[k] = descramble(&rx_scr,
							     rx_scr_tap, q >> k);
				modem_put_bits(modem, 1, bits, n);
			} else if (q >= 0)
				for (k = 0; k < n; k++)
					descramble(&rx_scr, rx_scr_tap, q >> k);
		}
		break;
	}
	last_z = z;
}

void V32Pump::rx_sample(float x)
{
	x = ec.run(last_tx, x, ec_mu);

	if (ac.run(x, rx.power, srate) && caller)
		ev_rev = 1;
	if (aa.run(x, rx.power, srate) && !caller)
		ev_rev = 1;
	usb1.run(x, rx.power, srate);
	ans2100.run(x, rx.power, srate);
	if (caller && automode) {
		v21m.run(x, rx.power, srate);
		v23m.run(x, rx.power, srate);
	}
	ev_ac = ac.on;
	ev_aa = aa.on;
	/* V.22 unscrambled ones (2250 Hz), not the 2100 Hz answer tone */
	ev_usb1 = usb1.on && !ac.on &&
		cplx_pow(usb1.lp2) > 4 * cplx_pow(ans2100.lp2);

	if (rx.sample(x))
		rx_symbol();

	if (!carrier && rx.power > tone_power(150)) {
		carrier = 1;
		if (rxp == RX_IDLE)
			rx.reset();
	} else if (carrier && rx.power < tone_power(90)) {
		carrier = 0;
		if (rxp != RX_DATA)
			rx_reset();
	}
	if (carrier)
		carrier_run = carrier_run > 0 ? carrier_run + 1 : 1;
	else
		carrier_run = carrier_run < 0 ? carrier_run - 1 : -1;
}

/*
 *    handshake
 */

void V32Pump::set_state(enum v32_state s)
{
	DSP_DBG("v32: state %d -> %d at %ld ms\n", state, s,
		total * 1000 / srate);
	state = s;
	count = 0;
}

void V32Pump::start_word(enum v32_txm m, unsigned w)
{
	tx_word = w;
	tx_word_bit = 0;
	set_tx(m);
}

int V32Pump::connect()
{
	set_state(V32_DATA);
	modem_set_param(modem, MDMPRM_TX_RATE, tx_rate->bps);
	modem_set_param(modem, MDMPRM_RX_RATE, rx_rate->bps);
	if (retraining) {
		/* the link goes on (V.42 knows nothing of it) */
		retraining = 0;
		DSP_DBG("v32: retrained, %d/%d\n", tx_rate->bps,
			rx_rate->bps);
		return DPSTAT_OK;
	}
	DSP_DBG("v32: connect %d/%d\n", tx_rate->bps, rx_rate->bps);
	return DPSTAT_CONNECT;
}

/* a retrain (V.32 5.5): the start-up again from AA / AC, the caller's
   from AA, the answerer's from AC; whoever starts it, the other modem
   answers the same way when it hears the signal */
void V32Pump::retrain()
{
	DSP_DBG("v32: retrain %d (%s)\n", ++retrains,
		caller ? "AA" : "AC");
	retraining = 1;
	automode = 0;                /* no falling back now */
	rx_reset();
	ev_rev = ev_r = ev_e = ev_s = 0;
	timeout = total + ms2samples(15000, srate);
	tx_syms = 0;
	if (caller) {
		set_state(C_AA);
		set_tx(TXM_AA);
	} else {
		set_state(A_AC);
		set_tx(TXM_AC);
	}
}

int V32Pump::fallback_to(enum DP_ID id, const char *mode)
{
	DSP_DBG("v32: remote is not V.32, falling back to %s\n", mode);
	if (rt && !caller)
		rt->ans_sent = 1;
	modem_set_param(modem, MDMPRM_DP_REQUESTED, id);
	return DPSTAT_CHANGEDP;
}

int V32Pump::fallback()
{
	return fallback_to(DP_V22BIS, "V.22bis");
}

/* the rates our receiver can take, judged by the decision error on
   the training signal (4 points) */
unsigned V32Pump::quality_caps()
{
	static const float min_snr[] = { 25, 22, 19, 16, 0 };
	float snr = -10 * log10f(rx_mse + 1e-6f);
	unsigned caps_ = 0;
	int i;
	for (i = 0; i < V32_NRATES; i++)
		if (snr >= min_snr[i])
			caps_ |= v32_rates[i].bits;
	/* TCM is shared by the trellis rates */
	if (caps_ & (RB_7200 | RB_9600 | RB_12000 | RB_14400))
		caps_ |= RB_TCM;
	return caps & (caps_ | RB_4800);
}

/* the best common rate of our capabilities and the remote's word */
const struct v32_rate *V32Pump::choose(unsigned w)
{
	return rate_of_word(w & quality_caps());
}

/* called once per transmitted symbol, before the symbol is made */
int V32Pump::handshake()
{
	long sym = tx_syms;
	const struct v32_rate *r;

	switch (state) {
	/* answering modem */
	case A_SILENCE:
		if (count >= ms2samples(1000, srate)) {
			set_state(A_ANS);
			set_tx(TXM_ANS);
		}
		break;
	case A_ANS:
		if (count >= ms2samples(3300, srate)) {
			set_state(A_GAP);
			set_tx(TXM_SILENCE);
		}
		break;
	case A_GAP:
		if (count >= ms2samples(75, srate)) {
			set_state(A_AC);
			set_tx(TXM_AC);
		}
		break;
	case A_AC:
		/* AC for at least 128 symbols and until AA is heard */
		if (sym >= 128 && ev_aa) {
			set_state(A_CA);
			tx_reverse = 1;
			ev_rev = 0;
		} else if (automode && count >= ms2samples(3000, srate))
			return fallback();
		break;
	case A_CA:
		/* the caller answers with CC */
		if (ev_rev) {
			ev_rev = 0;
			set_state(A_AC2);
			tx_reverse = 1;
			tx_syms = 0;
		} else if (count >= ms2samples(2000, srate))
			return DPSTAT_ERROR;
		break;
	case A_AC2:
		if (sym >= 64) {
			set_state(A_QUIET);
			set_tx(TXM_SILENCE);
		}
		break;
	case A_QUIET:
		if (sym >= 16) {
			set_state(A_TRAIN1);
			set_tx(TXM_S);
			ev_s = 0;
		}
		break;
	case A_TRAIN1:
	case A_TRAIN2:
	case C_TRAIN:
		/* S, S-bar, TRN; our echo canceller learns while the remote
		   is silent */
		if (txm == TXM_S && sym >= 256)
			set_tx(TXM_SBAR);
		else if (txm == TXM_SBAR && sym >= 16) {
			set_tx(TXM_TRN2);
			if (state != A_TRAIN2)
				ec_mu = 0.05f;
			/* TRN starts from a fixed scrambler state (as found
			   in recordings of the original library, whose
			   receiver trains on the exact sequence) */
			tx_scr = caller ? 0x673800 : 0x0FF800;
		}
		else if (txm == TXM_TRN2 && sym >= 256) {
			set_tx(TXM_TRN4);
			/* and so does the 4 point part */
			tx_scr = caller ? 0x221837 : 0x683C9F;
			if (state != A_TRAIN2)
				ec_mu = 0.05f;
		} else if (txm == TXM_TRN4 && sym >= V32_TRN_LEN) {
			if (ec_mu > 0) {
				float erle = ec.judge();
				DSP_DBG("v32: echo canceller took %.1f dB "
					"away%s\n", erle, ec.off ? ": off" : "");
			}
			ec_mu = 0;
			if (state == A_TRAIN1) {
				set_state(A_R1);
				start_word(TXM_R, RB_B4 | RB_SYNC | caps);
			} else if (state == A_TRAIN2) {
				set_state(A_R3);
				start_word(TXM_R, final_word(tx_rate));
				ev_e = 0;
			} else {
				set_state(C_R2);
				start_word(TXM_R, RB_B4 | RB_SYNC |
					       (r_word & r2_caps));
				ev_r = 0;
			}
		}
		break;
	case A_R1:
		/* R1 until the caller starts its training */
		if (ev_s && tx_word_bit == 0) {
			set_state(A_WAIT_R2);
			set_tx(TXM_SILENCE);
			ev_r = 0;
		}
		break;
	case A_WAIT_R2:
		if (ev_r) {
			ev_r = 0;
			r = choose(r_word);
			if (!r) {
				DSP_DBG("v32: no common rate in %04X\n", r_word);
				return DPSTAT_ERROR;
			}
			tx_rate = r;
			set_state(A_TRAIN2);
			set_tx(TXM_S);
		}
		break;
	case A_R3:
		if (ev_e && tx_word_bit == 0) {
			set_state(A_E);
			start_word(TXM_E, final_word(tx_rate) | RB_HEADER);
		}
		break;
	case A_E:
	case C_E:
		if (txm == TXM_E && sym >= 8) {
			set_tx(TXM_DATA);
			tx_trellis.diff = 0;
			tx_trellis.conv = 0;
		}
		if (txm == TXM_DATA && rxp == RX_DATA)
			return connect();
		break;

	/* calling modem */
	case C_AA:
		if (ev_rev && ev_ac) {
			ev_rev = 0;
			set_state(C_AA_DELAY);
			tx_syms = 0;
		} else if (automode && ev_usb1 &&
			   usb1.on_count > ms2samples(100, srate))
			return fallback();
		/* an FSK answerer's steady mark after its answer tone */
		else if (automode &&
			 v21m.on_count > ms2samples(300, srate))
			return fallback_to(DP_V21, "V.21");
		else if (automode && !ans2100.on &&
			 v23m.on_count > ms2samples(300, srate))
			return fallback_to(DP_V23, "V.23");
		break;
	case C_AA_DELAY:
		/* CC goes out 64 symbols after the reversal; our detector
		   sees it about 26 symbols late.  The answerer measures the
		   round trip delay from this, and the original library fails
		   its training when the estimate is too long. */
		if (sym >= 38) {
			set_state(C_CC);
			set_tx(TXM_CC);
			ev_rev = 0;
		}
		break;
	case C_CC:
		if (ev_rev) {
			set_state(C_WAIT_R1);
			set_tx(TXM_SILENCE);
			ev_r = 0;
		} else if (count >= ms2samples(2000, srate))
			return DPSTAT_ERROR;
		break;
	case C_WAIT_R1:
		if (ev_r) {
			ev_r = 0;
			r2_caps = quality_caps();
			DSP_DBG("v32: rates we can receive: %04X\n", r2_caps);
			set_state(C_TRAIN);
			set_tx(TXM_S);
		}
		break;
	case C_R2:
		/* R2 until R3 arrives */
		if (ev_r && tx_word_bit == 0) {
			ev_r = 0;
			r = choose(r_word);
			DSP_DBG("v32: R word %04X in R2 (choice %04X)\n",
				r_word, r ? r->bits : 0);
			if (!r || (r_word & RB_RATES) != r->bits) {
				/* not a final rate word yet (R1 repeated) */
				break;
			}
			tx_rate = r;
			set_state(C_E);
			start_word(TXM_E, final_word(r) | RB_HEADER);
		}
		break;

	case V32_DATA:
		/* the remote retrains: AA from a caller, AC from an
		   answerer (scrambled data has no such tone) */
		if ((!caller && ev_aa && !ev_ac) ||
		    (caller && ev_ac))
			retrain();
		else if (retrain_at && count >= retrain_at &&
			 !retrains) {
			retrain();
		}
		break;
	}
	return DPSTAT_OK;
}

int V32Pump::process(void *in, void *out, int cnt)
{
	s16 *src = static_cast<s16*>(in), *dst = static_cast<s16*>(out);
	int ret = DPSTAT_OK, r, i;
	float y;

	for (i = 0; i < cnt; i++) {
		/* the handshake acts on symbol boundaries */
		if (txm != TXM_ANS && tx.t + 1.0f / tx.sps >= 1) {
			r = handshake();
			if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
				return r;
			if (r != DPSTAT_OK)
				ret = r;
		} else if (txm == TXM_ANS || txm == TXM_SILENCE) {
			r = handshake();
			if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
				return r;
		}

		if (txm == TXM_ANS)
			y = DSP_TX_AMPLITUDE * ans.next();
		else
			y = tx.sample();
		last_tx = y;
		dst[i] = dsp_clip16(y);

		rx_sample(src[i]);

		if (state == V32_DATA) {
			if (-carrier_run >= carrier_loss) {
				DSP_DBG("v32: carrier lost\n");
				return DPSTAT_ERROR;
			}
		} else if (total >= timeout) {
			DSP_DBG("v32: handshake timeout in state %d\n", state);
			return DPSTAT_ERROR;
		}
		count++;
		total++;
	}
	return ret;
}

int V32Pump::start(struct modem *m, enum DP_ID id, int caller_, int srate_, int max_frag)
{
	int s7, s10, i;
	long min_rate, max_rate;

	modem = m;
	srate = srate_;
	caller = caller_;
	rt = (struct dp_runtime *)modem_get_param(m, MDMPRM_DPRUNTIME);
	automode = (id != DP_V32 && id != DP_V32BIS) ||
		(rt && rt->v32_automode);

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
		caps |= r->bits;
	}
	if (!caps)
		caps = RB_4800;

	tx.init(srate_, 1800, V32_BAUD, V32_ALPHA, V32_SPAN,
		    sqrtf(V32_POWER), V32_TX_AMPLITUDE);
	tx.get_symbol = v32_get_symbol;
	tx.ctx = this;
	/* symbol centres a quarter symbol off the sample grid: the
	   original library's receiver fails to train at some timing
	   phases, this one works for all its rates */
	tx.t = 0.25f;
	ans.set_freq(2100, srate_);
	if (rx.init(srate_, 1800, V32_BAUD, V32_ALPHA, V32_SPAN,
			V32_EQ_LEN, V32_POWER) ||
	    ec.init(V32_EC_LEN))
		return -1;
	aa.init(1800, srate_);
	ac.init(600, srate_);
	usb1.init(2250, srate_);
	ans2100.init(2100, srate_);
	v21m.init(1650, srate_);
	v23m.init(1300, srate_);

	/* the caller scrambles with GPC, the answerer with GPA */
	tx_scr_tap = caller_ ? 18 : 5;
	rx_scr_tap = caller_ ? 5 : 18;
	tx_rate = &v32_rates[V32_NRATES - 1];
	rx_rate = tx_rate;

	s7 = modem_get_sreg(m, SREG_WAIT_CARRIER_AFTER_DIAL);
	s10 = modem_get_sreg(m, SREG_CARRIER_LOSS_DISCONNECT_TIME);
	timeout = (long)(s7 > 0 ? s7 : 50) * srate_;
	carrier_loss = (long)(s10 > 0 ? s10 : 14) * srate_ / 10;
	if (getenv("DSP_V32_RETRAIN"))
		retrain_at = ms2samples(atoi(getenv("DSP_V32_RETRAIN")), srate_);

	if (caller_) {
		state = C_AA;
		txm = TXM_AA;
	} else if (rt && rt->ans_sent) {
		/* from V.8: the answer tone (ANSam) is already sent */
		rt->ans_sent = 0;
		state = A_GAP;
		txm = TXM_SILENCE;
	} else {
		state = A_SILENCE;
		txm = TXM_SILENCE;
	}
	DSP_DBG("v32: create %s (dp %d), rates %04X, srate %d\n",
		caller_ ? "caller" : "answerer", id, caps, srate_);
	return 0;
}

void V32Pump::cleanup()
{
	rx.release();
	ec.release();
}

/* the V.8/V.34 pump (dp_v34.cpp) hands over to this one, with the
   V.22bis fallback, when the remote modem does not do V.34 */
static const enum DP_ID v32_ids[] = { DP_V32, DP_V32BIS };

int dp_v32_init(void)
{
	unsigned i;
	for (i = 0; i < sizeof(v32_ids) / sizeof(v32_ids[0]); i++)
		modem_dp_register(v32_ids[i], &PumpOps<V32Pump>::ops);
	return 0;
}

void dp_v32_exit(void)
{
	unsigned i;
	for (i = 0; i < sizeof(v32_ids) / sizeof(v32_ids[0]); i++)
		modem_dp_deregister(v32_ids[i], &PumpOps<V32Pump>::ops);
}

} // namespace dsp
