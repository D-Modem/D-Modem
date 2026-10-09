/*
 *
 *    dp_v34.c - ITU V.8 start-up and the V.34 data pump
 *
 *    The pump is started for the default mode (DP_V8) and for
 *    AT+MS=34/90/92.  It runs V.8 (ANSam, CM, JM, CJ) and, when both
 *    modems offer V.34, the V.34 start-up and data mode.  Otherwise it
 *    hands over to the V.32bis pump, which falls back to V.22bis.
 *
 *    V.34 start-up, as the original library runs it:
 *
 *    phase 2 (600 baud DPSK control channel, caller on 1200 Hz, answerer
 *    on 2400 Hz; tones A and B are the unmodulated carriers):
 *
 *    answerer: INFO0a, A, A-bar (on INFO0c), A (40 ms after B-bar)
 *              L1 (160 ms) L2 (until B), A (50 ms), A-bar (10 ms),
 *              silence, A (when the caller's L2 is measured), INFO1a
 *              (on INFO1c), silence
 *    caller:   INFO0c, B, B-bar (40 ms after A-bar, 10 ms), silence,
 *              B (when the answerer's L2 is measured), B-bar (40 ms
 *              after A-bar, 10 ms), L1 (160 ms), L2 (until A), INFO1c,
 *              silence
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "v34.h"

#define V34_SRATE       9600
#define CC_BAUD         600
#define CC_SPS          16      /* samples per control channel symbol */
/* the original library's tones, INFO sequences and L2: about 2990 rms */
#define V34_LEVEL       2988.0f
#define V34_TONE_AMP    (V34_LEVEL * 1.41421f)
#define ANSAM_AMP       3200.0f
#define PROBE_PERIOD    64      /* L1/L2 repeat at 150 Hz */
#define TRN3_LEN        5120    /* TRN symbols in phase 3 */
#define TRN4_LEN        5120    /* and in phase 4 (about 1.5 s, as the original) */
/* a reversal reaches the line this long after we decide on it (the
   pulse shaping), and we see the remote's reversal this long after it
   was on the line: answers that must follow 40 ms after a reversal
   wait the rest */
#define CC_TX_DELAY     (4 * CC_SPS)
#define CC_RX_DELAY     (13 * V34_SRATE / 1000)
#define REV_WAIT        (ms2samples(40, V34_SRATE) - CC_TX_DELAY - CC_RX_DELAY)

/* our CM / JM: V-series data call, V.34 and V.32, LAPM, PSTN access */
static const unsigned char v8_msg[] = { 0xe0, 0xc1, 0x45, 0x11, 0x10, 0x2a, 0x0d };
#define V8_MOD1_V34     0x40    /* in the first modulation octet */
#define V8_MOD2_V32     0x01    /* in the first extension octet */

enum v34_state {
	/* V.8 answerer */
	V8A_SILENCE,
	V8A_ANSAM,
	V8A_JM,
	V8A_GAP,
	/* V.8 caller */
	V8C_LISTEN,             /* is the answer tone ANSam? */
	V8C_WAIT,
	V8C_CM,
	V8C_CJ,
	V8C_GAP,
	/* phase 2 answerer */
	P2A_INFO0,
	P2A_A,
	P2A_ABAR,
	P2A_ABAR_40,
	P2A_A_10,
	P2A_L1,
	P2A_L2,
	P2A_A_50,
	P2A_ABAR_10,
	P2A_SILENCE,
	P2A_A2,
	P2A_INFO1,
	/* phase 2 caller */
	P2C_INFO0,
	P2C_B,
	P2C_B_40,
	P2C_BBAR_10,
	P2C_SILENCE,
	P2C_B2,
	P2C_B2_40,
	P2C_BBAR2_10,
	P2C_L1,
	P2C_L2,
	P2C_INFO1,
	P2C_WAIT_INFO1,
	/* phases 3 and 4, answerer */
	P3A_SILENCE,
	P3A_S,
	P3A_SBAR,
	P3A_PP,
	P3A_TRN,
	P3A_J,
	P3A_WAIT,
	P4A_S,
	P4A_SBAR,
	P4A_TRN,
	/* phases 3 and 4, caller */
	P3C_WAIT,
	P3C_S,
	P3C_SBAR,
	P3C_PP,
	P3C_TRN,
	P3C_J,
	P4C_TRN,
	/* both */
	P4_MP,
	P4_E,
	V34_DATA,
};

enum v34_txm {
	TXM_SILENCE,
	TXM_ANSAM,
	TXM_FSK,
	TXM_CC,
	TXM_PROBE,
	TXM_PRIMARY,
};

struct bitq {
	unsigned char b[256];
	int n, pos;
};

/* control channel receiver: non-coherent DPSK at 600 baud */
#define CC_HIST         128

struct cc_rx {
	struct osc lo;
	cplx box[CC_SPS], old[CC_SPS];
	cplx acc;
	int pos;
	float phase_e[CC_SPS];
	int best;
	long n, last;           /* samples, at the last decision */
	float dmin;
	int rbit;               /* a tone reversal in the last symbol */
	int ones;
	float p;                /* |b|^2 of the last symbol */
	int on, on_count;
	int zeros;              /* '0' decisions in a row (tone) */
	unsigned sr;            /* last bits, newest in bit 0 */
	long rev_cand;          /* a 1 after a tone: a reversal if 0 follows */
	int frame_len;          /* > 0: within an INFO frame */
	unsigned char frame[128];
	unsigned char hist[CC_HIST];
	long hist_n;
};

struct info0 {
	int rates;              /* bit 0..: 2743, 2800, 3429, 3000L, 3000H, 3200L, 3200H */
	int allow_3429;
	int power_red;
	int max_diff;
	int cme;
	int c1664;
	int clock;
	int ack;
};

struct info1c {
	int power_red, add_power_red, md;
	struct { int high, preemph, max_rate; } r[V34_NBAUDS];
	int freq_offset;
};

struct info1a {
	int power_red, add_power_red, md;
	int high, preemph, max_rate;
	int baud_ac, baud_ca;
	int freq_offset;
};

#define EC_NEAR_LEN     256     /* 27 ms */
#define EC_FAR_LEN      256     /* 27 ms around the round trip delay */
#define EC_HIST         16384   /* up to 1.7 s of round trip delay */
#define EC_SEARCH       1024    /* samples correlated for the far echo */
#define EC_TRACK_MU     0.0f

struct v34_ec {
	float *hist, *wn, *wf;
	float *rxh;             /* received samples, for the delay search */
	int pos, rpos;
	int on, far_on, far_delay, trained;
	long n, since, filled, far_since;
	float mu, dbg_rx, dbg_err;
};

struct v34_dp {
	struct dp dp;
	struct modem *modem;
	struct dp_runtime *rt;
	int srate;
	int caller;
	enum v34_state state;
	long count, total, timeout;

	/* transmitter */
	enum v34_txm txm;
	struct osc ansam, ansam_am;
	float ansam_sign;
	struct fsk_mod fsk;
	float fsk_phase;
	int fsk_bit;
	struct bitq v8tx;
	int v8tx_repeat;        /* restart the message when it is done */
	struct qam_tx cc;
	float cc_sym;           /* +-1, 0: silence */
	int cc_rev;             /* reverse at the next symbol */
	struct bitq cctx;       /* INFO bits, a 1 reverses the phase */
	long cc_syms;
	float probe[PROBE_PERIOD];
	int probe_pos;
	float probe_gain;
	long probe_start;

	/* V.8 receiver */
	struct fsk_demod v21;
	unsigned v8_sr;         /* bit window, newest in bit 19 */
	int v8_sync;
	int v8_bits;
	unsigned char v8_rx[32], v8_last[32];
	int v8_n, v8_last_n;
	int v8_ok;              /* two identical CM / JM */
	int v8_zeros;           /* zero octets in a row (CJ) */
	int v8_cj;
	unsigned char v8_remote[32];
	int v8_remote_n;
	/* answer tone analysis (caller) */
	struct osc ans_lo;
	cplx ans_lp1, ans_lp2;
	double env_sum, env15_re, env15_im;
	long env_n;
	/* V.32 / V.22 callers during ANSam */
	struct goertzel aa_g;
	int aa_blocks;

