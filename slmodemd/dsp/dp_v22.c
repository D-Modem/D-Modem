/*
 *
 *    dp_v22.c - ITU V.22 / Bell 212A (1200 bit/s DPSK) and
 *               ITU V.22bis (2400 bit/s 16-QAM) data pump
 *
 *    600 baud, calling side transmits on 1200 Hz, answering side on
 *    2400 Hz.  Constellation and differential quadrant coding follow
 *    the tables of the original library (SMCv22_*): first quadrant
 *    points 00 (1,1), 01 (3,1), 10 (1,3), 11 (3,3); 1200 bit/s uses the
 *    01 point of each quadrant; dibit 00 -> +90, 01 -> 0, 10 -> +180,
 *    11 -> +270 degrees.
 *
 *    Receiver: band pass, down conversion, root raised cosine matched
 *    filter, AGC, cubic interpolator driven by a Gardner timing loop,
 *    T/2 spaced LMS equalizer and a decision directed carrier PLL.
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

#define V22_BAUD      600
#define V22_ALPHA     0.75f
#define V22_SPAN      3                 /* pulse half length, symbols */
#define V22_PULSE_RES 64                /* pulse table points per symbol */
#define V22_EQ_LEN    16                /* T/2 spaced taps */
#define V22_SYM_POWER 10.0f             /* mean power of both constellations */

#define MS2SYM(ms) ((ms) * V22_BAUD / 1000)

enum v22_tx {
	TX_SILENCE,
	TX_ANSTONE,
	TX_USB1,        /* unscrambled binary ones at 1200 bit/s */
	TX_S1,          /* unscrambled 00/11 dibits: V.22bis capability */
	TX_SB1_1200,    /* scrambled binary ones at 1200 bit/s */
	TX_SB1_2400,    /* scrambled binary ones at 2400 bit/s */
	TX_DATA,
};

enum v22_state {
	/* answering side */
	A_SILENCE,
	A_ANSTONE,
	A_GAP,
	A_USB1,
	A_S1,
	A_SB1_1200,
	A_SB1_2400,
	A_V22_SB1,
	/* calling side */
	C_WAIT_USB1,
	C_GAP,
	C_S1,
	C_SB1_1200,
	C_SB1_2400,
	C_V22_SB1,
	/* both */
	V22_WAIT_RX,
	V22_DATA,
};

struct v22_dp {
	struct dp dp;
	struct modem *modem;
	int srate;
	int caller;
	int bis;                /* we offer V.22bis */
	int remote_bis;         /* remote sent S1 */
	int automode;           /* fallen back from V.32bis: V.21 next */
	enum v22_state state;
	long count, total;      /* samples in state / since creation */
	long timeout, carrier_loss;
	float sps;              /* samples per symbol */

	/* transmitter */
	enum v22_tx tx_mode;
	int tx_rate;
	float tx_t;
	cplx tx_sym[2 * V22_SPAN];
	int tx_quad;
	int tx_s1_phase;
	struct v22_scrambler tx_scr;
	struct osc tx_carrier;
	struct osc ans;
	float tx_scale;
	float pulse[2 * V22_SPAN * V22_PULSE_RES + 2];

	/* receiver front end */
	struct biquad rx_bp;
	struct osc rx_lo;
	float *mf;              /* matched filter taps */
	cplx *mf_buf;
	int mf_len, mf_pos;
	struct power_meter in_pwr;
	int carrier;
	long carrier_run;       /* >0 samples with, <0 without carrier */
	float agc_pwr, agc_alpha;

	/* timing recovery */
	cplx hist[4];
	float t_next;
	float t_adj;
	int half;
	cplx mid, prev_sym;

	/* equalizer and carrier recovery */
	cplx eq[V22_EQ_LEN];
	cplx eq_buf[V22_EQ_LEN];
	float eq_mu;
	int adapt;
	int eq_train;           /* remote sends scrambled data now */
	int eq_freeze;          /* remote is changing to 16 points */
	float theta, dtheta;
	int rx_rate;            /* 1200 or 2400: slicer and data path */
	int rx_quad;

