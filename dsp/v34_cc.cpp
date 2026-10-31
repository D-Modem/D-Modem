/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    v34_cc.cpp - the control channel of half-duplex V.34 (10.2.4/V.34)
 *
 *    600 symbols/s, 1200 bit/s, both ways at once: the caller on a
 *    1200 Hz carrier, the answerer on 2400 Hz (1 dB down) with a guard
 *    tone of 1800 Hz (7 dB down).  Two bits a symbol, I1 first; the
 *    symbol is point 0 of the quarter-superconstellation, (1, 1), turned
 *    clockwise by Z quarter turns, Z = Z' + 2 I2 + I1 (mod 4): the steps
 *    between symbols carry the bits.  The bits are scrambled as the
 *    primary channel's (the caller with GPC, the answerer with GPA).
 *
 *    PPh      32 symbols, PPh(2k + I) = exp(j pi (2k(k - 1) + 1) / 4):
 *             point 0 four times, then turned by 180 degrees four times
 *    ALT      scrambled 0 1 alternations, the scrambler from 0
 *    MPh      the MP frames of half-duplex (Tables 23 and 24/V.34)
 *    E        20 scrambled ones, then data
 *    Sh       point 0, then turned 90 degrees counterclockwise, ...
 *    S-bar h  point 0 turned 180, then 270 degrees, ...
 *    AC       point 0, then turned 180 degrees, ...
 *
 *    The receiver knows PPh, Sh and AC by the steps between symbols
 *    alone, whatever its carrier's phase: 0 0 0 2 again and again
 *    (PPh), 3 1 3 1 ... (Sh) and then 3 3 (S-bar h), 2 2 2 ... (AC).
 *
 */

#include <algorithm>
#include <cinttypes>
#include <cmath>

#include "dm_types.h"
#include "bits.h"
#include "dsplibs.h"
#include "v34.h"