	/* phase 2 receiver */
	struct cc_rx ccr;
	int ev_info0, ev_info1, ev_rev;
	long rev_time;
	struct info0 rinfo0;
	struct info1c rinfo1c;
	struct info1a rinfo1a;
	long rtd;               /* round trip delay, samples */
	long rev_sent;          /* when our reversal went out */
	/* the remote's L1/L2 */
	struct goertzel probe_g[2];
	/* the remote's tone A or B during our own L2: exactly on a bin of
	   the 64 sample probe period, where the echo of L2 has nothing */
	struct goertzel tone_g;
	int tone_blocks;
	float pblock_e;
	int probe_blocks;
	long l1_start;
	double pacc[PROBE_PERIOD];
	float pprev[PROBE_PERIOD];
	double ptotal, pdiff;
	long pcount;
	float snr_db;
	float tone_db[26];      /* received level of each 150 Hz tone */
	int rate_of_baud[V34_NBAUDS];   /* receivable rates (N), by probing */

	/* echo canceller (from phase 3 on) */
	struct v34_ec ec;
	float last_tx;

	/* phases 3, 4 and data */
	struct v34_tx ptx;
	struct v34_rx prx;
	int prx_on;

	/* outcome */
	int baud_tx, baud_rx;   /* symbol rate codes */
	int high_tx, high_rx;
	int preemph_tx;
	int max_rate_tx, max_rate_rx;
	int rate_tx, rate_rx;   /* data mode, bit/s */
	long carrier_loss, lost;
};

static void v34_set_state(struct v34_dp *v, enum v34_state s)
{
	DSP_DBG("v34: state %d -> %d at %ld ms\n", v->state, s,
		v->total * 1000 / v->srate);
	v->state = s;
	v->count = 0;
}


/*
 *    bit queues and INFO sequences
 */

static void bq_reset(struct bitq *q)
{
	q->n = q->pos = 0;
}

static void bq_put(struct bitq *q, unsigned val, int nbits)
{
	int i;
	for (i = 0; i < nbits && q->n < (int)sizeof(q->b); i++)
		q->b[q->n++] = (val >> i) & 1;
}

/* the fill bits, frame sync 01110010 (left first in time) */
static void info_start(struct bitq *q)
{
	bq_reset(q);
	bq_put(q, 0xF, 4);
	bq_put(q, 0x4E, 8);     /* 01110010 sent from the left */
}

static void info_end(struct bitq *q)
{
	unsigned crc = v34_crc_bits(q->b + 12, q->n - 12, 0xFFFF);
	int i;
	for (i = 15; i >= 0; i--)
		bq_put(q, (crc >> i) & 1, 1);
	bq_put(q, 0xF, 4);
}

static void make_info0(struct v34_dp *v, struct bitq *q)
{
	info_start(q);
	bq_put(q, 1, 1);        /* 2743 */
	bq_put(q, 1, 1);        /* 2800 */
	bq_put(q, 1, 1);        /* 3429 */
	bq_put(q, 1, 1);        /* 3000 low carrier */
	bq_put(q, 1, 1);        /* 3000 high carrier */
	bq_put(q, 1, 1);        /* 3200 low carrier */
	bq_put(q, 1, 1);        /* 3200 high carrier */
	bq_put(q, 1, 1);        /* 3429 allowed */
	bq_put(q, 0, 1);        /* power reduction */
	bq_put(q, 0, 3);        /* symbol rate difference */
	bq_put(q, 0, 1);        /* CME */
	bq_put(q, 1, 1);        /* 1664 point constellations */
	bq_put(q, 0, 2);        /* internal clock */
	bq_put(q, 0, 1);        /* acknowledge */
	info_end(q);
}

static void make_info1c(struct v34_dp *v, struct bitq *q)
{
	int i;
	info_start(q);
	bq_put(q, 0, 3);
	bq_put(q, 0, 3);
	bq_put(q, 0, 7);        /* no MD */
	for (i = 0; i < V34_NBAUDS; i++) {
		int rate = v->rate_of_baud[i];
		if (!(v->rinfo0.rates & (1 << 2)) && i == V34_BAUD_3429)
			rate = 0;
		bq_put(q, 0, 1);        /* low carrier */
		bq_put(q, 0, 4);        /* no pre-emphasis */
		bq_put(q, rate, 4);
	}
	bq_put(q, 0, 10);       /* frequency offset */
	info_end(q);
}

static void make_info1a(struct v34_dp *v, struct bitq *q)
{
	info_start(q);
	bq_put(q, 0, 3);
	bq_put(q, 0, 3);
	bq_put(q, 0, 7);        /* no MD */
	bq_put(q, v->high_rx, 1);
	bq_put(q, 0, 4);        /* no pre-emphasis */
	bq_put(q, v->max_rate_rx, 4);
	bq_put(q, v->baud_tx, 3);       /* answerer -> caller */
	bq_put(q, v->baud_rx, 3);       /* caller -> answerer */
	bq_put(q, 0, 10);
	info_end(q);
}

/* a field of an INFO frame, least significant bit first in time */
static int bits_val(const unsigned char *b, int first, int last)
{
	int i, val = 0;
	for (i = last; i >= first; i--)
		val = (val << 1) | b[i];
	return val;
}

/* b: the frame from bit 0 (fill); 1 when the CRC at crc_pos is good */
static int info_crc_ok(const unsigned char *b, int crc_pos)
{
	unsigned crc = v34_crc_bits(b + 12, crc_pos - 12, 0xFFFF);
	int i;
	for (i = 0; i < 16; i++)
		if (b[crc_pos + i] != ((crc >> (15 - i)) & 1))
			return 0;
	return 1;
}

static void parse_info0(struct info0 *i, const unsigned char *b)
{
	i->rates = bits_val(b, 12, 18);
	i->allow_3429 = b[19];
	i->power_red = b[20];
	i->max_diff = bits_val(b, 21, 23);
	i->cme = b[24];
	i->c1664 = b[25];
	i->clock = bits_val(b, 26, 27);
	i->ack = b[28];
}

static void parse_info1c(struct info1c *i, const unsigned char *b)
{
	int k;
	i->power_red = bits_val(b, 12, 14);
	i->add_power_red = bits_val(b, 15, 17);
	i->md = bits_val(b, 18, 24);
	for (k = 0; k < V34_NBAUDS; k++) {
		int o = 25 + 9 * k;
		i->r[k].high = b[o];
		i->r[k].preemph = bits_val(b, o + 1, o + 4);
		i->r[k].max_rate = bits_val(b, o + 5, o + 8);
	}
	i->freq_offset = bits_val(b, 79, 88);
}

static void parse_info1a(struct info1a *i, const unsigned char *b)
{
	i->power_red = bits_val(b, 12, 14);
	i->add_power_red = bits_val(b, 15, 17);
	i->md = bits_val(b, 18, 24);
	i->high = b[25];
	i->preemph = bits_val(b, 26, 29);
	i->max_rate = bits_val(b, 30, 33);
	i->baud_ac = bits_val(b, 34, 36);
	i->baud_ca = bits_val(b, 37, 39);
	i->freq_offset = bits_val(b, 40, 49);
}

/* the length of the INFO frame we expect next, up to the end of its CRC */
static int info_expected(struct v34_dp *v)
{
	if (!v->ev_info0)
		return 29 + 16;
	return v->caller ? 50 + 16 : 89 + 16;
}


/*
 *    V.8
 */

static void v8_put_octet(struct bitq *q, unsigned char c)
{
	bq_put(q, 0, 1);
	bq_put(q, c, 8);
	bq_put(q, 1, 1);
}

/* CM (caller) or JM (answerer); a JM repeats what both modems have */
static void v8_make_msg(struct v34_dp *v, struct bitq *q)
{
	int i;
	bq_reset(q);
	bq_put(q, 0x3FF, 10);
	for (i = 0; i < (int)sizeof(v8_msg); i++)
		v8_put_octet(q, v8_msg[i]);
	v->v8tx_repeat = 1;
}

static void v8_make_cj(struct bitq *q)
{
	bq_reset(q);
	v8_put_octet(q, 0);
	v8_put_octet(q, 0);
	v8_put_octet(q, 0);
}

/* is V.34 in the remote's CM / JM? */
static int v8_remote_v34(struct v34_dp *v)
{
	int i;
	for (i = 1; i < v->v8_remote_n; i++)
		if ((v->v8_remote[i] & 0x1F) == 0x05)
			return (v->v8_remote[i] & V8_MOD1_V34) != 0;
	return 0;
}

