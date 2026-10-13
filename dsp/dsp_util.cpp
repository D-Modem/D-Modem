/*
 *
 *    dsp_util.cpp - small DSP building blocks for the open data pumps
 *
 */

#include <math.h>
#include <pthread.h>
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

/* both prototype tables, once (pthread_once from Resampler::init) */
static void rs_tables(void)
{
	rs_design(rs_h6, 6);
	rs_design(rs_h5, 5);
}

/* up / down must be 6/5 (8000 -> 9600 Hz) or 5/6 (9600 -> 8000 Hz):
   0, else -1; the shared tables are built on the first call */
int Resampler::init(int up, int down)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	pthread_once(&once, rs_tables);
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