	/* pattern detectors */
	int run_usb1, run_s1, run_sb1_12, run_sb1_24;
	int last_dibit;
	int dq1, dq2, run_periodic;     /* tone like (period 1 or 2) input */
	cplx last_y;
	float rot_avg;          /* mean phase step per symbol of the input */
	long s1_seen;           /* sample time when remote S1 was detected */
	int sb1_seen;           /* remote scrambled ones at 1200 bit/s */
	struct v22_scrambler rx_scr12, rx_scr24;
};

/* quadrant change for the first dibit (Q1 Q2, Q1 first in time) */
static const int dibit2quad[4] = { 1, 0, 2, 3 };
/* and back: quadrant change -> dibit, the table is an involution */
static const int quad2dibit[4] = { 1, 0, 2, 3 };

/* first quadrant constellation indexed by Q3 Q4 */
static const cplx v22_points[4] = {
	{ 1, 1 }, { 3, 1 }, { 1, 3 }, { 3, 3 }
};

static cplx rot90(cplx c, int quad)
{
	switch (quad & 3) {
	case 1: return cplx_make(-c.im, c.re);
	case 2: return cplx_make(-c.re, -c.im);
	case 3: return cplx_make(c.im, -c.re);
	default: return c;
	}
}

static const char *v22_name(struct v22_dp *v)
{
	return v->bis ? "V22bis" : "V22";
}

static void v22_set_state(struct v22_dp *v, enum v22_state state)
{
	DSP_DBG("%s: state %d -> %d at %ld (%ld ms)\n", v22_name(v), v->state,
		state, v->total, v->total * 1000 / v->srate);
	v->state = state;
	v->count = 0;
	/* from here on the remote modem sends scrambled signals */
	if (state == A_S1 || state == C_GAP)
		v->eq_train = 1;
}

/*
 *    transmitter
 */

static cplx v22_tx_symbol(struct v22_dp *v)
{
	u8 bits[4] = { 1, 1, 1, 1 };
	int n = v->tx_rate == 2400 ? 4 : 2;
	int i, dibit, q34 = 1;

	switch (v->tx_mode) {
	case TX_SILENCE:
	case TX_ANSTONE:
		return cplx_make(0, 0);
	case TX_USB1:
		v->tx_quad = (v->tx_quad + dibit2quad[3]) & 3;
		return rot90(v22_points[1], v->tx_quad);
	case TX_S1:
		dibit = v->tx_s1_phase ? 3 : 0;
		v->tx_s1_phase ^= 1;
		v->tx_quad = (v->tx_quad + dibit2quad[dibit]) & 3;
		return rot90(v22_points[1], v->tx_quad);
	case TX_SB1_1200:
		n = 2;
		break;
	case TX_SB1_2400:
		n = 4;
		break;
	case TX_DATA:
		modem_get_bits(v->modem, 1, bits, n);
		break;
	}
	for (i = 0; i < n; i++)
		bits[i] = v22_scramble(&v->tx_scr, bits[i] & 1);
	dibit = bits[0] << 1 | bits[1];
	if (n == 4)
		q34 = bits[2] << 1 | bits[3];
	v->tx_quad = (v->tx_quad + dibit2quad[dibit]) & 3;
	return rot90(v22_points[q34], v->tx_quad);
}

static float v22_pulse(struct v22_dp *v, float t)
{
	float p = (t + V22_SPAN) * V22_PULSE_RES;
	int i;
	if (p < 0 || p >= 2 * V22_SPAN * V22_PULSE_RES)
		return 0;
	i = (int)p;
	p -= i;
	return v->pulse[i] + p * (v->pulse[i + 1] - v->pulse[i]);
}