static void v8_rx_bit(struct v34_dp *v, int bit)
{
	v->v8_sr = (v->v8_sr >> 1) | ((unsigned)bit << 19);
	/* ten ones, then the CM/JM sync octet framed */
	if (v->v8_sr == 0xF03FF) {
		if (v->v8_sync && v->v8_n > 1 && !v->v8_ok) {
			if (v->v8_n == v->v8_last_n &&
			    !memcmp(v->v8_rx, v->v8_last, v->v8_n)) {
				v->v8_ok = 1;
				memcpy(v->v8_remote, v->v8_rx, v->v8_n);
				v->v8_remote_n = v->v8_n;
				DSP_DBG("v34: V.8 %s received, %d octets\n",
					v->caller ? "JM" : "CM", v->v8_n);
			}
			memcpy(v->v8_last, v->v8_rx, v->v8_n);
			v->v8_last_n = v->v8_n;
		}
		v->v8_sync = 1;
		v->v8_n = 0;
		v->v8_bits = 0;
		v->v8_rx[v->v8_n++] = 0xE0;
		return;
	}
	if (!v->v8_sync)
		return;
	if (++v->v8_bits >= 10 && (v->v8_sr & 0x80400) == 0x80000) {
		unsigned char c = (v->v8_sr >> 11) & 0xFF;
		v->v8_bits = 0;
		if (c == 0) {
			if (++v->v8_zeros >= 3)
				v->v8_cj = 1;
		} else
			v->v8_zeros = 0;
		if (v->v8_n < (int)sizeof(v->v8_rx))
			v->v8_rx[v->v8_n++] = c;
	}
}

/* ANSam: 15 Hz amplitude modulation of the 2100 Hz answer tone */
static void ans_analyse(struct v34_dp *v, float x)
{
	const float a = 1.0f / (0.004f * V34_SRATE);
	cplx z = cplx_scale(osc_next_cplx(&v->ans_lo), x);
	float e, w;
	v->ans_lp1.re += a * (z.re - v->ans_lp1.re);
	v->ans_lp1.im += a * (z.im - v->ans_lp1.im);
	v->ans_lp2.re += a * (v->ans_lp1.re - v->ans_lp2.re);
	v->ans_lp2.im += a * (v->ans_lp1.im - v->ans_lp2.im);
	e = sqrtf(cplx_pow(v->ans_lp2));
	w = 2 * (float)M_PI * 15 * v->env_n / V34_SRATE;
	v->env_sum += e;
	v->env15_re += e * cosf(w);
	v->env15_im += e * sinf(w);
	v->env_n++;
}

/* the depth of the 15 Hz modulation, 0.2 for ANSam */
static float ans_am_depth(struct v34_dp *v)
{
	if (v->env_sum <= 0)
		return 0;
	return 2 * sqrt(v->env15_re * v->env15_re + v->env15_im * v->env15_im) /
		v->env_sum;
}


/*
 *    control channel
 */

static cplx cc_symbol(void *ctx)
{
	struct v34_dp *v = ctx;
	cplx z = { 0, 0 };
	if (v->cctx.pos < v->cctx.n) {
		if (v->cctx.b[v->cctx.pos++])
			v->cc_sym = -v->cc_sym;
	} else if (v->cc_rev) {
		v->cc_sym = -v->cc_sym;
		v->cc_rev = 0;
		v->rev_sent = v->total;
	}
	v->cc_syms++;
	z.re = v->cc_sym;
	return z;
}

/* start the tone (A or B), or keep it going */
static void cc_tone(struct v34_dp *v)
{
	if (v->txm != TXM_CC || v->cc_sym == 0) {
		v->cc_sym = 1;
		v->txm = TXM_CC;
	}
}

static void cc_send(struct v34_dp *v)
{
	cc_tone(v);
	v->cctx.pos = 0;
}

static int cc_busy(struct v34_dp *v)
{
	return v->cctx.pos < v->cctx.n || v->cc_rev;
}

/* zero symbols: the pulses in flight die away */
static void cc_silence(struct v34_dp *v)
{
	v->cc_sym = 0;
}

/* our last reversal has been on the line for this long */
static long rev_on_line(struct v34_dp *v)
{
	return v->cc_rev ? -1 : v->total - v->rev_sent - CC_TX_DELAY;
}

static void probe_init(struct v34_dp *v)
{
	/* V.34 Table 11: 150 Hz multiples, without 900, 1200, 1800 and
	   2400 Hz (the control channel carriers and their neighbours) */
	static const short f[] = { 150, 300, 450, 600, 750, 1050, 1350, 1500,
				   1650, 1950, 2100, 2250, 2550, 2700, 2850,
				   3000, 3150, 3300, 3450, 3600, 3750 };
	static const char inv[] = { 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1,
				    0, 1, 1, 1, 1, 0, 0 };
	double e = 0;
	int i, k;
	for (i = 0; i < PROBE_PERIOD; i++) {
		double x = 0;
		for (k = 0; k < (int)sizeof(f) / (int)sizeof(f[0]); k++)
			x += (inv[k] ? -1 : 1) *
				cos(2 * M_PI * f[k] * i / V34_SRATE);
		v->probe[i] = x;
		e += x * x;
	}
	e = sqrt(e / PROBE_PERIOD);
	for (i = 0; i < PROBE_PERIOD; i++)
		v->probe[i] *= V34_LEVEL / e;
}

static void probe_start(struct v34_dp *v)
{
	v->txm = TXM_PROBE;
	v->probe_pos = 0;
	v->probe_gain = 2;      /* L1: 6 dB up */
	v->probe_start = v->total;
}

static void cc_rx_init(struct cc_rx *r, float fc)
{
	memset(r, 0, sizeof(*r));
	osc_set_freq(&r->lo, -fc, V34_SRATE);
}

/* one sample; returns -1, or a decision (0/1) at a symbol centre */
static int cc_rx_sample(struct cc_rx *r, float x)
{
	cplx z = cplx_scale(osc_next_cplx(&r->lo), x), b, d;
	int i = r->pos, bit = -1;
	r->acc = cplx_add(r->acc, cplx_sub(z, r->box[i]));
	r->box[i] = z;
	b = cplx_scale(r->acc, 1.0f / CC_SPS);
	d = cplx_mulc(b, r->old[i]);
	r->old[i] = b;
	/* symbol timing: fast between frames (after the probing signals),
	   slow within them */
	/* the most reversed phase step of this symbol at any sampling
	   phase: a reversal of a tone is clear at one of them, wherever the
	   symbol timing is */
	if (cplx_pow(b) > 0 && d.re < r->dmin * cplx_pow(b))
		r->dmin = d.re / cplx_pow(b);
	r->phase_e[i] += (r->frame_len > 0 ? 0.02f : 0.2f) *
		(sqrtf(cplx_pow(d)) - r->phase_e[i]);
	if (++r->pos == CC_SPS)
		r->pos = 0;
	if (i == r->best && r->n - r->last > CC_SPS / 2) {
		/* (once per symbol when the timing moves) */
		r->last = r->n;
		/* tone amplitude A gives |b| = A / 2 */
		r->p = cplx_pow(b);
		if (r->p > 300.0f * 300.0f / 4)
			r->on_count = r->on_count > 0 ? r->on_count + 1 : 1;
		else
			r->on_count = r->on_count < 0 ? r->on_count - 1 : -1;
		if (r->on_count >= 4)
			r->on = 1;
		else if (r->on_count <= -4)
			r->on = 0;
		if (r->p > 300.0f * 300.0f / 4)
			bit = d.re < 0;
		r->rbit = r->dmin < -0.5f;
		r->dmin = 0;
		/* follow the symbol timing, two samples per symbol at most:
		   a jump would lose or repeat a decision (on a tone all
		   phases look alike) */
		{
			int b = r->best, step;
			float lo = r->phase_e[0];
			for (i = 0; i < CC_SPS; i++) {
				if (r->phase_e[i] > r->phase_e[b] * 1.2f)
					b = i;
				if (r->phase_e[i] < lo)
					lo = r->phase_e[i];
			}
			/* only where the phase steps show the timing: on a
			   tone all phases look alike, and the timing of the
			   frames before has to stay for its reversal */
			if (r->phase_e[b] < 1.5f * lo)
				b = r->best;
			step = (b - r->best + CC_SPS + CC_SPS / 2) % CC_SPS -
				CC_SPS / 2;
			if (step > 2)
				step = 2;
			else if (step < -2)
				step = -2;
			r->best = (r->best + step + CC_SPS) % CC_SPS;
		}
	}
	r->n++;
	return bit;
}

