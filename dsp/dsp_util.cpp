/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    dsp_util.cpp - small DSP building blocks for the open data pumps
 *
 */

#include <math.h>

#include <algorithm>
#include <mutex>
#include <string.h>

#include "dsp.h"

namespace dsp {


/*
 *    oscillator
 */

/* freq Hz at srate samples/s: only the increment changes, so the phase
   is continuous */
void Osc::set_freq(float freq, int srate)
{
	inc = (double)freq / srate;
}

/* sin(2 pi phase), then one sample on */
float Osc::next()
{
	float v = (float)sin(2 * M_PI * phase);
	phase += inc;
	phase -= floor(phase);
	return v;
}

/* exp(j 2 pi phase), then one sample on */
cplx Osc::next_cplx()
{
	double a = 2 * M_PI * phase;
	cplx v = cplx_make((float)cos(a), (float)sin(a));
	phase += inc;
	phase -= floor(phase);
	return v;
}


/*
 *    biquads (RBJ audio EQ cookbook forms)
 */

/* stores the coefficients divided by a0 and clears the state */
void Biquad::set(double nb0, double nb1, double nb2, double a0, double na1,
		 double na2)
{
	b0 = nb0 / a0;
	b1 = nb1 / a0;
	b2 = nb2 / a0;
	a1 = na1 / a0;
	a2 = na2 / a0;
	z1 = z2 = 0;
}

/* band pass, 0 dB peak gain at freq Hz, bandwidth freq / q */
void Biquad::bandpass(float freq, float q, int srate)
{
	double w = 2 * M_PI * freq / srate;
	double alpha = sin(w) / (2 * q);
	set(alpha, 0, -alpha, 1 + alpha, -2 * cos(w), 1 - alpha);
}

/* low pass with its corner at freq Hz (q 0.707: Butterworth) */
void Biquad::lowpass(float freq, float q, int srate)
{
	double w = 2 * M_PI * freq / srate;
	double alpha = sin(w) / (2 * q);
	double c = cos(w);
	set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

/* high pass with its corner at freq Hz (q 0.707: Butterworth) */
void Biquad::highpass(float freq, float q, int srate)
{
	double w = 2 * M_PI * freq / srate;
	double alpha = sin(w) / (2 * q);
	double c = cos(w);
	set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}


/*
 *    Goertzel
 */

/* a detector for freq Hz over blocks of block samples, state cleared */
void Goertzel::init(float freq, int block, int srate)
{
	*this = Goertzel();
	coef = 2 * cos(2 * M_PI * freq / srate);
	len = block;
}

/* one sample in; at the end of a block power gets the tone's mean
   square (A^2 / 2 for a sine of amplitude A at freq), the filter starts
   over and 1 is returned, else 0 */
int Goertzel::run(float x)
{
	float s = x + coef * s1 - s2;
	s2 = s1;
	s1 = s;
	if (++n < len)
		return 0;
	/* |X(k)|^2 scaled so that a sine of amplitude A gives A^2/2 */
	power = (s1 * s1 + s2 * s2 - coef * s1 * s2) * 2 / ((float)len * len);
	s1 = s2 = 0;
	n = 0;
	return 1;
}


/* single pole smoothing with a time constant of time_const_sec at
   srate (one sample at the least), power back to 0 */
void PowerMeter::init(float time_const_sec, int srate)
{
	alpha = 1.0f / (time_const_sec * srate);
	if (alpha > 1)
		alpha = 1;
	power = 0;
}


/*
 *    V.22 scrambler: polynomial 1 + x^-14 + x^-17.  Like the
 *    recommendation requires, 64 consecutive ones on the line invert
 *    the next bit on both sides.
 */

/* see dsp.h */
int V22Scrambler::scramble(int bit)
{
	int out = (bit ^ (reg >> 13) ^ (reg >> 16)) & 1;
	if (ones >= 64) {
		out ^= 1;
		ones = 0;
	}
	ones = out ? ones + 1 : 0;
	reg = (reg << 1) | out;
	return out;
}

/* the inverse of scramble(): the line bit goes into the register, and
   after 64 ones on the line the next output bit is inverted back */
int V22Scrambler::descramble(int bit)
{
	int out = (bit ^ (reg >> 13) ^ (reg >> 16)) & 1;
	if (ones >= 64) {
		out ^= 1;
		ones = 0;
	}
	ones = bit ? ones + 1 : 0;
	reg = (reg << 1) | (bit & 1);
	return out;
}


/*
 *    root raised cosine pulse with unit energy per symbol
 */

/* t in symbol periods, alpha the roll off; the closed form, with its
   limits at t = 0 and |t| = 1 / (4 alpha) */
float rrc_pulse(float t, float alpha)
{
	double x;
	if (fabs(t) < 1e-6)
		return 1 - alpha + 4 * alpha / M_PI;
	if (alpha > 0 && fabs(fabs(4 * alpha * t) - 1) < 1e-6) {
		x = M_PI / (4 * alpha);
		return alpha / sqrt(2) *
			((1 + 2 / M_PI) * sin(x) + (1 - 2 / M_PI) * cos(x));
	}
	x = M_PI * t;
	return (sin(x * (1 - alpha)) + 4 * alpha * t * cos(x * (1 + alpha))) /
		(x * (1 - 16 * alpha * alpha * t * t));
}


/*
 *    DTMF generator
 */

static const char dtmf_keys[] = "123A456B789C*0#D";
static const float dtmf_lo[] = { 697, 770, 852, 941 };
static const float dtmf_hi[] = { 1209, 1336, 1477, 1633 };

/* the tone pair of digit (0-9, *, #, A-D, a-d) at srate, from phase 0;
   0, or -1 (state unchanged) when it is no DTMF digit */
int DtmfGen::set(char digit, int srate)
{
	const char *p;
	int i;
	if (digit >= 'a' && digit <= 'd')
		digit -= 'a' - 'A';
	p = strchr(dtmf_keys, digit);
	if (!p || !digit)
		return -1;
	i = p - dtmf_keys;
	*this = DtmfGen();
	lo.set_freq(dtmf_lo[i / 4], srate);
	hi.set_freq(dtmf_hi[i % 4], srate);
	/* high group 2 dB above low group (twist), about -8 dBFS total */
	amp_lo = 6000;
	amp_hi = 7550;
	return 0;
}

/* the next sample of the low and high group tones together */
float DtmfGen::next()
{
	return amp_lo * lo.next() + amp_hi * hi.next();
}

/*
 *    DTMF detector
 */

/* see dsp.h: blocks of 205 samples at 8000 Hz (the bins 39 Hz apart,
   half the spacing of the tones), as many ms at srate */
void DtmfDetect::init(int srate)
{
	*this = DtmfDetect();
	n_block = (205 * srate + 4000) / 8000;
	for (int k = 0; k < 4; k++) {
		coef[k] = 2 * cosf(2 * (float)M_PI * dtmf_lo[k] / srate);
		coef[4 + k] = 2 * cosf(2 * (float)M_PI * dtmf_hi[k] / srate);
	}
}

/* see dsp.h: a block is a digit when it is not too quiet (-50 dBFS
   rms), the strongest tone of each group is 6 dB above the others of
   its group, the two together have 70 per cent of the block's energy
   (speech and noise spread theirs), and the high one is within 8 dB
   below and 4 dB above the low one (Q.24's twist) */
char DtmfDetect::classify(float e, const float *p) const
{
	static const char keys[] = "123A456B789C*0#D";
	int lo = 0, hi = 4;

	if (e < (float)n_block * 100.0f * 100.0f)
		return 0;
	for (int k = 1; k < 4; k++) {
		if (p[k] > p[lo])
			lo = k;
		if (p[4 + k] > p[hi])
			hi = 4 + k;
	}
	for (int k = 0; k < 4; k++) {
		if (k != lo && p[k] * 4 > p[lo])
			return 0;
		if (4 + k != hi && p[4 + k] * 4 > p[hi])
			return 0;
	}
	if (2 * (p[lo] + p[hi]) < 0.7f * n_block * e)
		return 0;
	if (p[hi] < p[lo] * 0.158f || p[hi] > p[lo] * 2.51f)
		return 0;
	return keys[lo * 4 + hi - 4];
}

/* see dsp.h */
DtmfDetect::Event DtmfDetect::sample(float x)
{
	float p[8];
	Event ev = NONE;
	char d;

	for (int k = 0; k < 8; k++) {
		float s = x + coef[k] * s1[k] - s2[k];
		s2[k] = s1[k];
		s1[k] = s;
	}
	energy += x * x;
	if (++n < n_block)
		return NONE;
	for (int k = 0; k < 8; k++)
		p[k] = s1[k] * s1[k] + s2[k] * s2[k] - coef[k] * s1[k] * s2[k];
	d = classify(energy, p);
	if (digit) {
		if (d != digit || energy * 4 < ref) {
			digit = 0;
			ev = OFF;
		}
	} else if (d && d == last) {
		digit = d;
		ref = energy;
		ev = ON;
	}
	last = d;
	for (int k = 0; k < 8; k++)
		s1[k] = s2[k] = 0;
	energy = 0;
	n = 0;
	return ev;
}

/*
 *    Call progress tones: ringing and busy
 */

#define CT_BLOCK_MS     20
#define CT_SEG_MIN      (300 / CT_BLOCK_MS)     /* a segment counts */
#define CT_RING_MIN     (600 / CT_BLOCK_MS)     /* of tone: a ring */
#define CT_DROP_MAX     2                       /* blocks of a dropout */
#define CT_GAP          (300 / CT_BLOCK_MS)     /* of quiet: a ring ends */

/* see dsp.h */
void CallToneDetect::init(int srate)
{
	static const float f[5] = { 400, 425, 440, 450, 480 };

	*this = CallToneDetect();
	len = srate * CT_BLOCK_MS / 1000;
	for (int k = 0; k < 5; k++)
		g[k].init(f[k], len, srate);
	ans[0].init(2100, len, srate);
	ans[1].init(2225, len, srate);
	band.bandpass(480, 1.2f, srate);
	drop = CT_GAP;
	ans_drop[0] = ans_drop[1] = CT_GAP;
}

/* see dsp.h */
void CallToneDetect::sample(float x)
{
	float b = band.run(x);

	energy += x * x;
	band_energy += b * b;
	for (int k = 0; k < 5; k++)
		g[k].run(x);
	ans[0].run(x);
	ans[1].run(x);
	if (++n < len)
		return;
	block();
	n = 0;
	energy = band_energy = 0;
}

/* a block of the ringing tone when not too quiet (-50 dBFS rms) and one
   of its frequencies, or one of its pairs, has 60 per cent of the
   block's energy (speech and noise spread theirs); of the busy tone's
   band when that has half of it.  A ring is the burst of segments,
   each 0.3 s of the tone at least, that comes to 0.6 s of them; it ends
   after 0.3 s without.  The band on and off in runs of 0.2 to 0.8 s,
   four times on, is a busy cadence.  An answer tone is 0.3 s of blocks
   with 60 per cent of their energy at its frequency (ANSam's phase
   reversals may cost a block or two), once until it has gone. */
void CallToneDetect::block()
{
	float e = energy / len, t = 0;
	bool on, b;

	for (int k = 0; k < 5; k++)
		t = std::max(t, g[k].power);
	t = std::max(t, g[2].power + g[4].power);       /* 440 + 480 */
	t = std::max(t, g[0].power + g[3].power);       /* 400 + 450 */
	on = e > 100.0f * 100.0f && t > 0.6f * e;
	b = e > 100.0f * 100.0f && band_energy > 0.5f * energy;

	if (on) {
		if (drop > CT_DROP_MAX)
			seg = 0;
		drop = 0;
		if (++seg == CT_SEG_MIN)
			burst += CT_SEG_MIN;
		else if (seg > CT_SEG_MIN)
			burst++;
		if (!ringing && burst >= CT_RING_MIN) {
			ringing = true;
			rings++;
		}
	} else if (++drop == CT_GAP) {
		seg = burst = 0;
		ringing = false;
	}
	if (ringing && on)
		since_ring = 0;
	else
		since_ring++;

	for (int k = 0; k < 2; k++) {
		if (e > 100.0f * 100.0f && ans[k].power > 0.6f * e) {
			if (ans_drop[k] > CT_DROP_MAX)
				ans_run[k] = 0;
			ans_drop[k] = 0;
			if (++ans_run[k] == CT_SEG_MIN)
				answer[k]++;
		} else {
			ans_drop[k]++;
		}
	}

	if (b == (band_on != 0)) {
		band_run++;
	} else {
		int ms = band_run * CT_BLOCK_MS;
		if (ms >= 200 && ms <= 800)
			rounds += band_on;
		else
			rounds = 0;
		band_on = b;
		band_run = 1;
		if (rounds >= 4) {
			rounds = 0;
			busy++;
		}
	}
}

/*
 *    CNG
 */

#define CNG_RUN_MIN     (360 / CT_BLOCK_MS)     /* blocks of a burst */
#define CNG_RUN_MAX     (700 / CT_BLOCK_MS)

/* see dsp.h */
void CngDetect::init(int srate)
{
	*this = CngDetect();
	len = srate * CT_BLOCK_MS / 1000;
	tone.init(1100, len, srate);
	ans.init(2100, len, srate);
	drop = CT_GAP;
}

/* see dsp.h */
void CngDetect::sample(float x)
{
	energy += x * x;
	tone.run(x);
	ans.run(x);
	if (++n < len)
		return;
	block();
	n = 0;
	energy = 0;
}

/* see dsp.h: a run ends on its second block off (one is a dropout) */
void CngDetect::block()
{
	float e = energy / len, rest = e - ans.power;
	bool on = rest > 100.0f * 100.0f && tone.power > 0.6f * rest;

	if (on) {
		if (drop > 1)
			run = 0;
		drop = 0;
		run++;
		return;
	}
	if (++drop == 2) {
		if (run >= CNG_RUN_MIN && run <= CNG_RUN_MAX)
			bursts++;
		run = 0;
	}
}

/*
 *    Ring signal
 */

/* see dsp.h: two sections of 80 Hz low pass, the envelope's time
   constant 100 ms */
void RingDetect::init(int srate_)
{
	*this = RingDetect();
	srate = srate_;
	lp[0].lowpass(80, 0.707f, srate);
	lp[1].lowpass(80, 0.707f, srate);
	decay = 1.0f / (0.1f * srate);
}

/* see dsp.h */
bool RingDetect::sample(float x)
{
	float y = lp[1].run(lp[0].run(x)), a = fabsf(y);
	bool pulse = false;

	if (a > env)
		env = a;
	else
		env -= decay * env;
	if (!on) {
		if (env > 4000) {
			if (++on_samples > srate / 20) {
				on = true;
				off_samples = 0;
			}
		} else {
			on_samples = 0;
		}
	} else if (env < 1500) {
		if (++off_samples > srate * 3 / 20) {
			on = false;
			on_samples = 0;
		}
	} else {
		off_samples = 0;
	}
	if (a > 100) {
		int s = y > 0 ? 1 : -1;
		pulse = on && s > 0 && sign < 0;
		sign = s;
	}
	return pulse;
}

/*
 *    8000 <-> 9600 Hz sample rate conversion through 48 kHz
 */
#define RS_LEN  480     /* prototype taps at 48 kHz */

static float rs_h6[RS_LEN], rs_h5[RS_LEN];

/* the modified Bessel function I0(x), by its power series (40 terms),
   for the Kaiser window */
static double bessel_i0(double x)
{
	double s = 1, t = 1;
	int k;
	for (k = 1; k < 40; k++) {
		t *= (x / (2 * k)) * (x / (2 * k));
		s += t;
	}
	return s;
}

/* the low pass prototype at 48 kHz, cut off at 3870 Hz, Kaiser window
   (beta 7: about 70 dB), sorted into L phases of RS_LEN / L taps */
static void rs_design(float *h, int L)
{
	const double fc = 3870.0 / 48000, beta = 7.0;
	int n, taps = RS_LEN / L;
	for (n = 0; n < RS_LEN; n++) {
		double m = n - (RS_LEN - 1) / 2.0, r = 2 * m / (RS_LEN - 1);
		double s = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
		double w = bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta);
		/* gain L: each output sees one phase of the zero stuffed
		   input */
		h[(n % L) * taps + n / L] = (float)(L * s * w);
	}
}

/* both prototype tables, once (std::call_once from Resampler::init) */
static void rs_tables(void)
{
	rs_design(rs_h6, 6);
	rs_design(rs_h5, 5);
}

/* up / down must be 6/5 (8000 -> 9600 Hz) or 5/6 (9600 -> 8000 Hz):
   0, else -1; the shared tables are built on the first call */
int Resampler::init(int up, int down)
{
	static std::once_flag once;
	std::call_once(once, rs_tables);
	if (!((up == 6 && down == 5) || (up == 5 && down == 6)))
		return -1;
	*this = Resampler();
	L = up;
	M = down;
	taps = RS_LEN / L;
	h = L == 6 ? rs_h6 : rs_h5;
	return 0;
}

/* x into the history, then every output due before the next input:
   phase t of the polyphase filter, t stepping by M modulo L */
int Resampler::run(float x, float out[2])
{
	int n = 0;
	/* the history twice over, so that it reads as one block */
	if (--pos < 0)
		pos = taps - 1;
	hist[pos] = hist[pos + taps] = x;
	while (t < L) {
		const float *hp = h + t * taps, *b = hist + pos;
		float y = 0;
		int k;
		for (k = 0; k < taps; k++)
			y += hp[k] * b[k];
		out[n++] = y;
		t += M;
	}
	t -= L;
	return n;
}

} // namespace dsp