static float v22_tx_sample(struct v22_dp *v)
{
	cplx s = { 0, 0 }, c;
	int k;

	if (v->tx_mode == TX_ANSTONE) {
		v->tx_t = 0;
		memset(v->tx_sym, 0, sizeof(v->tx_sym));
		return DSP_TX_AMPLITUDE * osc_next(&v->ans);
	}

	v->tx_t += 1.0f / v->sps;
	if (v->tx_t >= 1) {
		v->tx_t -= 1;
		memmove(v->tx_sym + 1, v->tx_sym,
			sizeof(v->tx_sym) - sizeof(v->tx_sym[0]));
		v->tx_sym[0] = v22_tx_symbol(v);
	}
	/* symbol k was sent k periods ago, delayed by V22_SPAN periods */
	for (k = 0; k < 2 * V22_SPAN; k++) {
		float g = v22_pulse(v, v->tx_t + k - V22_SPAN);
		s.re += g * v->tx_sym[k].re;
		s.im += g * v->tx_sym[k].im;
	}
	c = osc_next_cplx(&v->tx_carrier);
	return v->tx_scale * (s.re * c.re - s.im * c.im);
}

/*
 *    receiver
 */

static int v22_slice(struct v22_dp *v, cplx z, cplx *d, int *q34)
{
	int quad;
	cplx r;

	if (z.re >= 0)
		quad = z.im >= 0 ? 0 : 3;
	else
		quad = z.im >= 0 ? 1 : 2;
	if (v->rx_rate == 2400) {
		r = rot90(z, -quad);
		*q34 = (r.im > 2) << 1 | (r.re > 2);
	} else
		*q34 = 1;
	*d = rot90(v22_points[*q34], quad);
	return quad;
}

static void v22_rx_bits(struct v22_dp *v, int dquad, int q34_16)
{
	int dibit = quad2dibit[dquad];
	int b0 = dibit >> 1, b1 = dibit & 1;
	int ones12, ones24;
	u8 bits[4];

	/* raw (unscrambled) patterns */
	v->run_usb1 = dibit == 3 ? v->run_usb1 + 1 : 0;
	if ((dibit == 0 && v->last_dibit == 3) || (dibit == 3 && v->last_dibit == 0))
		v->run_s1++;
	else
		v->run_s1 = 0;
	v->last_dibit = dibit;

	/* 1200 bit/s path */
	bits[0] = v22_descramble(&v->rx_scr12, b0);
	bits[1] = v22_descramble(&v->rx_scr12, b1);
	ones12 = bits[0] & bits[1];
	v->run_sb1_12 = ones12 ? v->run_sb1_12 + 1 : 0;

	/* 2400 bit/s path */
	{
		u8 b24[4];
		b24[0] = v22_descramble(&v->rx_scr24, b0);
		b24[1] = v22_descramble(&v->rx_scr24, b1);
		b24[2] = v22_descramble(&v->rx_scr24, q34_16 >> 1);
		b24[3] = v22_descramble(&v->rx_scr24, q34_16 & 1);
		ones24 = b24[0] & b24[1] & b24[2] & b24[3];
		v->run_sb1_24 = ones24 ? v->run_sb1_24 + 1 : 0;
		DSP_TRACE("%s: b24 %d%d%d%d run %d\n", v->caller ? "C" : "A",
			  b24[0], b24[1], b24[2], b24[3], v->run_sb1_24);
		if (v->rx_rate == 2400)
			memcpy(bits, b24, 4);
	}

	if (v->state == V22_DATA && v->carrier)
		modem_put_bits(v->modem, 1, bits, v->rx_rate == 2400 ? 4 : 2);
}

static void v22_eq_reset(struct v22_dp *v)
{
	memset(v->eq, 0, sizeof(v->eq));
	v->eq[V22_EQ_LEN / 2].re = 1;
}