/* the remote's L1/L2, once per sample: returns -1, or at the end of a
   block of one period whether two of the probe tones are clearly there
   (each carries 1/21 of the power) */
static int probe_rx_sample(struct v34_dp *v, float x)
{
	float e;
	int on;
	v->pblock_e += x * x;
	goertzel_run(&v->probe_g[0], x);
	if (!goertzel_run(&v->probe_g[1], x))
		return -1;
	e = v->pblock_e / PROBE_PERIOD;
	v->pblock_e = 0;
	on = e > tone_power(300) && v->probe_g[0].power > e / 60 &&
		v->probe_g[1].power > e / 60;
	return on;
}

/* collect one sample of the remote's L2 */
static void probe_collect(struct v34_dp *v, float x)
{
	int i = v->pcount % PROBE_PERIOD;
	v->pacc[i] += x;
	v->ptotal += (double)x * x;
	/* the noise from the differences of successive periods: a sample
	   clock offset does not count as noise then */
	if (v->pcount >= PROBE_PERIOD)
		v->pdiff += (double)(x - v->pprev[i]) * (x - v->pprev[i]);
	v->pprev[i] = x;
	v->pcount++;
}

static const int v34_max_rate_of_baud[V34_NBAUDS] = { 9, 11, 11, 12, 13, 14 };

/* the remote's L2: the signal to noise ratio after equalization at
   each symbol rate, and the data rates (in 2400 bit/s steps) they
   allow; returns the best */
#define PROBE_MARGIN 2	/* dB */

static int probe_analyse(struct v34_dp *v)
{
	double periods = (double)v->pcount / PROBE_PERIOD, pt, pn, ptone[26];
	int i, k, n, b, best = 1;

	memset(v->rate_of_baud, 0, sizeof(v->rate_of_baud));
	if (periods < 4)
		return 1;
	pt = v->ptotal / v->pcount;
	pn = v->pdiff / (v->pcount - PROBE_PERIOD) / 2;
	if (pn < pt * 1e-6)
		pn = pt * 1e-6;
	v->snr_db = 10 * log10((pt - pn) / pn);
	for (k = 1; k <= 25; k++) {
		double re = 0, im = 0;
		for (i = 0; i < PROBE_PERIOD; i++) {
			double m = v->pacc[i] / periods;
			re += m * cos(2 * M_PI * k * i / PROBE_PERIOD);
			im += m * sin(2 * M_PI * k * i / PROBE_PERIOD);
		}
		ptone[k] = (re * re + im * im) * 2 / (PROBE_PERIOD * PROBE_PERIOD);
		v->tone_db[k] = 10 * log10(ptone[k] + 1e-3);
	}
	for (b = 0; b < V34_NBAUDS; b++) {
		float rs = v34_symbol_rate(b), fc = v34_carrier(b, 0);
		double inv = 0, snr;
		int cnt = 0;
		for (k = 1; k <= 25; k++) {
			float f = 150.0f * k;
			if (k == 6 || k == 8 || k == 12 || k == 16 ||
			    fabsf(f - fc) > 0.45f * rs)
				continue;
			/* the data signal's density over the noise density
			   (white up to half the sample rate) */
			inv += 1 / (ptone[k] * 21 * (V34_SRATE / 2) / (rs * pn) +
				    1e-9);
			cnt++;
		}
		if (!cnt)
			continue;
		/* what the receiver itself achieves: about 40 dB */
		snr = 10 * log10(1 / (inv / cnt + 1e-4));
		/* with the 16 state code the bit error rate stays below 1e-5
		   from 3 dB per bit per symbol + 4 dB, at every symbol rate;
		   plus a margin for the receiver's own impairments */
		for (n = v34_max_rate_of_baud[b];
		     n > 1 && snr < 3 * 2400.0 * n / rs + 4 + PROBE_MARGIN; n--)
			;
		v->rate_of_baud[b] = n;
		DSP_DBG("v34: probe %d baud: SNR %.1f dB -> %d bit/s\n",
			(int)(rs + 0.5f), snr, n * 2400);
		if (n > best)
			best = n;
	}
	DSP_DBG("v34: probe SNR %.1f dB\n", v->snr_db);
	return best;
}

/*
 *    receiver
 */

static void cc_rx_bit(struct v34_dp *v, int bit)
{
	struct cc_rx *r = &v->ccr;

	if (dsplibs_debug_level >= 6)
		DSP_DBG("v34: cc %s %ld %d p %.0f\n", v->caller ? "C" : "A",
			v->total * 1000 / v->srate, bit, r->p);
	r->sr = (r->sr << 1) | bit;
	r->hist[r->hist_n++ % CC_HIST] = bit;
	if (r->frame_len > 0)
		r->frame_len--;
	/* a frame sync: slow timing adaptation for the rest of the frame */
	if ((r->sr & 0xFF) == 0x72)
		r->frame_len = info_expected(v);
	/* whether the last bits are a frame (fill, sync, CRC): the timing
	   may need a few of the fill bits after the probing signals */
	if (!v->ev_info1) {
		int len = info_expected(v), i, ok;
		if (r->hist_n >= len - 3) {
			for (i = 0; i < len; i++)
				r->frame[i] = i < 4 ? 1 :
					r->hist[(r->hist_n - len + i) % CC_HIST];
			ok = info_crc_ok(r->frame, len - 16);
			for (i = 4; i < 12 && ok; i++)
				ok = r->frame[i] == ((0x72 >> (11 - i)) & 1);
			if (ok) {
				/* no reversals within the frame */
				if (v->ev_rev && v->total - v->rev_time <
				    (long)len * CC_SPS)
					v->ev_rev = 0;
				r->zeros = 0;
				r->rev_cand = 0;
				r->hist_n = 0;
				if (!v->ev_info0) {
					parse_info0(&v->rinfo0, r->frame);
					v->ev_info0 = 1;
					DSP_DBG("v34: INFO0 rates %02x 3429 %d "
						"diff %d\n", v->rinfo0.rates,
						v->rinfo0.allow_3429,
						v->rinfo0.max_diff);
				} else {
					if (v->caller)
						parse_info1a(&v->rinfo1a, r->frame);
					else {
						int b;
						parse_info1c(&v->rinfo1c, r->frame);
						for (b = 0; b < V34_NBAUDS; b++)
							DSP_DBG("v34: INFO1c baud %d high %d "
								"preemph %d rate %d\n", b,
								v->rinfo1c.r[b].high,
								v->rinfo1c.r[b].preemph,
								v->rinfo1c.r[b].max_rate);
					}
					v->ev_info1 = 1;
					DSP_DBG("v34: INFO1 received\n");
				}
				return;
			}
		}
	}
	/* a reversal after a tone; its phase steps can fall into two
	   decisions */
	bit = r->rbit;
	if (bit) {
		if (r->zeros >= 6) {
			r->rev_cand = v->total;
			r->ones = 1;
		} else if (r->rev_cand && r->ones == 1)
			r->ones = 2;
		else
			r->rev_cand = 0;
		r->zeros = 0;
	} else {
		if (r->rev_cand && r->zeros == 0) {
			v->ev_rev = 1;
			v->rev_time = r->rev_cand;
			DSP_DBG("v34: reversal at %ld ms\n",
				r->rev_cand * 1000 / v->srate);
		}
		r->rev_cand = 0;
		r->zeros++;
	}
}

/*
 *    echo canceller
 *
 *    Real pass band NLMS on the line samples, as the original V.34 modems
 *    do it: a near end part over the last EC_NEAR_LEN transmitted samples
 *    and a far end part over EC_FAR_LEN samples around the round trip
 *    delay (the echo from the other end's hybrid; over VoIP it comes back
 *    after the network delay).  Both share one error.  They train while
 *    we send our phase 3 sequences and the remote is silent, fast, then
 *    slower, and track slowly in full duplex.
 */
static int ec_init(struct v34_ec *e)
{
	memset(e, 0, sizeof(*e));
	e->hist = calloc(EC_HIST, sizeof(float));
	e->wn = calloc(EC_NEAR_LEN, sizeof(float));
	e->wf = calloc(EC_FAR_LEN, sizeof(float));
	e->rxh = calloc(EC_SEARCH, sizeof(float));
	return e->hist && e->wn && e->wf && e->rxh ? 0 : -1;
}

