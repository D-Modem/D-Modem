/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    dp_v22.cpp - ITU V.22 / Bell 212A (1200 bit/s DPSK) and
 *               ITU V.22bis (2400 bit/s 16-QAM) data pump
 *
 *    600 baud, calling side transmits on 1200 Hz, answering side on
 *    2400 Hz.  Constellation and differential quadrant coding follow
 *    the tables of the original library (SMCv22_*): first quadrant
 *    points 00 (1,1), 01 (3,1), 10 (1,3), 11 (3,3); 1200 bit/s uses the
 *    01 point of each quadrant; dibit 00 -> +90, 01 -> 0, 10 -> +180,
 *    11 -> +270 degrees.
 *
 *    Transmitter: QamTx (root raised cosine pulses on our carrier).
 *    Receiver: band pass, down conversion, root raised cosine matched
 *    filter, AGC, cubic interpolator driven by a Gardner timing loop,
 *    T/2 spaced LMS equalizer and a decision directed carrier PLL: its
 *    own, not QamRx, for the AGC on every sample, the interpolation after
 *    a fixed matched filter and the equalizer that trains on scrambled
 *    signals only, which the interop tests with the other modems were
 *    tuned on.
 *
 */

#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

#include "dm_types.h"
#include "dsp.h"
#include "pump.h"
#include "qam.h"

namespace dsp {

#define V22_BAUD      600
#define V22_ALPHA     0.75f
#define V22_SPAN      3                 /* pulse half length, symbols */
#define V22_EQ_LEN    16                /* T/2 spaced taps */
#define V22_SYM_POWER 10.0f             /* mean power of both constellations */

#define MS2SYM(ms) (static_cast<s32>(ms) * V22_BAUD / 1000)

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

struct V22Pump : PumpBase<V22Pump, v22_state> {
	static constexpr const char *name = "V.22/V.22bis data pump";

	s16 bis;                /* we offer V.22bis */
	s16 remote_bis;         /* remote sent S1 */
	s16 automode;           /* fallen back from V.32bis: V.21 next */
	/* a Bell 103 caller takes our unscrambled ones (2250 Hz) for its
	   answerer's 2225 Hz mark and answers with its own, 1270 Hz: 100 ms
	   of it before its data (only a pure tone, a V.22 caller never
	   sends one) */
	Bell103Detect bell;
	float sps;              /* samples per symbol */

	/* transmitter */
	enum v22_tx tx_mode;
	s32 tx_rate;
	s16 tx_quad;
	s16 tx_s1_phase;
	V22Scrambler tx_scr;
	QamTx tx;               /* on our carrier (1200 Hz calling, 2400 Hz
				   answering) */
	Osc ans;

	/* receiver front end */
	Biquad rx_bp;
	Osc rx_lo;
	std::vector<float> mf;  /* matched filter taps */
	std::vector<cplx> mf_buf;
	s16 mf_len, mf_pos;
	PowerMeter in_pwr;
	CarrierDetect carrier;
	float agc_pwr, agc_alpha;

	/* timing recovery */
	cplx hist[4];
	float t_next;
	float t_adj;
	s16 half;
	cplx mid, prev_sym;

	/* equalizer and carrier recovery */
	cplx eq[V22_EQ_LEN];
	cplx eq_buf[V22_EQ_LEN];
	float eq_mu;
	s16 adapt;
	s16 eq_train;           /* remote sends scrambled data now */
	s16 eq_freeze;          /* remote is changing to 16 points */
	float theta, dtheta;
	s32 rx_rate;            /* 1200 or 2400: slicer and data path */
	s16 rx_quad;

	/* pattern detectors */
	s32 run_usb1, run_s1, run_sb1_12, run_sb1_24;
	s16 last_dibit;
	s16 dq1, dq2;
	s32 run_periodic;       /* tone like (period 1 or 2) input */
	cplx last_y;
	float rot_avg;          /* mean phase step per symbol of the input */
	s32 s1_seen;            /* sample time when remote S1 was detected */
	s16 sb1_seen;           /* remote scrambled ones at 1200 bit/s */
	s32 sb1_at;             /* when 270 ms of them were in */
	V22Scrambler rx_scr12, rx_scr24;