static void v22_rx_symbol(struct v22_dp *v, cplx y)
{
	cplx out = { 0, 0 }, z, d, e, rot;
	int i, quad, q34, q34_16;
	float perr;

	/* unscrambled ones step exactly -90 degrees per symbol; this tells
	   them from a 2225 Hz answer tone (-105 degrees) */
	d = cplx_mulc(y, v->last_y);
	v->last_y = y;
	v->rot_avg += 0.1f * (atan2f(d.im, d.re) - v->rot_avg);

	/* equalizer (the delay line was fed with both half symbols) */
	for (i = 0; i < V22_EQ_LEN; i++)
		out = cplx_add(out, cplx_mul(v->eq[i], v->eq_buf[i]));
	rot = cplx_make(cosf(v->theta), -sinf(v->theta));
	z = cplx_mul(out, rot);

	quad = v22_slice(v, z, &d, &q34);
	/* 16 point decision for the 2400 bit/s detector */
	if (v->rx_rate == 2400)
		q34_16 = q34;
	else {
		cplx r = rot90(z, -quad);
		q34_16 = (r.im > 2) << 1 | (r.re > 2);
	}

	if (v->adapt) {
		float norm = 1e-3f, mu;
		cplx dp = d;
		/* carrier PLL; while the remote changes from 4 to 16 points
		   track the phase on the 16 point grid, which holds both */
		if (v->remote_bis && v->rx_rate == 1200)
			dp = rot90(v22_points[q34_16], quad);
		perr = (z.im * dp.re - z.re * dp.im) / cplx_pow(dp);
		if (perr > 0.5f)
			perr = 0.5f;
		else if (perr < -0.5f)
			perr = -0.5f;
		/* the frequency term only learns on scrambled signals, noise
		   or tones before the handshake would only mislead it */
		if (v->eq_train && v->run_periodic < 6)
			v->dtheta += 0.0005f * perr;
		/* +-8 Hz is plenty for any real channel */
		if (v->dtheta > 0.08f)
			v->dtheta = 0.08f;
		else if (v->dtheta < -0.08f)
			v->dtheta = -0.08f;
		v->theta += 0.08f * perr;
		/* normalized LMS with the error rotated back to the eq output;
		   the unscrambled sequences are nearly pure tones and would
		   make the equalizer blow up out of band, so only train on
		   scrambled signals */
		for (i = 0; i < V22_EQ_LEN; i++)
			norm += cplx_pow(v->eq_buf[i]);
		mu = v->eq_mu / norm;
		if (v->run_periodic >= 6 || !v->eq_train || v->eq_freeze)
			mu = 0;
		e = cplx_sub(d, z);
		e = cplx_mulc(e, rot);
		for (i = 0; i < V22_EQ_LEN; i++) {
			cplx g = cplx_mulc(e, v->eq_buf[i]);
			v->eq[i].re += mu * g.re;
			v->eq[i].im += mu * g.im;
		}
	}
	v->theta += v->dtheta;
	v->theta = remainderf(v->theta, 2 * (float)M_PI);

	DSP_TRACE("%s: %ld sym %6.2f %6.2f q %d dq %d th %5.2f c %d\n",
		 v->caller ? "C" : "A", v->total, z.re, z.im, quad,
		 (quad - v->rx_quad) & 3, v->theta, v->carrier);
	i = (quad - v->rx_quad) & 3;
	if (i == v->dq2)
		v->run_periodic++;
	else {
		/* a tone like sequence ended: anything the equalizer learned
		   on it is unreliable out of band, start over from a clean
		   centre tap */
		if (v->run_periodic >= 20 && v->rx_rate == 1200)
			v22_eq_reset(v);
		v->run_periodic = 0;
	}
	v->dq2 = v->dq1;
	v->dq1 = i;
	v22_rx_bits(v, i, q34_16);
	v->rx_quad = quad;
}

static void v22_rx_reset(struct v22_dp *v)
{
	v22_eq_reset(v);
	memset(v->eq_buf, 0, sizeof(v->eq_buf));
	v->theta = v->dtheta = 0;
	v->t_adj = 0;
	v->agc_pwr = 0;
}