static void ec_free(struct v34_ec *e)
{
	free(e->hist);
	free(e->wn);
	free(e->wf);
	free(e->rxh);
	e->hist = e->wn = e->wf = e->rxh = NULL;
}

/* where the far echo is: the received signal of the last EC_SEARCH
   samples against our (scrambled, nearly white) transmitted signal at
   every delay beyond the near part.  The far part then covers
   EC_FAR_LEN samples from a little before the strongest echo. */
static int cmp_float(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y;
}

static void ec_search(struct v34_ec *e)
{
	const int mask = EC_HIST - 1;
	float best = 0, run = 0, c, floor;
	int d, k, best_d = -1;
	/* only where our signal has been sent */
	long n = e->filled - EC_SEARCH;

	if (n > EC_HIST - EC_SEARCH)
		n = EC_HIST - EC_SEARCH;
	if (n < 2 * EC_NEAR_LEN) {
		e->far_on = 0;
		return;
	}
	static float corr[EC_HIST], win[EC_HIST];

	for (d = 0; d < n; d++) {
		c = 0;
		for (k = 0; k < EC_SEARCH; k++)
			c += e->rxh[(e->rpos - k) & (EC_SEARCH - 1)] *
			     e->hist[(e->pos - k - d) & mask];
		corr[d] = c * c;
	}
	/* the echo energy in windows of 32 delays, beyond the near part */
	for (d = 0; d < n; d++) {
		run += corr[d];
		if (d >= 32)
			run -= corr[d - 32];
		win[d] = run;
		if (d >= EC_NEAR_LEN && run > best) {
			best = run;
			best_d = d - 16;
		}
	}
	/* against the median window: the floor without any echo */
	qsort(win, (size_t)n, sizeof(float), cmp_float);
	floor = win[n / 2] + 1e-9f;
	if (best_d < 0 || best < 16 * floor) {
		e->far_on = 0;
		DSP_DBG("v34: no far echo (best at %d, %.1f dB above the "
			"floor)\n", best_d, 10 * log10f(best / floor));
		return;
	}
	d = best_d - EC_FAR_LEN / 4;
	if (d < EC_NEAR_LEN)
		d = EC_NEAR_LEN;
	if (d > EC_HIST - EC_FAR_LEN - 1)
		d = EC_HIST - EC_FAR_LEN - 1;
	e->far_delay = d;
	e->far_on = 1;
	e->far_since = 0;
	DSP_DBG("v34: far echo at %d samples (%.1f dB above the floor)\n",
		best_d, 10 * log10f(best / floor));
}

static float ec_run(struct v34_ec *e, float tx, float rx)
{
	const int mask = EC_HIST - 1;
	float est = 0, en = 1, err, g, h;
	int i, p;

	e->pos = (e->pos + 1) & mask;
	e->hist[e->pos] = tx;
	e->filled++;
	e->rpos = (e->rpos + 1) & (EC_SEARCH - 1);
	if (!e->on) {
		e->rxh[e->rpos] = rx;
		return rx;
	}
	for (i = 0, p = e->pos; i < EC_NEAR_LEN; i++, p = (p - 1) & mask) {
		h = e->hist[p];
		est += e->wn[i] * h;
		en += h * h;
	}
	if (e->far_on)
		for (i = 0, p = (e->pos - e->far_delay) & mask; i < EC_FAR_LEN;
		     i++, p = (p - 1) & mask) {
			h = e->hist[p];
			est += e->wf[i] * h;
			en += h * h;
		}
	err = rx - est;
	/* what the near part leaves, for the far echo search */
	e->rxh[e->rpos] = err;
	/* normalized by the energy in the taps; not on our own silence */
	if (e->mu > 0 && en > 1e4f * (EC_NEAR_LEN + EC_FAR_LEN)) {
		g = e->mu * err / en;
		for (i = 0, p = e->pos; i < EC_NEAR_LEN; i++, p = (p - 1) & mask)
			e->wn[i] += g * e->hist[p];
		if (e->far_on)
			for (i = 0, p = (e->pos - e->far_delay) & mask;
			     i < EC_FAR_LEN; i++, p = (p - 1) & mask)
				e->wf[i] += g * e->hist[p];
	}
	if (dsplibs_debug_level >= 6) {
		e->dbg_rx += 0.001f * (rx * rx - e->dbg_rx);
		e->dbg_err += 0.001f * (err * err - e->dbg_err);
		if ((e->pos & 4095) == 0)
			DSP_DBG("v34: echo canceller mu %.3f in %.0f out %.0f "
				"(%.1f dB)\n", e->mu, e->dbg_rx, e->dbg_err,
				10 * log10f((e->dbg_rx + 1) / (e->dbg_err + 1)));
	}
	return err;
}

/* the adaptation, per sample: train while our own phase 3 sequences go
   out (the remote is silent then), then track */
static void ec_schedule(struct v34_dp *v)
{
	struct v34_ec *e = &v->ec;
	int ours = v->state == P3A_S || v->state == P3A_SBAR ||
		v->state == P3A_PP || v->state == P3A_TRN ||
		v->state == P3C_S || v->state == P3C_SBAR ||
		v->state == P3C_PP || v->state == P3C_TRN;
	int training;

	if (ours && !e->on) {
		e->on = 1;
		e->n = e->since = e->filled = 0;
		e->far_on = 0;
		DSP_DBG("v34: echo canceller on\n");
	}
	if (ours)
		e->since++;
	/* the answerer sends its J until it has heard the caller's S */
	training = ours && (!v->caller ||
		e->since > v->rtd + ms2samples(150, v->srate));
	if (training) {
		e->n++;
		/* the far echo is found after some of our signal went out
		   (and came back) */
		if (!e->far_on && (e->n == EC_SEARCH + ms2samples(50, v->srate) ||
				   e->n == ms2samples(250, v->srate) ||
				   e->n == ms2samples(500, v->srate) ||
				   e->n == ms2samples(1000, v->srate)))
			ec_search(e);
		/* fast at first, also for a far part found late */
		e->far_since++;
		{
			long t = e->n;
			if (e->far_on && e->far_since < t)
				t = e->far_since;
			e->mu = t < ms2samples(300, v->srate) ? 0.5f :
				t < ms2samples(600, v->srate) ? 0.2f : 0.05f;
		}
		e->trained = 1;
	} else if (e->trained)
		e->mu = EC_TRACK_MU;
}

static void pump_rx_sample(struct v34_dp *v, float x)
{
	int bit, k;

	if (v->state < P2A_INFO0) {
		bit = fsk_demod_run(&v->v21, x);
		if (bit >= 0 && v->v21.carrier)
			v8_rx_bit(v, bit);
		if (v->state == V8A_JM || v->state == V8A_GAP ||
		    v->state == V8C_CJ || v->state == V8C_GAP) {
			bit = cc_rx_sample(&v->ccr, x);
			if (bit >= 0)
				cc_rx_bit(v, bit);
		}
		if (v->state == V8C_LISTEN)
			ans_analyse(v, x);
		if (goertzel_run(&v->aa_g, x)) {
			if (v->aa_g.power > tone_power(300))
				v->aa_blocks++;
			else
				v->aa_blocks = 0;
		}
		return;
	}
	if (v->prx_on) {
		ec_schedule(v);
		x = ec_run(&v->ec, v->last_tx, x);
		v34_rx_sample(&v->prx, x);
		return;
	}
	bit = cc_rx_sample(&v->ccr, x);
	if (bit >= 0)
		cc_rx_bit(v, bit);
	if (goertzel_run(&v->tone_g, x))
		v->tone_blocks = v->tone_g.power > tone_power(150) ?
			v->tone_blocks + 1 : 0;
	k = probe_rx_sample(v, x);
	if (k >= 0) {
		v->probe_blocks = k ? v->probe_blocks + 1 : 0;
		if (v->probe_blocks == 2 && !v->l1_start &&
		    (v->state == P2A_SILENCE || v->state == P2C_SILENCE)) {
			v->l1_start = v->total - 2 * PROBE_PERIOD;
			DSP_DBG("v34: remote L1 at %ld ms\n",
				v->l1_start * 1000 / v->srate);
		}
	}
	/* measure L2: from 20 ms after L1 to the end of our wait */
	if (v->l1_start && v->total - v->l1_start >= ms2samples(180, v->srate) &&
	    v->total - v->l1_start < ms2samples(460, v->srate))
		probe_collect(v, x);
}