	const char *mode_name() const;
	/* its name in the log (the mode's) */
	const char *tag() const { return mode_name(); }
	/* in data mode (the carrier watched, no handshake timeout) */
	bool in_data() const { return state == V22_DATA; }
	void set_state(enum v22_state state_);
	cplx tx_symbol();
	float tx_sample();
	s16 slice(cplx z, cplx *d, s16 *q34);
	void rx_bits(s16 dquad, s16 q34_16, s16 dq_raw);
	void eq_reset();
	void rx_symbol(cplx y);
	void rx_reset();
	void rx_sample(float x);
	void set_rx_rate(s32 rate);
	void set_tx(enum v22_tx mode);
	s16 connect();
	s16 handshake();
	s16 start(struct modem *m, enum DP_ID id, s16 caller_, s32 srate_,
		  s32 max_frag);
	s16 sample(float x, float *y);
	/* cnt samples in and out, through sample() */
	s16 process(void *in, void *out, s32 cnt) override
	{
		return pump_loop(in, out, cnt);
	}
};

/* quadrant change for the first dibit (Q1 Q2, Q1 first in time) */
static const s16 dibit2quad[4] = { 1, 0, 2, 3 };
/* and back: quadrant change -> dibit, the table is an involution */
static const s16 quad2dibit[4] = { 1, 0, 2, 3 };

/* first quadrant constellation indexed by Q3 Q4 */
static const cplx v22_points[4] = {
	{ 1, 1 }, { 3, 1 }, { 1, 3 }, { 3, 3 }
};

/* c turned by quad * 90 degrees counter clockwise */
static cplx rot90(cplx c, s16 quad)
{
	switch (quad & 3) {
	case 1: return cplx_make(-c.im, c.re);
	case 2: return cplx_make(-c.re, -c.im);
	case 3: return cplx_make(c.im, -c.re);
	default: return c;
	}
}

/* the name for the debug output */
const char *V22Pump::mode_name() const
{
	return bis ? "V22bis" : "V22";
}

/* to state_, count from 0; from A_S1 / C_GAP on the remote's signal is
   scrambled and the equalizer may train */
void V22Pump::set_state(enum v22_state state_)
{
	PumpBase::set_state(state_);
	/* from here on the remote modem sends scrambled signals */
	if (state_ == A_S1 || state_ == C_GAP)
		eq_train = 1;
}

/*
 *    transmitter
 */

/* the next symbol of the current tx_mode: 0 in silence or during the
   answer tone, else the quadrant advanced by the (scrambled) dibit and
   the point of Q3Q4 (01 at 1200 bit/s); data bits from the link layer */
cplx V22Pump::tx_symbol()
{
	u8 bits[4] = { 1, 1, 1, 1 };
	s16 n = tx_rate == 2400 ? 4 : 2;
	s16 i, dibit, q34 = 1;

	switch (tx_mode) {
	case TX_SILENCE:
	case TX_ANSTONE:
		return cplx_make(0, 0);
	case TX_USB1:
		tx_quad = (tx_quad + dibit2quad[3]) & 3;
		return rot90(v22_points[1], tx_quad);
	case TX_S1:
		dibit = tx_s1_phase ? 3 : 0;
		tx_s1_phase ^= 1;
		tx_quad = (tx_quad + dibit2quad[dibit]) & 3;
		return rot90(v22_points[1], tx_quad);
	case TX_SB1_1200:
		n = 2;
		break;
	case TX_SB1_2400:
		n = 4;
		break;
	case TX_DATA:
		modem_get_bits(modem, 1, bits, n);
		break;
	}
	for (i = 0; i < n; i++)
		bits[i] = tx_scr.scramble(bits[i] & 1);
	dibit = bits[0] << 1 | bits[1];
	if (n == 4)
		q34 = bits[2] << 1 | bits[3];
	tx_quad = (tx_quad + dibit2quad[dibit]) & 3;
	return rot90(v22_points[q34], tx_quad);
}

/* the next line sample: the answer tone, or the pulse shaped symbols
   on our carrier (1200 Hz calling, 2400 Hz answering) */
float V22Pump::tx_sample()
{
	if (tx_mode == TX_ANSTONE) {
		/* the symbols start afresh after it */
		tx.t = 0;
		tx.flush();
		return DSP_TX_AMPLITUDE * ans.next();
	}
	return tx.sample([this] { return tx_symbol(); });
}

/*
 *    receiver
 */

/* the decision for z: returns its quadrant, the point in *d and its
   Q3Q4 in *q34 (always 01 at 1200 bit/s) */
s16 V22Pump::slice(cplx z, cplx *d, s16 *q34)
{
	s16 quad;
	cplx r;

	/* 1200 bit/s has one point a quadrant, 01 (3 + j), 18 degrees off
	   the axes: decide around it, 45 degrees each way, not on the axes,
	   or a phase error of 18 degrees (a speech codec's) turns it into
	   its neighbour (z turned by 26.6 degrees, 2 + j, puts it on the
	   diagonal).  Only with a remote known to be V.22: as V.22bis
	   ourselves, before data a remote may yet turn out V.22bis (S1)
	   and change to all 16 points while we are at 1200; the axes then */
	r = z;
	if (rx_rate == 1200 && !remote_bis && (!bis || state == V22_DATA))
		r = cplx_mul(z, cplx_make(2, 1));
	if (r.re >= 0)
		quad = r.im >= 0 ? 0 : 3;
	else
		quad = r.im >= 0 ? 1 : 2;
	if (rx_rate == 2400) {
		r = rot90(z, -quad);
		*q34 = (r.im > 2) << 1 | (r.re > 2);
	} else
		*q34 = 1;
	*d = rot90(v22_points[*q34], quad);
	return quad;
}

/* dquad: the quadrant change of the decisions; dq_raw: the phase change
   from the last symbol, whatever the carrier phase (for the unscrambled
   patterns) */
void V22Pump::rx_bits(s16 dquad, s16 q34_16, s16 dq_raw)
{
	s16 dibit = quad2dibit[dquad], raw = quad2dibit[dq_raw];
	s16 b0 = dibit >> 1, b1 = dibit & 1;
	s16 ones12, ones24;
	u8 bits[4];

	/* raw (unscrambled) patterns, from the phase changes themselves: S1
	   is two points 90 degrees apart, which the decisions put in the same
	   quadrant while the carrier phase is near 45 degrees off, and there
	   its error averages out (the receiver of the answerer, which has
	   just the caller's 100 ms of S1 to find it in, took a V.22bis caller
	   for V.22) */
	run_usb1 = raw == 3 ? run_usb1 + 1 : 0;
	if ((raw == 0 && last_dibit == 3) || (raw == 3 && last_dibit == 0))
		run_s1++;
	else
		run_s1 = 0;
	last_dibit = raw;

	/* 1200 bit/s path */
	bits[0] = rx_scr12.descramble(b0);
	bits[1] = rx_scr12.descramble(b1);
	ones12 = bits[0] & bits[1];
	run_sb1_12 = ones12 ? run_sb1_12 + 1 : 0;

	/* 2400 bit/s path */
	{
		u8 b24[4];
		b24[0] = rx_scr24.descramble(b0);
		b24[1] = rx_scr24.descramble(b1);
		b24[2] = rx_scr24.descramble(q34_16 >> 1);
		b24[3] = rx_scr24.descramble(q34_16 & 1);
		ones24 = b24[0] & b24[1] & b24[2] & b24[3];
		run_sb1_24 = ones24 ? run_sb1_24 + 1 : 0;
		DSP_TRACE("%s: b24 %d%d%d%d run %d\n", caller ? "C" : "A",
			  b24[0], b24[1], b24[2], b24[3], run_sb1_24);
		if (rx_rate == 2400)
			memcpy(bits, b24, 4);
	}

	if (state == V22_DATA && carrier.on)
		modem_put_bits(modem, 1, bits, rx_rate == 2400 ? 4 : 2);
}

/* the equalizer back to a single centre tap */
void V22Pump::eq_reset()
{
	zero(eq);
	eq[V22_EQ_LEN / 2].re = 1;
}

/* one symbol sample y (the T/2 delay line already has it): the raw
   phase step for the pattern detectors, equalizer, slicer, carrier PLL
   and LMS (only on scrambled signals), then the bits (rx_bits) */
void V22Pump::rx_symbol(cplx y)
{
	cplx out = { 0, 0 }, z, d, e, rot;
	s16 i, quad, q34, q34_16, dq_raw;
	float perr, step;

	/* unscrambled ones step exactly -90 degrees per symbol; this tells
	   them from a 2225 Hz answer tone (-105 degrees) */
	d = cplx_mulc(y, last_y);
	last_y = y;
	step = atan2f(d.im, d.re);
	rot_avg += 0.1f * (step - rot_avg);
	dq_raw = static_cast<s16>(
			 lrintf(step / (static_cast<float>(M_PI) / 2))) &
		 3;

	/* equalizer (the delay line was fed with both half symbols) */
	for (i = 0; i < V22_EQ_LEN; i++)
		out = cplx_add(out, cplx_mul(eq[i], eq_buf[i]));
	rot = cplx_make(cosf(theta), -sinf(theta));
	z = cplx_mul(out, rot);

	quad = slice(z, &d, &q34);
	/* 16 point decision for the 2400 bit/s detector */
	if (rx_rate == 2400)
		q34_16 = q34;
	else {
		cplx r = rot90(z, -quad);
		q34_16 = (r.im > 2) << 1 | (r.re > 2);
	}

	if (adapt) {
		float norm = 1e-3f, mu;
		cplx pt = d;
		/* carrier PLL; while the remote changes from 4 to 16 points
		   track the phase on the 16 point grid, which holds both */
		if (remote_bis && rx_rate == 1200)
			pt = rot90(v22_points[q34_16], quad);
		perr = (z.im * pt.re - z.re * pt.im) / cplx_pow(pt);
		if (perr > 0.5f)
			perr = 0.5f;
		else if (perr < -0.5f)
			perr = -0.5f;
		/* the frequency term only learns on scrambled signals, noise
		   or tones before the handshake would only mislead it */
		if (eq_train && run_periodic < 6)
			dtheta += 0.0005f * perr;
		/* +-8 Hz is plenty for any real channel */
		if (dtheta > 0.08f)
			dtheta = 0.08f;
		else if (dtheta < -0.08f)
			dtheta = -0.08f;
		theta += 0.08f * perr;
		/* normalized LMS with the error rotated back to the eq output;
		   the unscrambled sequences are nearly pure tones and would
		   make the equalizer blow up out of band, so only train on
		   scrambled signals */
		for (i = 0; i < V22_EQ_LEN; i++)
			norm += cplx_pow(eq_buf[i]);
		mu = eq_mu / norm;
		if (run_periodic >= 6 || !eq_train || eq_freeze)
			mu = 0;
		e = cplx_sub(d, z);
		e = cplx_mulc(e, rot);
		for (i = 0; i < V22_EQ_LEN; i++) {
			cplx g = cplx_mulc(e, eq_buf[i]);
			eq[i].re += mu * g.re;
			eq[i].im += mu * g.im;
		}
	}
	theta += dtheta;
	theta = remainderf(theta, 2 * static_cast<float>(M_PI));

	DSP_TRACE("%s: %" PRId32 " sym %6.2f %6.2f q %d dq %d th %5.2f c %d\n",
		 caller ? "C" : "A", total, z.re, z.im, quad,
		 (quad - rx_quad) & 3, theta, carrier.on);
	i = (quad - rx_quad) & 3;
	if (i == dq2)
		run_periodic++;
	else {
		/* a tone like sequence ended: anything the equalizer learned
		   on it is unreliable out of band, start over from a clean
		   centre tap */
		if (run_periodic >= 20 && rx_rate == 1200)
			eq_reset();
		run_periodic = 0;
	}
	dq2 = dq1;
	dq1 = i;
	rx_bits(i, q34_16, dq_raw);
	rx_quad = quad;
}

/* the receiver from scratch for a new carrier: equalizer, its delay
   line, carrier phase and frequency, timing integrator, AGC */
void V22Pump::rx_reset()
{
	eq_reset();
	zero(eq_buf);
	theta = dtheta = 0;
	t_adj = 0;
	agc_pwr = 0;
}

/* one line sample: carrier detector, down conversion, matched filter,
   AGC, cubic interpolation at the T/2 instants with the Gardner timing
   loop, and rx_symbol() at every symbol */
void V22Pump::rx_sample(float x)
{
	cplx b, y;
	s16 i, j;
	float g, mu;

	/* carrier detector on the band passed input */
	x = rx_bp.run(x);
	in_pwr.run(x);
	switch (carrier.run(in_pwr.power)) {
	case 1:
		rx_reset();
		DSP_DBG("%s: carrier on at %" PRId32 "\n", mode_name(), total);
		break;
	case -1:
		DSP_DBG("%s: carrier off at %" PRId32 "\n", mode_name(), total);
		break;
	}
	carrier_tick(carrier.on);

	/* down conversion and matched filter */
	b = cplx_scale(rx_lo.next_cplx(), x);
	mf_buf[mf_pos] = b;
	y = cplx_make(0, 0);
	for (i = 0, j = mf_pos; i < mf_len; i++) {
		y.re += mf[i] * mf_buf[j].re;
		y.im += mf[i] * mf_buf[j].im;
		if (--j < 0)
			j = mf_len - 1;
	}
	if (++mf_pos >= mf_len)
		mf_pos = 0;

	/* AGC to the constellation power */
	agc_pwr += agc_alpha * (cplx_pow(y) - agc_pwr);
	g = agc_pwr > 1e-3f ?
		sqrtf(V22_SYM_POWER * (1 - V22_ALPHA / 4) / agc_pwr) : 0;
	y = cplx_scale(y, g);

	std::copy(hist + 1, hist + 4, hist);
	hist[3] = y;

	/* T/2 samples at t_next (in samples behind hist[2]) */
	t_next -= 1;
	while (t_next <= 0) {
		/* cubic Lagrange interpolation between hist[1] and hist[2] */
		float m = 1 + t_next;
		float c0 = -m * (m - 1) * (m - 2) / 6;
		float c1 = (m + 1) * (m - 1) * (m - 2) / 2;
		float c2 = -(m + 1) * m * (m - 2) / 2;
		float c3 = (m + 1) * m * (m - 1) / 6;
		cplx s;
		s.re = c0 * hist[0].re + c1 * hist[1].re +
			c2 * hist[2].re + c3 * hist[3].re;
		s.im = c0 * hist[0].im + c1 * hist[1].im +
			c2 * hist[2].im + c3 * hist[3].im;

		std::copy_backward(eq_buf, eq_buf + V22_EQ_LEN - 1,
				   eq_buf + V22_EQ_LEN);
		eq_buf[0] = s;
		t_next += sps / 2;

		if (half) {
			/* Gardner timing error detector */
			float e = (mid.re * (prev_sym.re - s.re) +
				   mid.im * (prev_sym.im - s.im)) /
				V22_SYM_POWER;
			if (e > 1) e = 1; else if (e < -1) e = -1;
			if (carrier.on) {
				t_adj += 0.001f * sps * e;
				/* +-1000 ppm at most */
				if (t_adj > 0.001f * sps)
					t_adj = 0.001f * sps;
				else if (t_adj < -0.001f * sps)
					t_adj = -0.001f * sps;
				t_next += 0.03f * sps * e + t_adj;
			}
			prev_sym = s;
			rx_symbol(s);
		} else
			mid = s;
		half ^= 1;
	}

	/* adaptation only with a carrier; the equalizer is frozen while
	   the slicer may not match what the remote sends (1200 -> 2400) */
	mu = state == V22_DATA ? 0.01f : 0.05f;
	adapt = carrier.on && carrier_run > ms2samples(20, srate);
	eq_freeze = remote_bis && rx_rate == 1200 &&
		total - s1_seen > ms2samples(350, srate);
	eq_mu = mu;
}

/* the receiver's slicer and bit path to rate (1200 or 2400) */
void V22Pump::set_rx_rate(s32 rate)
{
	DSP_DBG("%s: rx rate %" PRId32 "\n", mode_name(), rate);
	rx_rate = rate;
}

/* transmit mode; the rate follows it (data keeps the rate trained) */
void V22Pump::set_tx(enum v22_tx mode)
{
	tx_mode = mode;
	if (mode == TX_SB1_2400 || (mode == TX_DATA && tx_rate == 2400))
		tx_rate = 2400;
	else if (mode != TX_DATA)
		tx_rate = 1200;
}

/* to data: rates to the core, DPSTAT_CONNECT */
s16 V22Pump::connect()
{
	set_state(V22_DATA);
	set_tx(TX_DATA);
	return connect_at(tx_rate, rx_rate);
}

/* the handshake, run once per sample after the receiver: the answerer's
   and the caller's start-up sequences (6.3.1/V.22 and V.22bis), the
   Bell 212A variants and the automode fallbacks.  Returns DPSTAT_OK,
   DPSTAT_CONNECT or DPSTAT_CHANGEDP. */
s16 V22Pump::handshake()
{
	s32 ms = static_cast<s32>(s64{count} * 1000 / srate);

	switch (state) {
	/* answering modem */
	case A_SILENCE:
		if (ms >= ANS_SILENCE_MS) {
			set_state(A_ANSTONE);
			set_tx(TX_ANSTONE);
		}
		break;
	case A_ANSTONE:
		/* a Bell 212A caller answers the tone with scrambled ones
		   right away and expects the same back */
		if (run_sb1_12 >= MS2SYM(100)) {
			set_state(A_V22_SB1);
			set_tx(TX_SB1_1200);
		} else if (ms >= ANS_TONE_MS) {
			set_state(A_GAP);
			set_tx(TX_SILENCE);
		}
		break;
	case A_GAP:
		if (ms >= ANS_GAP_MS) {
			set_state(A_USB1);
			set_tx(TX_USB1);
		}
		break;
	case A_USB1:
		if (bis && run_s1 >= MS2SYM(40)) {
			DSP_DBG("%s: S1 received\n", mode_name());
			remote_bis = 1;
			s1_seen = total;
			set_state(A_S1);
			set_tx(TX_S1);
		} else if (run_sb1_12 >= MS2SYM(270)) {
			set_state(A_V22_SB1);
			set_tx(TX_SB1_1200);
		} else if (automode && bell.found()) {
			DSP_DBG("%s: Bell 103 caller, falling back to it\n",
				mode_name());
			return hand_over(DP_B103, 1);
		} else if (automode && ms >= 3000) {
			/* no V.22 caller: a V.21 one waits for our mark
			   (V.32bis Annex A) */
			DSP_DBG("%s: no answer, falling back to V.21\n",
				mode_name());
			return hand_over(DP_V21, 1);
		}
		break;
	case A_S1:
		if (ms >= 100) {
			set_state(A_SB1_1200);
			set_tx(TX_SB1_1200);
		}
		break;
	case A_SB1_1200:
		if (ms >= 500) {
			set_state(A_SB1_2400);
			set_tx(TX_SB1_2400);
		}
		break;
	case A_SB1_2400:
		if (ms >= 200)
			set_state(V22_WAIT_RX);
		break;
	case A_V22_SB1:
		if (ms >= 765)
			return connect();
		break;

	/* calling modem */
	case C_WAIT_USB1:
		if (run_usb1 >= MS2SYM(155) &&
		    fabsf(rot_avg + static_cast<float>(M_PI) / 2) < 0.1f) {
			DSP_DBG("%s: unscrambled ones received\n", mode_name());
			set_state(C_GAP);
		} else if (run_sb1_12 >= MS2SYM(270)) {
			/* answerer skipped the unscrambled ones (Bell 212A) */
			sb1_seen = 1;
			sb1_at = total;
			set_state(C_V22_SB1);
			set_tx(TX_SB1_1200);
		}
		break;
	case C_GAP:
		if (ms >= 456) {
			if (bis) {
				set_state(C_S1);
				set_tx(TX_S1);
			} else {
				set_state(C_V22_SB1);
				set_tx(TX_SB1_1200);
			}
		}
		break;
	case C_S1:
		if (ms >= 100) {
			set_state(C_SB1_1200);
			set_tx(TX_SB1_1200);
		}
		break;
	case C_SB1_1200:
		if (!remote_bis && run_s1 >= MS2SYM(40)) {
			DSP_DBG("%s: S1 received\n", mode_name());
			remote_bis = 1;
			s1_seen = total;
		}
		if (remote_bis) {
			if (total - s1_seen >= ms2samples(600, srate)) {
				set_state(C_SB1_2400);
				set_tx(TX_SB1_2400);
			}
		} else if (run_sb1_12 >= MS2SYM(270)) {
			/* V.22 answerer: stay at 1200 bit/s */
			sb1_seen = 1;
			sb1_at = total;
			set_state(C_V22_SB1);
		}
		break;
	case C_SB1_2400:
		if (ms >= 200)
			set_state(V22_WAIT_RX);
		break;
	case C_V22_SB1:
		/* 6.3.1.2/V.22: data 765 ms after 270 ms of the answerer's
		   scrambled ones (an answerer that waits for ours to time its
		   own drops the call when data come sooner) */
		if (!sb1_seen && run_sb1_12 >= MS2SYM(270)) {
			sb1_seen = 1;
			sb1_at = total;
		}
		if (sb1_seen && total - sb1_at >= ms2samples(765, srate))
			return connect();
		/* a Bell 103 answerer only keeps its 2225 Hz mark */
		if (dp.id == DP_B212 && !sb1_seen && ms >= 1500) {
			DSP_DBG("%s: no 212A answer, falling back to Bell 103\n",
				mode_name());
			/* the answerer's mark is there already */
			return hand_over(DP_B103, 1);
		}
		break;

	case V22_WAIT_RX:
		/* the switch itself needed 40 ms of scrambled ones at 2400 */
		if (rx_rate == 2400)
			return connect();
		break;
	case V22_DATA:
		break;
	}

	/* scrambled ones seen: safe to train the equalizer and PLL */
	if (run_sb1_12 >= 10 || run_sb1_24 >= 10)
		eq_train = 1;

	/* switch the receiver to 16 points once scrambled ones at 2400
	   bit/s are seen */
	if (remote_bis && rx_rate == 1200 && run_sb1_24 >= MS2SYM(40))
		set_rx_rate(2400);
	return DPSTAT_OK;
}

/* a sample x in, *y out: receiver, Bell 103 detector (automode),
   handshake and transmitter.  The handshake's status (the time limit,
   S7, and the loss of carrier in data, S10, are pump_loop()'s) */
s16 V22Pump::sample(float x, float *y)
{
	s16 r;

	rx_sample(x);
	if (automode)
		bell.run(x);
	r = handshake();
	if (r == DPSTAT_CHANGEDP)
		return r;       /* the rest of the block silent */
	*y = tx_sample();
	return r;
}

/* V.22 / Bell 212A (id DP_V22, DP_B212) or V.22bis, calling or
   answering: pulse and matched filter tables, timeouts from S7 and S10,
   and the first state (an automode answerer whose answer tone is sent
   starts at the gap; a Bell 212A caller after a 2225 Hz tone at once
   with scrambled ones); 0 */
s16 V22Pump::start(struct modem *m, enum DP_ID id, s16 caller_,
		   s32 srate_, s32 max_frag)
{
	s16 i;
	float sum;

	pump_setup(m, srate_, caller_);
	bis = !(id == DP_V22 || id == DP_B212);
	sps = static_cast<float>(srate_) / V22_BAUD;

	/* a pass band rms of DSP_TX_AMPLITUDE / sqrt(2) */
	tx.init(srate_, caller_ ? 1200 : 2400, V22_BAUD, V22_ALPHA, V22_SPAN,
		sqrtf(V22_SYM_POWER), DSP_TX_AMPLITUDE);
	ans.set_freq(id == DP_B212 ? 2225 : 2100, srate_);
	tx_rate = 1200;

	rx_bp.bandpass(caller_ ? 2400 : 1200, 1.0f, srate_);
	rx_lo.set_freq(caller_ ? -2400 : -1200, srate_);
	mf_len = static_cast<s16>(2 * V22_SPAN * sps) | 1;
	mf.assign(mf_len, 0);
	mf_buf.assign(mf_len, cplx());
	for (i = 0, sum = 0; i < mf_len; i++) {
		mf[i] = rrc_pulse((i - mf_len / 2) / sps, V22_ALPHA);
		sum += mf[i] * mf[i];
	}
	for (i = 0; i < mf_len; i++)
		mf[i] /= sqrtf(sum);
	in_pwr.init(0.03f, srate_);
	carrier.init(180, 110);
	agc_alpha = 1.0f / (0.05f * srate_);
	t_next = sps / 2;
	rx_rate = 1200;
	rx_reset();

	tx_mode = TX_SILENCE;
	if (!caller_ && rt && rt->ans_sent) {
		/* automode fallback from V.32bis: the answer tone was sent
		   (after V.8, agreed: no automode) */
		rt->ans_sent = 0;
		automode = !rt->after_v8;
		rt->after_v8 = 0;
		bell.init(srate_);
		state = A_GAP;
	} else if (!caller_)
		state = A_SILENCE;
	else if (id == DP_B212 && !(rt && rt->answer_tone == ANSTONE_2100)) {
		/* Bell 212A: answer the 2225 Hz tone with scrambled ones (an
		   ITU answer tone: the V.22 handshake, the same modulation) */
		state = C_WAIT_USB1;
		set_state(C_V22_SB1);
		set_tx(TX_SB1_1200);
	} else
		state = C_WAIT_USB1;
	DSP_DBG("%s: create %s (dp %d), srate %" PRId32 "\n", mode_name(),
		caller_ ? "caller" : "answerer", id, srate_);
	return 0;
}

static const enum DP_ID v22_ids[] = {
	DP_V22, DP_V22BIS, DP_B212,
};

/* registers the pump as DP_V22, DP_V22BIS and DP_B212; 0 */
s16 dp_v22_init(void)
{
	u16 i;
	for (i = 0; i < std::size(v22_ids); i++)
		modem_dp_register(v22_ids[i], &PumpOps<V22Pump>::ops);
	return 0;
}

/* deregisters them */
void dp_v22_exit(void)
{
	u16 i;
	for (i = 0; i < std::size(v22_ids); i++)
		modem_dp_deregister(v22_ids[i], &PumpOps<V22Pump>::ops);
}

} // namespace dsp