static void v22_rx_sample(struct v22_dp *v, float x)
{
	cplx b, y;
	int i, j;
	float g, mu;

	/* carrier detector on the band passed input */
	x = biquad_run(&v->rx_bp, x);
	power_meter_run(&v->in_pwr, x);
	if (!v->carrier && v->in_pwr.power > tone_power(180)) {
		v->carrier = 1;
		v22_rx_reset(v);
		DSP_DBG("%s: carrier on at %ld\n", v22_name(v), v->total);
	} else if (v->carrier && v->in_pwr.power < tone_power(110)) {
		v->carrier = 0;
		DSP_DBG("%s: carrier off at %ld\n", v22_name(v), v->total);
	}
	if (v->carrier)
		v->carrier_run = v->carrier_run > 0 ? v->carrier_run + 1 : 1;
	else
		v->carrier_run = v->carrier_run < 0 ? v->carrier_run - 1 : -1;

	/* down conversion and matched filter */
	b = cplx_scale(osc_next_cplx(&v->rx_lo), x);
	v->mf_buf[v->mf_pos] = b;
	y = cplx_make(0, 0);
	for (i = 0, j = v->mf_pos; i < v->mf_len; i++) {
		y.re += v->mf[i] * v->mf_buf[j].re;
		y.im += v->mf[i] * v->mf_buf[j].im;
		if (--j < 0)
			j = v->mf_len - 1;
	}
	if (++v->mf_pos >= v->mf_len)
		v->mf_pos = 0;

	/* AGC to the constellation power */
	v->agc_pwr += v->agc_alpha * (cplx_pow(y) - v->agc_pwr);
	g = v->agc_pwr > 1e-3f ?
		sqrtf(V22_SYM_POWER * (1 - V22_ALPHA / 4) / v->agc_pwr) : 0;
	y = cplx_scale(y, g);

	memmove(v->hist, v->hist + 1, 3 * sizeof(cplx));
	v->hist[3] = y;

	/* T/2 samples at t_next (in samples behind hist[2]) */
	v->t_next -= 1;
	while (v->t_next <= 0) {
		/* cubic Lagrange interpolation between hist[1] and hist[2] */
		float m = 1 + v->t_next;
		float c0 = -m * (m - 1) * (m - 2) / 6;
		float c1 = (m + 1) * (m - 1) * (m - 2) / 2;
		float c2 = -(m + 1) * m * (m - 2) / 2;
		float c3 = (m + 1) * m * (m - 1) / 6;
		cplx s;
		s.re = c0 * v->hist[0].re + c1 * v->hist[1].re +
			c2 * v->hist[2].re + c3 * v->hist[3].re;
		s.im = c0 * v->hist[0].im + c1 * v->hist[1].im +
			c2 * v->hist[2].im + c3 * v->hist[3].im;

		memmove(v->eq_buf + 1, v->eq_buf,
			(V22_EQ_LEN - 1) * sizeof(cplx));
		v->eq_buf[0] = s;
		v->t_next += v->sps / 2;

		if (v->half) {
			/* Gardner timing error detector */
			float e = (v->mid.re * (v->prev_sym.re - s.re) +
				   v->mid.im * (v->prev_sym.im - s.im)) /
				V22_SYM_POWER;
			if (e > 1) e = 1; else if (e < -1) e = -1;
			if (v->carrier) {
				v->t_adj += 0.001f * v->sps * e;
				/* +-1000 ppm at most */
				if (v->t_adj > 0.001f * v->sps)
					v->t_adj = 0.001f * v->sps;
				else if (v->t_adj < -0.001f * v->sps)
					v->t_adj = -0.001f * v->sps;
				v->t_next += 0.03f * v->sps * e + v->t_adj;
			}
			v->prev_sym = s;
			v22_rx_symbol(v, s);
		} else
			v->mid = s;
		v->half ^= 1;
	}

	/* adaptation only with a carrier; the equalizer is frozen while
	   the slicer may not match what the remote sends (1200 -> 2400) */
	mu = v->state == V22_DATA ? 0.01f : 0.05f;
	v->adapt = v->carrier && v->carrier_run > ms2samples(20, v->srate);
	v->eq_freeze = v->remote_bis && v->rx_rate == 1200 &&
		v->total - v->s1_seen > ms2samples(350, v->srate);
	v->eq_mu = mu;
}

static void v22_set_rx_rate(struct v22_dp *v, int rate)
{
	DSP_DBG("%s: rx rate %d\n", v22_name(v), rate);
	v->rx_rate = rate;
}

static void v22_set_tx(struct v22_dp *v, enum v22_tx mode)
{
	v->tx_mode = mode;
	if (mode == TX_SB1_2400 || (mode == TX_DATA && v->tx_rate == 2400))
		v->tx_rate = 2400;
	else if (mode != TX_DATA)
		v->tx_rate = 1200;
}