namespace dsp {

#define CC_SRATE        9600
#define CC_BAUD         600
#define CC_ALPHA        0.5f
#define CC_TX_SPAN      4
#define CC_RX_SPAN      6
#define CC_EQ_LEN       16
/* the nominal level, as phase 2's: a sine of this amplitude */
#define CC_AMP          (2988.0f * 1.41421f)

/* point 0 turned clockwise by z quarter turns */
static cplx cc_point(s16 z)
{
	static const signed char p[4][2] = { { 1, 1 }, { 1, -1 },
					     { -1, -1 }, { -1, 1 } };
	return cplx_make(p[z & 3][0], p[z & 3][1]);
}

/* the quarter turns of the point nearest to z */
static s16 cc_quarter(cplx z)
{
	return z.re >= 0 ? (z.im >= 0 ? 0 : 1) : (z.im < 0 ? 2 : 3);
}

/* PPh(i)'s quarter turns: 0 or 2 */
static s16 cc_pph(s32 i)
{
	s32 k = (i % 32) / 2;
	return (2 * k * (k - 1)) % 8 ? 2 : 0;
}

/* (v34.h) */
void V34CcTx::init(s16 caller)
{
	reinit(*this);
	float amp = caller ? CC_AMP : CC_AMP * 0.891251f;       /* -1 dB */
	qam.init(CC_SRATE, caller ? 1200 : 2400, CC_BAUD, CC_ALPHA,
		 CC_TX_SPAN, 1.41421f, amp);
	guard_amp = caller ? 0 : CC_AMP * 0.446684f;            /* -7 dB */
	guard.set_freq(1800, CC_SRATE);
	tap = caller ? 18 : 5;
	seg = V34CC_SILENCE;
}

/* (v34.h) */
void V34CcTx::segment(enum v34_ccseg s)
{
	DSP_DBG("v34 cc tx: segment %d -> %d after %" PRId32 " symbols\n",
		seg, s, n);
	seg = s;
	n = 0;
	if (s == V34CC_ALT)
		scr = 0;
	if (s == V34CC_MPH) {
		nbits = pos = 0;
		mp_sent = mp_to_e = 0;
	}
}

/* (v34.h): E goes on to data by itself after its 10 symbols */
cplx V34CcTx::symbol()
{
	s16 b[2] = { 1, 1 }, k;
	cplx p = { 0, 0 };

	if (seg == V34CC_MPH && pos >= nbits) {
		/* a whole MPh out: E, or the next */
		if (nbits)
			mp_sent++;
		if (mp_to_e)
			segment(V34CC_E);
		else {
			nbits = mp.build(bits);
			pos = 0;
		}
	}
	switch (seg) {
	case V34CC_SILENCE:
		n++;
		return p;
	case V34CC_PPH:
		z = cc_pph(n);
		n++;
		return cc_point(z);
	case V34CC_SH:
		/* ends on the counterclockwise turn (an even length) */
		z = n & 1 ? 3 : 0;
		n++;
		return cc_point(z);
	case V34CC_SHBAR:
		z = n & 1 ? 1 : 2;
		n++;
		return cc_point(z);
	case V34CC_AC:
		z = n & 1 ? 2 : 0;
		n++;
		return cc_point(z);
	case V34CC_ALT:
		b[0] = 0;
		break;
	case V34CC_MPH:
		b[0] = bits[pos];
		b[1] = bits[pos + 1];
		pos += 2;
		break;
	case V34CC_DATA:
		{
			unsigned char d[2];
			link.get(d, 2);
			b[0] = d[0] & 1;
			b[1] = d[1] & 1;
		}
		break;
	case V34CC_E:
	case V34CC_ONES:
		break;
	}
	for (k = 0; k < 2; k++)
		b[k] = scramble(&scr, tap, b[k]);
	z = (z + 2 * b[1] + b[0]) & 3;
	n++;
	if (seg == V34CC_E && n == 10)
		segment(V34CC_DATA);
	return cc_point(z);
}

/* (v34.h) with the guard tone */
float V34CcTx::sample()
{
	float y = qam.sample([this] { return symbol(); });
	if (guard_amp)
		y += guard_amp * guard.next();
	return y;
}


/*
 *    receiver
 */

/* (v34.h) */
s16 V34CcRx::init(s16 caller)
{
	reinit(*this);
	tap = caller ? 5 : 18;
	return qam.init(CC_SRATE, caller ? 2400 : 1200, CC_BAUD, CC_ALPHA,
			CC_RX_SPAN, CC_EQ_LEN, 2.0f);
}

/* the buffers init() took */
void V34CcRx::release()
{
	qam.release();
}

/* (v34.h) */
void V34CcRx::resync()
{
	DSP_DBG("v34 cc rx: waiting for PPh or Sh\n");
	ev_pph = ev_shbar = ev_ac = ev_mp = ev_e = 0;
	decoding = data_on = 0;
	ended = 0;
	on = 0;
	pin = 0;
	nsteps = 0;
	mp_n = 0;
	ones = 0;
	sr = 0;
}

/* a descrambled bit: MPh frames, E, then data to link */
void V34CcRx::bit(s16 b)
{
	if (data_on) {
		unsigned char c = static_cast<unsigned char>(b);
		link.put(&c, 1);
		return;
	}
	sr = (sr << 1) | static_cast<u32>(b);
	if (mp_n) {
		mpbits[mp_n++] = static_cast<unsigned char>(b);
		if (mp_n == 19)
			mp_len = b ? 188 : 88;
		if (mp_n == mp_len) {
			mp_n = 0;
			if (mp.parse(mpbits, mp_len) == 0) {
				if (!ev_mp)
					DSP_DBG("v34 cc rx: MPh type %d rate %d "
						"cc %d trellis %d mask %04x\n",
						mp.type, mp.rate_ca,
						mp.rate_ac >> 3, mp.trellis,
						mp.rate_mask);
				ev_mp = 1;
			} else
				DSP_DBG("v34 cc rx: MPh with a bad CRC\n");
		}
		return;
	}
	ones = b ? ones + 1 : 0;
	if (ones == 20) {
		DSP_DBG("v34 cc rx: E at symbol %" PRId32 "\n", sym);
		ev_e = 1;
		data_on = 1;
		return;
	}
	if ((sr & 0x3FFFF) == 0x3FFFE) {
		s16 i;
		for (i = 0; i < 17; i++)
			mpbits[i] = 1;
		mpbits[17] = 0;
		mp_n = 18;
		mp_len = 0;
	}
}

/* the steps of the last 16 symbols are PPh's (15 of them at least) */
static bool cc_is_pph(const unsigned char *st)
{
	s16 ph, k, m;
	for (ph = 0; ph < 4; ph++) {
		for (k = 0, m = 0; k < 16; k++)
			m += st[k] == ((k + ph) % 4 == 3 ? 2 : 0);
		if (m >= 15)
			return true;
	}
	return false;
}

/* a symbol in qam.z: the decision, the equalizer and carrier
   adapted on it, the step from the last; the signals in the steps, or
   their bits */
void V34CcRx::symbol()
{
	cplx z = qam.z;
	s16 qd = cc_quarter(z), st, k;
	cplx d = cc_point(qd);
	float e = cplx_pow(cplx_sub(z, d)) / 2;

	sym++;
	mse += 0.02f * (e - mse);
	/* adapted on the decisions that look right (not on a signal
	   fading out, nor on another one) */
	if (e < 0.5f)
		qam.adapt(d, 0.01f, 0.05f, 0.001f);
	st = (qd - q) & 3;
	q = qd;
	std::copy_backward(steps, steps + V34CC_HIST - 1,
			   steps + V34CC_HIST);
	steps[0] = static_cast<unsigned char>(st);
	if (nsteps < V34CC_HIST)
		nsteps++;
	if (dsplibs_debug_level >= 7)
		DSP_DBG("v34 cc rx%d: %" PRId32 " step %d mse %.3f\n", tap,
			sym, st, mse);
	if (!decoding && nsteps >= 16) {
		if (cc_is_pph(steps)) {
			DSP_DBG("v34 cc rx: PPh at symbol %" PRId32 "\n", sym);
			ev_pph = decoding = 1;
		}
		/* Sh's 3 1 3 1 ..., then 3 3 */
		for (k = 1; k < 13 && steps[k] == (k & 1 ? 3 : 1); k++)
			;
		if (k == 13 && steps[0] == 3) {
			DSP_DBG("v34 cc rx: Sh, S-bar h at symbol %" PRId32
				"\n", sym);
			ev_shbar = decoding = 1;
		}
		for (k = 0; k < 16 && steps[k] == 2; k++)
			;
		if (k == 16 && !ev_ac) {
			DSP_DBG("v34 cc rx: AC at symbol %" PRId32 "\n", sym);
			ev_ac = 1;
		}
		return;
	}
	if (decoding) {
		bit(descramble(&scr, tap, st & 1));
		bit(descramble(&scr, tap, st >> 1));
	}
}

/* a line sample at 9600 Hz */
void V34CcRx::sample(float x)
{
	/* the gain and the timing held without the signal (once it has
	   had its level), as it fades too: it comes back at its level, not
	   into a gain the silence or the primary channel has moved */
	s16 idle = hold || ended || (level > 0 && !on) ||
		   (level > 0 && pin < 0.5f * level);
	qam.timing_hold = idle;
	qam.agc_hold = idle;
	if (hold) {
		qam.dtheta = 0;
		qam.t_adj = 0;
	}
	if (!qam.sample(hold ? 0 : x) || hold || ended)
		return;
	/* the power in the band (the matched filter's, before the gain),
	   fast: on from a tenth of the level it had */
	float g2 = qam.agc_gain * qam.agc_gain;
	pin += 0.25f * ((g2 > 0 ? cplx_pow(qam.y) / g2 : 0) - pin);
	s16 on_ = pin > 2000.0f && (level == 0 || pin > 0.1f * level);
	if (on_ != on)
		DSP_DBG("v34 cc rx: signal %s at symbol %" PRId32 " (%.0f, "
			"level %.0f)\n", on_ ? "on" : "off", sym, pin, level);
	if (!on_ && on && data_on)
		ended = 1;
	on = on_;
	if (on && !ended) {
		level += (level ? 0.01f : 1.0f) * (pin - level);
		symbol();
	}
}

} // namespace dsp
