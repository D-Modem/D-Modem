/*
 *
 *    dsp_util.c - small DSP building blocks for the open data pumps
 *
 */

#include <math.h>
#include <string.h>

#include "dsp.h"


/*
 *    oscillator
 */

void osc_set_freq(struct osc *o, float freq, int srate)
{
	o->inc = (double)freq / srate;
}

float osc_next(struct osc *o)
{
	float v = (float)sin(2 * M_PI * o->phase);
	o->phase += o->inc;
	o->phase -= floor(o->phase);
	return v;
}

cplx osc_next_cplx(struct osc *o)
{
	double a = 2 * M_PI * o->phase;
	cplx v = cplx_make((float)cos(a), (float)sin(a));
	o->phase += o->inc;
	o->phase -= floor(o->phase);
	return v;
}


/*
 *    biquads (RBJ audio EQ cookbook forms)
 */

static void biquad_set(struct biquad *f, double b0, double b1, double b2,
		       double a0, double a1, double a2)
{
	f->b0 = b0 / a0;
	f->b1 = b1 / a0;
	f->b2 = b2 / a0;
	f->a1 = a1 / a0;
	f->a2 = a2 / a0;
	f->z1 = f->z2 = 0;
}

void biquad_bandpass(struct biquad *f, float freq, float q, int srate)
{
	double w = 2 * M_PI * freq / srate;
	double alpha = sin(w) / (2 * q);
	biquad_set(f, alpha, 0, -alpha, 1 + alpha, -2 * cos(w), 1 - alpha);
}

void biquad_lowpass(struct biquad *f, float freq, float q, int srate)
{
	double w = 2 * M_PI * freq / srate;
	double alpha = sin(w) / (2 * q);
	double c = cos(w);
	biquad_set(f, (1 - c) / 2, 1 - c, (1 - c) / 2,
		   1 + alpha, -2 * c, 1 - alpha);
}

void biquad_highpass(struct biquad *f, float freq, float q, int srate)
{
	double w = 2 * M_PI * freq / srate;
	double alpha = sin(w) / (2 * q);
	double c = cos(w);
	biquad_set(f, (1 + c) / 2, -(1 + c), (1 + c) / 2,
		   1 + alpha, -2 * c, 1 - alpha);
}


/*
 *    Goertzel
 */

void goertzel_init(struct goertzel *g, float freq, int len, int srate)
{
	memset(g, 0, sizeof(*g));
	g->coef = 2 * cos(2 * M_PI * freq / srate);
	g->len = len;
}

int goertzel_run(struct goertzel *g, float x)
{
	float s = x + g->coef * g->s1 - g->s2;
	g->s2 = g->s1;
	g->s1 = s;
	if (++g->n < g->len)
		return 0;
	/* |X(k)|^2 scaled so that a sine of amplitude A gives A^2/2 */
	g->power = (g->s1 * g->s1 + g->s2 * g->s2 - g->coef * g->s1 * g->s2)
		* 2 / ((float)g->len * g->len);
	g->s1 = g->s2 = 0;
	g->n = 0;
	return 1;
}


void power_meter_init(struct power_meter *p, float time_const_sec, int srate)
{
	p->alpha = 1.0f / (time_const_sec * srate);
	if (p->alpha > 1)
		p->alpha = 1;
	p->power = 0;
}


/*
 *    V.22 scrambler: polynomial 1 + x^-14 + x^-17.  Like the
 *    recommendation requires, 64 consecutive ones on the line invert
 *    the next bit on both sides.
 */

int v22_scramble(struct v22_scrambler *s, int bit)
{
	int out = (bit ^ (s->reg >> 13) ^ (s->reg >> 16)) & 1;
	if (s->ones >= 64) {
		out ^= 1;
		s->ones = 0;
	}
	s->ones = out ? s->ones + 1 : 0;
	s->reg = (s->reg << 1) | out;
	return out;
}

int v22_descramble(struct v22_scrambler *s, int bit)
{
	int out = (bit ^ (s->reg >> 13) ^ (s->reg >> 16)) & 1;
	if (s->ones >= 64) {
		out ^= 1;
		s->ones = 0;
	}
	s->ones = bit ? s->ones + 1 : 0;
	s->reg = (s->reg << 1) | (bit & 1);
	return out;
}


/*
 *    root raised cosine pulse with unit energy per symbol
 */

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

int dtmf_gen_set(struct dtmf_gen *d, char digit, int srate)
{
	const char *p;
	int i;
	if (digit >= 'a' && digit <= 'd')
		digit -= 'a' - 'A';
	p = strchr(dtmf_keys, digit);
	if (!p || !digit)
		return -1;
	i = p - dtmf_keys;
	memset(d, 0, sizeof(*d));
	osc_set_freq(&d->lo, dtmf_lo[i / 4], srate);
	osc_set_freq(&d->hi, dtmf_hi[i % 4], srate);
	/* high group 2 dB above low group (twist), about -8 dBFS total */
	d->amp_lo = 6000;
	d->amp_hi = 7550;
	return 0;
}

float dtmf_gen_next(struct dtmf_gen *d)
{
	return d->amp_lo * osc_next(&d->lo) + d->amp_hi * osc_next(&d->hi);
}