static int v22_connect(struct v22_dp *v)
{
	v22_set_state(v, V22_DATA);
	v22_set_tx(v, TX_DATA);
	modem_set_param(v->modem, MDMPRM_TX_RATE, v->tx_rate);
	modem_set_param(v->modem, MDMPRM_RX_RATE, v->rx_rate);
	DSP_DBG("%s: connect %d/%d\n", v22_name(v), v->tx_rate, v->rx_rate);
	return DPSTAT_CONNECT;
}

/* the handshake, run once per sample after the receiver */
static int v22_handshake(struct v22_dp *v)
{
	long ms = v->count * 1000 / v->srate;

	switch (v->state) {
	/* answering modem */
	case A_SILENCE:
		if (ms >= 1000) {
			v22_set_state(v, A_ANSTONE);
			v22_set_tx(v, TX_ANSTONE);
		}
		break;
	case A_ANSTONE:
		/* a Bell 212A caller answers the tone with scrambled ones
		   right away and expects the same back */
		if (v->run_sb1_12 >= MS2SYM(100)) {
			v22_set_state(v, A_V22_SB1);
			v22_set_tx(v, TX_SB1_1200);
		} else if (ms >= 3300) {
			v22_set_state(v, A_GAP);
			v22_set_tx(v, TX_SILENCE);
		}
		break;
	case A_GAP:
		if (ms >= 75) {
			v22_set_state(v, A_USB1);
			v22_set_tx(v, TX_USB1);
		}
		break;
	case A_USB1:
		if (v->bis && v->run_s1 >= MS2SYM(40)) {
			DSP_DBG("%s: S1 received\n", v22_name(v));
			v->remote_bis = 1;
			v->s1_seen = v->total;
			v22_set_state(v, A_S1);
			v22_set_tx(v, TX_S1);
		} else if (v->run_sb1_12 >= MS2SYM(270)) {
			v22_set_state(v, A_V22_SB1);
			v22_set_tx(v, TX_SB1_1200);
		} else if (v->automode && ms >= 3000) {
			/* no V.22 caller: a V.21 one waits for our mark
			   (V.32bis Annex A) */
			struct dp_runtime *rt = (struct dp_runtime *)
				modem_get_param(v->modem, MDMPRM_DPRUNTIME);
			DSP_DBG("%s: no answer, falling back to V.21\n",
				v22_name(v));
			if (rt)
				rt->ans_sent = 1;
			modem_set_param(v->modem, MDMPRM_DP_REQUESTED, DP_V21);
			return DPSTAT_CHANGEDP;
		}
		break;
	case A_S1:
		if (ms >= 100) {
			v22_set_state(v, A_SB1_1200);
			v22_set_tx(v, TX_SB1_1200);
		}
		break;
	case A_SB1_1200:
		if (ms >= 500) {
			v22_set_state(v, A_SB1_2400);
			v22_set_tx(v, TX_SB1_2400);
		}
		break;
	case A_SB1_2400:
		if (ms >= 200)
			v22_set_state(v, V22_WAIT_RX);
		break;
	case A_V22_SB1:
		if (ms >= 765)
			return v22_connect(v);
		break;

	/* calling modem */
	case C_WAIT_USB1:
		if (v->run_usb1 >= MS2SYM(155) &&
		    fabsf(v->rot_avg + (float)M_PI / 2) < 0.1f) {
			DSP_DBG("%s: unscrambled ones received\n", v22_name(v));
			v22_set_state(v, C_GAP);
		} else if (v->run_sb1_12 >= MS2SYM(270)) {
			/* answerer skipped the unscrambled ones (Bell 212A) */
			v->sb1_seen = 1;
			v22_set_state(v, C_V22_SB1);
			v22_set_tx(v, TX_SB1_1200);
		}
		break;
	case C_GAP:
		if (ms >= 456) {
			if (v->bis) {
				v22_set_state(v, C_S1);
				v22_set_tx(v, TX_S1);
			} else {
				v22_set_state(v, C_V22_SB1);
				v22_set_tx(v, TX_SB1_1200);
			}
		}
		break;
	case C_S1:
		if (ms >= 100) {
			v22_set_state(v, C_SB1_1200);
			v22_set_tx(v, TX_SB1_1200);
		}
		break;
	case C_SB1_1200:
		if (!v->remote_bis && v->run_s1 >= MS2SYM(40)) {
			DSP_DBG("%s: S1 received\n", v22_name(v));
			v->remote_bis = 1;
			v->s1_seen = v->total;
		}
		if (v->remote_bis) {
			if (v->total - v->s1_seen >= ms2samples(600, v->srate)) {
				v22_set_state(v, C_SB1_2400);
				v22_set_tx(v, TX_SB1_2400);
			}
		} else if (v->run_sb1_12 >= MS2SYM(270)) {
			/* V.22 answerer: stay at 1200 bit/s */
			v->sb1_seen = 1;
			v22_set_state(v, C_V22_SB1);
		}
		break;
	case C_SB1_2400:
		if (ms >= 200)
			v22_set_state(v, V22_WAIT_RX);
		break;
	case C_V22_SB1:
		/* the answerer's scrambled ones must have been seen; it may
		   already send data by the end of our 765 ms */
		if (v->run_sb1_12 >= MS2SYM(100))
			v->sb1_seen = 1;
		if (ms >= 765 && v->sb1_seen)
			return v22_connect(v);
		/* a Bell 103 answerer only keeps its 2225 Hz mark */
		if (v->dp.id == DP_B212 && !v->sb1_seen && ms >= 1500) {
			struct dp_runtime *rt = (struct dp_runtime *)
				modem_get_param(v->modem, MDMPRM_DPRUNTIME);
			DSP_DBG("%s: no 212A answer, falling back to Bell 103\n",
				v22_name(v));
			/* the answerer's mark is there already */
			if (rt)
				rt->ans_sent = 1;
			modem_set_param(v->modem, MDMPRM_DP_REQUESTED, DP_B103);
			return DPSTAT_CHANGEDP;
		}
		break;

	case V22_WAIT_RX:
		/* the switch itself needed 40 ms of scrambled ones at 2400 */
		if (v->rx_rate == 2400)
			return v22_connect(v);
		break;
	case V22_DATA:
		break;
	}

	/* scrambled ones seen: safe to train the equalizer and PLL */
	if (v->run_sb1_12 >= 10 || v->run_sb1_24 >= 10)
		v->eq_train = 1;

	/* switch the receiver to 16 points once scrambled ones at 2400
	   bit/s are seen */
	if (v->remote_bis && v->rx_rate == 1200 && v->run_sb1_24 >= MS2SYM(40))
		v22_set_rx_rate(v, 2400);
	return DPSTAT_OK;
}