/*
 *    handshake, once per sample
 */

static int v34_fallback(struct v34_dp *v, const char *why)
{
	DSP_DBG("v34: %s, falling back to V.32bis\n", why);
	if (v->rt) {
		v->rt->v32_automode = 1;
		if (!v->caller)
			v->rt->ans_sent = 1;
	}
	modem_set_param(v->modem, MDMPRM_DP_REQUESTED, DP_V32BIS);
	return DPSTAT_CHANGEDP;
}

/* the answerer picks the symbol rates: the fastest both modems allow */
static void choose_bauds(struct v34_dp *v)
{
	int b, best = V34_BAUD_2400, best_rate = 0;

	probe_analyse(v);
	for (b = V34_BAUD_2400; b <= V34_BAUD_3429; b++) {
		int rate = v->rate_of_baud[b];
		if (b == V34_BAUD_3429 && !v->rinfo0.allow_3429)
			continue;
		if (b == V34_BAUD_2743 || b == V34_BAUD_2800)
			continue;
		/* the caller can receive at most this */
		if (v->rinfo1c.r[b].max_rate < rate)
			rate = v->rinfo1c.r[b].max_rate;
		if (rate >= best_rate) {
			best_rate = rate;
			best = b;
		}
	}
	v->baud_tx = v->baud_rx = best;
	v->max_rate_rx = v->rate_of_baud[best] ? v->rate_of_baud[best] : 1;
}

/* the primary channel at the agreed symbol rates */
static void v34_primary_start(struct v34_dp *v)
{
	/* the carriers as the receiving modems asked for them in INFO1c
	   (answerer -> caller) and INFO1a (caller -> answerer) */
	int high = v->caller ? v->high_tx : v->rinfo1c.r[v->baud_tx].high;
	v34_tx_init(&v->ptx, v->baud_tx, high, v->caller);
	v34_rx_init(&v->prx, v->baud_rx, v->caller ? 0 : v->high_rx,
		    v->caller);
	v->prx_on = 1;
	v->txm = TXM_PRIMARY;
}

static void v34_tx_next(struct v34_dp *v, enum v34_state s, enum v34_txseg seg)
{
	v34_set_state(v, s);
	v34_tx_segment(&v->ptx, seg);
}

/* the fastest rate allowed by both MP sequences at the symbol rate */
static int v34_pick_rate(int baud, int max, int mask)
{
	int n;
	for (n = max; n >= 1; n--)
		if ((mask >> (n - 1)) & 1 && v34_rates[baud][n - 1].b)
			return n;
	return 0;
}

static void v34_get_bits(void *ctx, unsigned char *bits, int n)
{
	struct v34_dp *v = ctx;
	memset(bits, 1, n);
	modem_get_bits(v->modem, 1, bits, n);
}

static void v34_put_bits(void *ctx, const unsigned char *bits, int n)
{
	struct v34_dp *v = ctx;
	modem_put_bits(v->modem, 1, (u8 *)bits, n);
}

/* both MP sequences are known: set up the data mode */
static int v34_data_setup(struct v34_dp *v)
{
	const struct v34_mp *l = &v->ptx.mp, *r = &v->prx.mp;
	int mask = l->rate_mask & r->rate_mask, ntx, nrx;
	int ca = l->rate_ca < r->rate_ca ? l->rate_ca : r->rate_ca;
	int ac = l->rate_ac < r->rate_ac ? l->rate_ac : r->rate_ac;
	struct v34_dparm p;

	ntx = v34_pick_rate(v->ptx.baud, v->caller ? ca : ac, mask);
	nrx = v34_pick_rate(v->prx.baud, v->caller ? ac : ca, mask);
	if (r->nonlinear || r->h[0] || r->h[1] || r->h[2] ||
	    r->h[3] || r->h[4] || r->h[5])
		DSP_DBG("v34: remote asks for non-linear %d, "
			"precoding: not supported\n", r->nonlinear);
	if (!ntx || !nrx || r->trellis > 2 || l->trellis > 2 ||
	    v34_data_params(&p, v->ptx.baud, ntx, r->expanded))
		return -1;
	/* each MP asks for the trellis code of the other's transmitter */
	p.states = 16 << r->trellis;
	v34_tx_data(&v->ptx, &p, v34_get_bits, v);
	v->rate_tx = p.rate;
	if (v34_data_params(&p, v->prx.baud, nrx, l->expanded))
		return -1;
	p.states = 16 << l->trellis;
	v34_rx_data(&v->prx, &p, v34_put_bits, v);
	v->rate_rx = p.rate;
	DSP_DBG("v34: data rates tx %d rx %d, trellis tx %d rx %d states\n",
		v->rate_tx, v->rate_rx, 16 << r->trellis, 16 << l->trellis);
	return 0;
}

/* the trellis code we ask the remote to send: 0, 1, 2 for 16, 32, 64
   states */
#ifndef V34_TRELLIS
#define V34_TRELLIS 2
#endif

static void v34_mp_start(struct v34_dp *v)
{
	struct v34_mp *mp = &v->ptx.mp;
	memset(mp, 0, sizeof(*mp));
	mp->type = 1;
	mp->rate_ca = v->caller ? 14 : v->max_rate_rx;
	mp->rate_ac = v->caller ? v->max_rate_rx : 14;
	mp->expanded = 1;
	mp->trellis = V34_TRELLIS;
	mp->rate_mask = 0x3FFF;
	mp->asym = 1;
	v34_tx_next(v, P4_MP, V34_TX_MP);
}

/* the transmitter takes the next V.8 bit with this sample */
static int fsk_bit_edge(struct v34_dp *v)
{
	return v->fsk_phase + 300.0f / v->srate >= 1;
}