static int v22_process(struct dp *dp, void *in, void *out, int cnt)
{
	struct v22_dp *v = dp->dp_data;
	s16 *src = in, *dst = out;
	int ret = DPSTAT_OK, r, i;

	for (i = 0; i < cnt; i++) {
		v22_rx_sample(v, src[i]);
		r = v22_handshake(v);
		if (r == DPSTAT_CHANGEDP) {
			/* the rest of the block goes out silent */
			memset(dst + i, 0, (cnt - i) * sizeof(*dst));
			return r;
		}
		if (r != DPSTAT_OK)
			ret = r;
		if (v->state == V22_DATA) {
			if (-v->carrier_run >= v->carrier_loss) {
				DSP_DBG("%s: carrier lost\n", v22_name(v));
				return DPSTAT_ERROR;
			}
		} else if (v->total >= v->timeout) {
			DSP_DBG("%s: handshake timeout\n", v22_name(v));
			return DPSTAT_ERROR;
		}
		dst[i] = dsp_clip16(v22_tx_sample(v));
		v->count++;
		v->total++;
	}
	return ret;
}

static struct dp *v22_create(struct modem *m, enum DP_ID id,
			     int caller, int srate, int max_frag,
			     struct dp_operations *op)
{
	struct v22_dp *v;
	struct dp_runtime *rt;
	int i, s7, s10;
	float sum;

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
	v->bis = !(id == DP_V22 || id == DP_B212);
	v->sps = (float)srate / V22_BAUD;