static int v34_handshake(struct v34_dp *v)
{
	long c = v->count;

	switch (v->state) {
	/* V.8 answerer */
	case V8A_SILENCE:
		if (c >= ms2samples(1000, v->srate)) {
			v34_set_state(v, V8A_ANSAM);
			v->txm = TXM_ANSAM;
		}
		break;
	case V8A_ANSAM:
		if (v->v8_ok) {
			if (!v8_remote_v34(v))
				return v34_fallback(v, "no V.34 in CM");
			v34_set_state(v, V8A_JM);
			/* the caller's INFO0c follows its CJ by only 75 ms:
			   listen for it already */
			cc_rx_init(&v->ccr, 1200);
			v8_make_msg(v, &v->v8tx);
			fsk_mod_init(&v->fsk, 1650, 1850, V34_TONE_AMP, v->srate);
			v->fsk_phase = 1;
			v->txm = TXM_FSK;
		} else if (v->aa_blocks >= 10)
			return v34_fallback(v, "V.32 caller");
		else if (c >= ms2samples(5000, v->srate))
			return v34_fallback(v, "no CM");
		break;
	case V8A_JM:
		if (v->v8_cj) {
			v34_set_state(v, V8A_GAP);
			v->txm = TXM_SILENCE;
		}
		break;
	case V8A_GAP:
		if (c >= ms2samples(75, v->srate)) {
			v34_set_state(v, P2A_INFO0);
			qam_tx_init(&v->cc, v->srate, 2400, CC_BAUD, 0.5f, 4, 1,
				    V34_TONE_AMP);
			v->cc.get_symbol = cc_symbol;
			v->cc.ctx = v;
			make_info0(v, &v->cctx);
			cc_send(v);
		}
		break;

	/* V.8 caller */
	case V8C_LISTEN:
		if (c >= ms2samples(500, v->srate)) {
			float d = ans_am_depth(v);
			DSP_DBG("v34: answer tone AM depth %.2f\n", d);
			if (d < 0.1f)
				return v34_fallback(v, "answer tone is not ANSam");
			v34_set_state(v, V8C_WAIT);
		}
		break;
	case V8C_WAIT:
		if (v->total >= ms2samples(1000, v->srate)) {
			v34_set_state(v, V8C_CM);
			v8_make_msg(v, &v->v8tx);
			fsk_mod_init(&v->fsk, 980, 1180, V34_TONE_AMP, v->srate);
			v->fsk_phase = 1;
			v->txm = TXM_FSK;
		}
		break;
	case V8C_CM:
		/* finish the octet, then CJ */
		if (v->v8_ok && fsk_bit_edge(v) && v->v8tx.pos % 10 == 0) {
			if (!v8_remote_v34(v))
				return v34_fallback(v, "no V.34 in JM");
			v34_set_state(v, V8C_CJ);
			v8_make_cj(&v->v8tx);
			v->v8tx_repeat = 0;
			cc_rx_init(&v->ccr, 2400);
		} else if (c >= ms2samples(5000, v->srate))
			return v34_fallback(v, "no JM");
		break;
	case V8C_CJ:
		if (v->v8tx.pos >= v->v8tx.n && fsk_bit_edge(v)) {
			v34_set_state(v, V8C_GAP);
			v->txm = TXM_SILENCE;
		}
		break;
	case V8C_GAP:
		if (c >= ms2samples(75, v->srate)) {
			v34_set_state(v, P2C_INFO0);
			qam_tx_init(&v->cc, v->srate, 1200, CC_BAUD, 0.5f, 4, 1,
				    V34_TONE_AMP);
			v->cc.get_symbol = cc_symbol;
			v->cc.ctx = v;
			make_info0(v, &v->cctx);
			cc_send(v);
		}
		break;

	/* phase 2 answerer */
	case P2A_INFO0:
		if (!cc_busy(v))
			v34_set_state(v, P2A_A);
		break;
	case P2A_A:
		if (c >= ms2samples(50, v->srate) && v->ev_info0) {
			v34_set_state(v, P2A_ABAR);
			v->cc_rev = 1;
			v->ev_rev = 0;
		}
		break;
	case P2A_ABAR:
		if (v->ev_rev && v->rev_time > v->rev_sent) {
			v->rtd = v->rev_time - v->rev_sent - CC_TX_DELAY -
				CC_RX_DELAY - ms2samples(40, v->srate);
			DSP_DBG("v34: round trip %ld ms\n",
				v->rtd * 1000 / v->srate);
			v34_set_state(v, P2A_ABAR_40);
			v->count = v->total - v->rev_time;
		}
		break;
	case P2A_ABAR_40:
		if (c >= REV_WAIT) {
			v34_set_state(v, P2A_A_10);
			v->cc_rev = 1;
		}
		break;
	case P2A_A_10:
		if (rev_on_line(v) >= ms2samples(10, v->srate)) {
			v34_set_state(v, P2A_L1);
			probe_start(v);
		}
		break;
	case P2A_L1:
		if (c >= ms2samples(160, v->srate)) {
			v34_set_state(v, P2A_L2);
			v->probe_gain = 1;
		}
		break;
	case P2A_L2:
		/* until the caller answers with B */
		if (v->tone_blocks >= 3 || c >= ms2samples(2000, v->srate)) {
			v34_set_state(v, P2A_A_50);
			cc_tone(v);
		}
		break;
	case P2A_A_50:
		if (c >= ms2samples(50, v->srate)) {
			v34_set_state(v, P2A_ABAR_10);
			v->cc_rev = 1;
		}
		break;
	case P2A_ABAR_10:
		if (rev_on_line(v) >= ms2samples(10, v->srate)) {
			v34_set_state(v, P2A_SILENCE);
			cc_silence(v);
		}
		break;
	case P2A_SILENCE:
		if (v->l1_start && v->total - v->l1_start >=
		    ms2samples(465, v->srate)) {
			v34_set_state(v, P2A_A2);
			cc_tone(v);
		}
		break;
	case P2A_A2:
		if (v->ev_info1) {
			choose_bauds(v);
			v34_set_state(v, P2A_INFO1);
			make_info1a(v, &v->cctx);
			cc_send(v);
		}
		break;
	case P2A_INFO1:
		if (!cc_busy(v)) {
			cc_silence(v);
			v34_set_state(v, P3A_SILENCE);
			v34_primary_start(v);
		}
		break;

	/* phase 2 caller */
	case P2C_INFO0:
		if (!cc_busy(v)) {
			v34_set_state(v, P2C_B);
			v->ev_rev = 0;
		}
		break;
	case P2C_B:
		if (v->ev_info0 && v->ev_rev) {
			v34_set_state(v, P2C_B_40);
			v->count = v->total - v->rev_time;
		}
		break;
	case P2C_B_40:
		if (c >= REV_WAIT) {
			v34_set_state(v, P2C_BBAR_10);
			v->cc_rev = 1;
			v->ev_rev = 0;
		}
		break;
	case P2C_BBAR_10:
		if (rev_on_line(v) >= ms2samples(10, v->srate)) {
			v34_set_state(v, P2C_SILENCE);
			cc_silence(v);
		}
		break;
	case P2C_SILENCE:
		if (v->ev_rev && !v->rtd) {
			v->rtd = v->rev_time - v->rev_sent - CC_TX_DELAY -
				CC_RX_DELAY - ms2samples(40, v->srate);
			DSP_DBG("v34: round trip %ld ms\n",
				v->rtd * 1000 / v->srate);
		}
		if (v->l1_start && v->total - v->l1_start >=
		    ms2samples(465, v->srate)) {
			v->max_rate_rx = probe_analyse(v);
			v34_set_state(v, P2C_B2);
			cc_tone(v);
			v->ev_rev = 0;
		}
		break;
	case P2C_B2:
		if (v->ev_rev) {
			v34_set_state(v, P2C_B2_40);
			v->count = v->total - v->rev_time;
		}
		break;
	case P2C_B2_40:
		if (c >= REV_WAIT) {
			v34_set_state(v, P2C_BBAR2_10);
			v->cc_rev = 1;
		}
		break;
	case P2C_BBAR2_10:
		if (rev_on_line(v) >= ms2samples(10, v->srate)) {
			v34_set_state(v, P2C_L1);
			probe_start(v);
		}
		break;
	case P2C_L1:
		if (c >= ms2samples(160, v->srate)) {
			v34_set_state(v, P2C_L2);
			v->probe_gain = 1;
		}
		break;
	case P2C_L2:
		if (v->tone_blocks >= 3 || c >= ms2samples(2000, v->srate)) {
			v34_set_state(v, P2C_INFO1);
			make_info1c(v, &v->cctx);
			cc_send(v);
		}
		break;
	case P2C_INFO1:
		if (!cc_busy(v)) {
			cc_silence(v);
			v34_set_state(v, P2C_WAIT_INFO1);
		}
		break;
	case P2C_WAIT_INFO1:
		if (v->ev_info1) {
			v->baud_tx = v->rinfo1a.baud_ca;
			v->baud_rx = v->rinfo1a.baud_ac;
			v->high_tx = v->rinfo1a.high;
			v->preemph_tx = v->rinfo1a.preemph;
			if (v->rate_of_baud[v->baud_rx])
				v->max_rate_rx = v->rate_of_baud[v->baud_rx];
			DSP_DBG("v34: symbol rates %d/%d\n",
				v34_bauds[v->baud_tx].nominal,
				v34_bauds[v->baud_rx].nominal);
			v34_set_state(v, P3C_WAIT);
			v34_primary_start(v);
		}
		break;

	/* phase 3 answerer */
	case P3A_SILENCE:
		if (c >= ms2samples(125, v->srate))
			v34_tx_next(v, P3A_S, V34_TX_S);
		break;
	case P3A_S:
		if (v->ptx.n >= 128)
			v34_tx_next(v, P3A_SBAR, V34_TX_SBAR);
		break;
	case P3A_SBAR:
		if (v->ptx.n >= 16)
			v34_tx_next(v, P3A_PP, V34_TX_PP);
		break;
	case P3A_PP:
		if (v->ptx.n >= 288)
			v34_tx_next(v, P3A_TRN, V34_TX_TRN);
		break;
	case P3A_TRN:
		if (v->ptx.n >= TRN3_LEN) {
			v34_tx_next(v, P3A_J, V34_TX_J);
			v->prx.ev_s = 0;
		}
		break;
	case P3A_J:
		/* until the caller's S */
		if (v->prx.ev_s && v->ptx.n % 8 == 0)
			v34_tx_next(v, P3A_WAIT, V34_TX_SILENCE);
		break;
	case P3A_WAIT:
		/* until the caller's J */
		if (v->prx.ev_j)
			v34_tx_next(v, P4A_S, V34_TX_S);
		break;
	case P4A_S:
		if (v->ptx.n >= 128)
			v34_tx_next(v, P4A_SBAR, V34_TX_SBAR);
		break;
	case P4A_SBAR:
		if (v->ptx.n >= 16)
			v34_tx_next(v, P4A_TRN, V34_TX_TRN);
		break;
	case P4A_TRN:
		if (v->ptx.n >= TRN4_LEN)
			v34_mp_start(v);
		break;

	/* phase 3 caller */
	case P3C_WAIT:
		/* the answerer's training has trained our receiver, and it
		   listens from its J on */
		if (v->prx.stage == V34_RX_TRN && v->prx.ev_j)
			v34_tx_next(v, P3C_S, V34_TX_S);
		break;
	case P3C_S:
		if (v->ptx.n >= 128)
			v34_tx_next(v, P3C_SBAR, V34_TX_SBAR);
		break;
	case P3C_SBAR:
		if (v->ptx.n >= 16)
			v34_tx_next(v, P3C_PP, V34_TX_PP);
		break;
	case P3C_PP:
		if (v->ptx.n >= 288)
			v34_tx_next(v, P3C_TRN, V34_TX_TRN);
		break;
	case P3C_TRN:
		if (v->ptx.n >= TRN3_LEN) {
			v34_tx_next(v, P3C_J, V34_TX_J);
			v->prx.ev_s = 0;
			v->prx.ev_j = v->prx.j_count = 0;
		}
		break;
	case P3C_J:
		/* until the answerer's S of phase 4, then J' and TRN */
		if (v->prx.ev_s && v->ptx.n % 8 == 0)
			v34_tx_next(v, P4C_TRN, V34_TX_JP);
		break;
	case P4C_TRN:
		if (v->ptx.seg == V34_TX_TRN && v->ptx.n >= TRN4_LEN)
			v34_mp_start(v);
		break;

	/* phase 4, both */
	case P4_MP:
		if (v->prx.ev_mp && !v->ptx.ack_mp) {
			/* before the remote's E: its data follows at once */
			if (v34_data_setup(v))
				return v34_fallback(v, "no common data rate");
			v->ptx.ack_mp = 1;
		}
		/* E after a complete MP' once the remote has acknowledged
		   ours */
		if (v->prx.ev_mp && v->prx.mp.ack && v->ptx.mp_sent >= 1 &&
		    v->ptx.mp.ack && v->ptx.pos >= v->ptx.nbits)
			v34_tx_next(v, P4_E, V34_TX_E);
		break;
	case P4_E:
		if (v->ptx.seg == V34_TX_DATA && v->prx.ev_e) {
			v34_set_state(v, V34_DATA);
			modem_set_param(v->modem, MDMPRM_TX_RATE, v->rate_tx);
			modem_set_param(v->modem, MDMPRM_RX_RATE, v->rate_rx);
			DSP_DBG("v34: connect %d/%d\n", v->rate_tx, v->rate_rx);
			return DPSTAT_CONNECT;
		}
		break;
	case V34_DATA:
		if (v->prx.on)
			v->lost = 0;
		else if (++v->lost >= v->carrier_loss) {
			DSP_DBG("v34: carrier lost\n");
			return DPSTAT_ERROR;
		}
		break;
	}
	return DPSTAT_OK;
}

static float pump_tx_sample(struct v34_dp *v)
{
	float y = 0;

	switch (v->txm) {
	case TXM_SILENCE:
		break;
	case TXM_ANSAM:
		/* 15 Hz, 20 % amplitude modulation, reversals every 450 ms */
		if (v->count % ms2samples(450, v->srate) == 0 && v->count)
			v->ansam_sign = -v->ansam_sign;
		y = v->ansam_sign * ANSAM_AMP * (1 + 0.2f * osc_next(&v->ansam_am)) *
			osc_next(&v->ansam);
		break;
	case TXM_FSK:
		v->fsk_phase += 300.0f / v->srate;
		if (v->fsk_phase >= 1) {
			v->fsk_phase -= 1;
			if (v->v8tx.pos >= v->v8tx.n && v->v8tx_repeat)
				v->v8tx.pos = 0;
			v->fsk_bit = v->v8tx.pos < v->v8tx.n ?
				v->v8tx.b[v->v8tx.pos++] : 1;
		}
		y = fsk_mod_next(&v->fsk, v->fsk_bit);
		break;
	case TXM_CC:
		y = qam_tx_sample(&v->cc);
		break;
	case TXM_PROBE:
		y = v->probe_gain * v->probe[v->probe_pos];
		if (++v->probe_pos == PROBE_PERIOD)
			v->probe_pos = 0;
		break;
	case TXM_PRIMARY:
		y = v34_tx_sample(&v->ptx);
		break;
	}
	return y;
}

static int v34_process(struct dp *dp, void *in, void *out, int cnt)
{
	struct v34_dp *v = dp->dp_data;
	s16 *src = in, *dst = out;
	int ret = DPSTAT_OK, r, i;

	for (i = 0; i < cnt; i++) {
		r = v34_handshake(v);
		if (r == DPSTAT_CHANGEDP || r == DPSTAT_ERROR)
			return r;
		if (r != DPSTAT_OK)
			ret = r;
		dst[i] = dsp_clip16(pump_tx_sample(v));
		v->last_tx = dst[i];
		pump_rx_sample(v, src[i]);
		if (v->state != V34_DATA && v->total >= v->timeout) {
			DSP_DBG("v34: start-up timeout in state %d\n", v->state);
			return DPSTAT_ERROR;
		}
		v->count++;
		v->total++;
	}
	return ret;
}

static struct dp *v34_create(struct modem *m, enum DP_ID id,
			     int caller, int srate, int max_frag,
			     struct dp_operations *op)
{
	struct v34_dp *v;
	int s7, s10;

	if (srate != V34_SRATE) {
		DSP_DBG("v34: sample rate %d not supported\n", srate);
		return NULL;
	}
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
	s7 = modem_get_sreg(m, SREG_WAIT_CARRIER_AFTER_DIAL);
	v->timeout = (long)(s7 > 0 ? s7 : 50) * srate;
	s10 = modem_get_sreg(m, SREG_CARRIER_LOSS_DISCONNECT_TIME);
	v->carrier_loss = (long)(s10 > 0 ? s10 : 14) * srate / 10;

	if (ec_init(&v->ec)) {
		ec_free(&v->ec);
		free(v);
		return NULL;
	}
	osc_set_freq(&v->ansam, 2100, srate);
	osc_set_freq(&v->ansam_am, 15, srate);
	v->ansam_sign = 1;
	osc_set_freq(&v->ans_lo, -2100, srate);
	goertzel_init(&v->aa_g, 1800, srate / 50, srate);
	goertzel_init(&v->tone_g, caller ? 2400 : 1200, PROBE_PERIOD, srate);
	goertzel_init(&v->probe_g[0], 1050, PROBE_PERIOD, srate);
	goertzel_init(&v->probe_g[1], 3000, PROBE_PERIOD, srate);
	probe_init(v);
	if (caller) {
		/* the answer tone was heard: CM goes back on V.21 channel 1,
		   JM comes on channel 2 */
		fsk_demod_init(&v->v21, 1650, 1850, 300, srate);
		v->state = V8C_LISTEN;
	} else {
		fsk_demod_init(&v->v21, 980, 1180, 300, srate);
		v->state = V8A_SILENCE;
	}
	v->txm = TXM_SILENCE;
	DSP_DBG("v34: create %s (dp %d)\n", caller ? "caller" : "answerer", id);
	return &v->dp;
}

static int v34_delete(struct dp *dp)
{
	struct v34_dp *v = dp->dp_data;
	if (v->prx_on)
		v34_rx_free(&v->prx);
	ec_free(&v->ec);
	free(v);
	return 0;
}

static int v34_hangup(struct dp *dp)
{
	return 0;
}

static struct dp_operations v34_operations = {
	.name = "V.8/V.34 data pump",
	.create = v34_create,
	.delete = v34_delete,
	.process = v34_process,
	.hangup = v34_hangup,
};

/* DP_V8 is what the modem core starts for the default (fastest) mode;
   V.90 and V.92 are not implemented, they start V.34 as well */
static const enum DP_ID v34_ids[] = { DP_V8, DP_V34, DP_V90, DP_V92 };

int dp_v34_init(void)
{
	int i;
	for (i = 0; i < (int)(sizeof(v34_ids) / sizeof(v34_ids[0])); i++)
		modem_dp_register(v34_ids[i], &v34_operations);
	return 0;
}

void dp_v34_exit(void)
{
	int i;
	for (i = 0; i < (int)(sizeof(v34_ids) / sizeof(v34_ids[0])); i++)
		modem_dp_deregister(v34_ids[i], &v34_operations);
}