	for (i = 0; i <= 2 * V22_SPAN * V22_PULSE_RES + 1; i++)
		v->pulse[i] = rrc_pulse((float)i / V22_PULSE_RES - V22_SPAN, V22_ALPHA);
	/* passband rms of DSP_TX_AMPLITUDE/sqrt(2) for unit energy pulses */
	v->tx_scale = DSP_TX_AMPLITUDE / sqrtf(V22_SYM_POWER);
	osc_set_freq(&v->tx_carrier, caller ? 1200 : 2400, srate);
	osc_set_freq(&v->ans, id == DP_B212 ? 2225 : 2100, srate);
	v->tx_rate = 1200;

	biquad_bandpass(&v->rx_bp, caller ? 2400 : 1200, 1.0f, srate);
	osc_set_freq(&v->rx_lo, caller ? -2400 : -1200, srate);
	v->mf_len = (int)(2 * V22_SPAN * v->sps) | 1;
	v->mf = calloc(v->mf_len, sizeof(float));
	v->mf_buf = calloc(v->mf_len, sizeof(cplx));
	if (!v->mf || !v->mf_buf) {
		free(v->mf);
		free(v->mf_buf);
		free(v);
		return NULL;
	}
	for (i = 0, sum = 0; i < v->mf_len; i++) {
		v->mf[i] = rrc_pulse((i - v->mf_len / 2) / v->sps, V22_ALPHA);
		sum += v->mf[i] * v->mf[i];
	}
	for (i = 0; i < v->mf_len; i++)
		v->mf[i] /= sqrtf(sum);
	power_meter_init(&v->in_pwr, 0.03f, srate);
	v->agc_alpha = 1.0f / (0.05f * srate);
	v->t_next = v->sps / 2;
	v->rx_rate = 1200;
	v22_rx_reset(v);

	s7 = modem_get_sreg(m, SREG_WAIT_CARRIER_AFTER_DIAL);
	s10 = modem_get_sreg(m, SREG_CARRIER_LOSS_DISCONNECT_TIME);
	v->timeout = (long)(s7 > 0 ? s7 : 50) * srate;
	v->carrier_loss = (long)(s10 > 0 ? s10 : 14) * srate / 10;

	v->tx_mode = TX_SILENCE;
	rt = (struct dp_runtime *)modem_get_param(m, MDMPRM_DPRUNTIME);
	if (!caller && rt && rt->ans_sent) {
		/* automode fallback from V.32bis: the answer tone was sent */
		rt->ans_sent = 0;
		v->automode = 1;
		v->state = A_GAP;
	} else if (!caller)
		v->state = A_SILENCE;
	else if (id == DP_B212) {
		/* Bell 212A: answer the 2225 Hz tone with scrambled ones */
		v->state = C_WAIT_USB1;
		v22_set_state(v, C_V22_SB1);
		v22_set_tx(v, TX_SB1_1200);
	} else
		v->state = C_WAIT_USB1;
	DSP_DBG("%s: create %s (dp %d), srate %d\n", v22_name(v),
		caller ? "caller" : "answerer", id, srate);
	return &v->dp;
}

static int v22_delete(struct dp *dp)
{
	struct v22_dp *v = dp->dp_data;
	free(v->mf);
	free(v->mf_buf);
	free(v);
	return 0;
}

static int v22_hangup(struct dp *dp)
{
	return 0;
}

static struct dp_operations v22_operations = {
	.name = "V.22/V.22bis data pump",
	.create = v22_create,
	.delete = v22_delete,
	.process = v22_process,
	.hangup = v22_hangup,
};

static const enum DP_ID v22_ids[] = {
	DP_V22, DP_V22BIS, DP_B212,
};

int dp_v22_init(void)
{
	unsigned i;
	for (i = 0; i < sizeof(v22_ids) / sizeof(v22_ids[0]); i++)
		modem_dp_register(v22_ids[i], &v22_operations);
	return 0;
}

void dp_v22_exit(void)
{
	unsigned i;
	for (i = 0; i < sizeof(v22_ids) / sizeof(v22_ids[0]); i++)
		modem_dp_deregister(v22_ids[i], &v22_operations);
}
