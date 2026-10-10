/*
 *    netsim.c - the digital network between two modem_half processes:
 *    G.711 at 8000 Hz, with sample rate conversion for a modem that
 *    runs at 9600 Hz (as if its codec were on an analogue line).
 *
 *    netsim <rate a> <rate b> <ulaw|alaw> <a in> <a out> <b in> <b out>
 *
 *    reads A's samples (16 bit) from fd <a in>, writes them to B on
 *    fd <b out>, and the other way round.
 */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dsp.h"

unsigned int dsplibs_debug_level = 0;
int dsplibs_debug_printf(const char *fmt, ...) { return 0; }

static int alaw;
static int plain;       /* NET_PLAIN=1: 8000 -> 9600 Hz with the short
			   resampler of dsp_util, not the raised cosine */
static float gain = 1;
/* NET_DROP=<at s>:<ms>: the line goes silent both ways for a while (a
   hit); NET_STEP=<at s>:<dB>: its loss changes for good (a reroute) */
static float drop_at = -1, drop_ms, step_at = -1, step_g = 1;
/* NET_NOISE=<at s>:<rms>: white noise from then on, both ways */
static float noise_at = -1, noise_rms;
/* NET_DELAY=<ms>: see main().
   NET_ECHO=<dB>:<ms>: the hybrid at A's line reflects what A receives
   back towards B */
#define ECHO_MAX 16384
static float echo_g, echo_buf[ECHO_MAX];
static long echo_d, echo_w, echo_r;     /* samples to A, from A */

/* G.711 (the usual reference algorithms) */
static int seg_search(int v, const short *t)
{
	int i;
	for (i = 0; i < 8; i++)
		if (v <= t[i])
			return i;
	return 8;
}

static unsigned char lin2ulaw(int x)
{
	static const short seg_end[8] = { 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF,
					  0x7FF, 0xFFF, 0x1FFF };
	int mask = 0xFF, seg;
	x >>= 2;
	if (x < 0) {
		x = -x;
		mask = 0x7F;
	}
	if (x > 8159)
		x = 8159;
	x += 0x84 >> 2;
	seg = seg_search(x, seg_end);
	if (seg >= 8)
		return (unsigned char)(0x7F ^ mask);
	return (unsigned char)(((seg << 4) | ((x >> (seg + 1)) & 15)) ^ mask);
}

static int ulaw2lin(unsigned char u)
{
	int t;
	u = ~u;
	t = ((u & 15) << 3) + 0x84;
	t <<= (u & 0x70) >> 4;
	return (u & 0x80) ? 0x84 - t : t - 0x84;
}

static unsigned char lin2alaw(int x)
{
	static const short seg_end[8] = { 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF,
					  0x3FF, 0x7FF, 0xFFF };
	int mask = 0xD5, seg, aval;
	x >>= 3;
	if (x < 0) {
		mask = 0x55;
		x = -x - 1;
	}
	seg = seg_search(x, seg_end);
	if (seg >= 8)
		return (unsigned char)(0x7F ^ mask);
	aval = seg << 4;
	aval |= seg < 2 ? (x >> 1) & 15 : (x >> seg) & 15;
	return (unsigned char)(aval ^ mask);
}

static int alaw2lin(unsigned char a)
{
	int t, seg;
	a ^= 0x55;
	t = (a & 15) << 4;
	seg = (a & 0x70) >> 4;
	if (seg == 0)
		t += 8;
	else if (seg == 1)
		t += 0x108;
	else
		t = (t + 0x108) << (seg - 1);
	return (a & 0x80) ? t : -t;
}

static int g711(int x)
{
	if (x > 32767)
		x = 32767;
	if (x < -32768)
		x = -32768;
	return alaw ? alaw2lin(lin2alaw(x)) : ulaw2lin(lin2ulaw(x));
}

/* 8000 -> 9600 Hz like a codec on an analogue line: a raised cosine
   (Nyquist at 4000 Hz, roll-off 0.15) at 48 kHz, so that the PCM
   samples stay free of intersymbol interference; the converter of
   dsp_util cuts below 4000 Hz, which takes away part of the PCM signal */
#define RC_TAPS 240             /* per phase, 6 phases: 30 ms */
struct rc_up {
	float h[6][RC_TAPS];
	float hist[2 * RC_TAPS];
	int pos, t;
};

static void rc_init(struct rc_up *r)
{
	const double a = getenv("NET_RC") ? atof(getenv("NET_RC")) : 0.15;
	const double fs = 8000;
	int n, len = 6 * RC_TAPS;
	memset(r, 0, sizeof(*r));
	for (n = 0; n < len; n++) {
		double t = (n - len / 2) / 48000.0, x = fs * t, h;
		double d = 1 - (2 * a * x) * (2 * a * x);
		h = x == 0 ? 1 : sin(M_PI * x) / (M_PI * x);
		h *= fabs(d) < 1e-9 ? M_PI / 4 : cos(M_PI * a * x) / d;
		/* Hann window */
		h *= 0.5 - 0.5 * cos(2 * M_PI * (n + 0.5) / len);
		r->h[n % 6][n / 6] = (float)h;
	}
}

/* one 8000 Hz sample in, 1 or 2 samples at 9600 Hz out */
static int rc_run(struct rc_up *r, float x, float *out)
{
	int n = 0;
	if (--r->pos < 0)
		r->pos = RC_TAPS - 1;
	r->hist[r->pos] = r->hist[r->pos + RC_TAPS] = x;
	while (r->t < 6) {
		const float *h = r->h[r->t], *b = r->hist + r->pos;
		float y = 0;
		int k;
		for (k = 0; k < RC_TAPS; k++)
			y += h[k] * b[k];
		out[n++] = y;
		r->t += 5;
	}
	r->t -= 6;
	return n;
}

struct dir {
	int in, out, rate_in, rate_out;
	struct resampler rs;
	struct rc_up rc;
	short obuf[8192];
	int on;
	long nin;               /* input samples so far */
};

/* samples of one direction: to 8000 Hz G.711 and on */
static int run(struct dir *d, int a_out, int a_in)
{
	short ibuf[2048];
	int n = read(d->in, ibuf, sizeof(ibuf)), i, k, m, o = 0;
	float y[2];
	if (n <= 0)
		return n == 0 || errno != EINTR ? -1 : 0;
	n /= 2;
	for (i = 0; i < n; i++) {
		float t = (float)d->nin++ / d->rate_in;
		if (drop_at >= 0 && t >= drop_at && t < drop_at + drop_ms / 1000)
			ibuf[i] = 0;
		if (step_at >= 0 && t >= step_at)
			ibuf[i] = (short)lrintf(ibuf[i] * step_g);
		if (noise_at >= 0 && t >= noise_at) {
			/* Gaussian (Box-Muller) */
			double u1 = (rand() + 1.0) / (RAND_MAX + 2.0);
			double u2 = rand() / (RAND_MAX + 1.0);
			double x = ibuf[i] + noise_rms *
				sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
			ibuf[i] = (short)(x > 32767 ? 32767 : x < -32768 ?
					  -32768 : lrint(x));
		}
		/* A sends its sample k as it receives its sample k: the
		   hybrid's echo comes in A's own sample clock */
		if (a_out && echo_g) {
			long k = echo_r - echo_d;
			if (k >= 0 && k < echo_w && echo_w - k < ECHO_MAX)
				ibuf[i] = (short)lrintf(ibuf[i] + echo_g *
							echo_buf[k % ECHO_MAX]);
		}
		if (a_out)
			echo_r++;
		if (d->rate_in == d->rate_out) {
			d->obuf[o++] = (short)g711(ibuf[i]);
			continue;
		}
		if (d->rate_in == 8000 && !plain) {
			/* the network, then the far codec (with the
			   analogue loss or gain NET_GAIN on the way) */
			m = rc_run(&d->rc, g711(ibuf[i]) * gain, y);
			for (k = 0; k < m; k++)
				d->obuf[o++] = (short)(y[k] > 32767 ? 32767 :
						       y[k] < -32768 ? -32768 : y[k]);
		} else {
			m = resampler_run(&d->rs, ibuf[i], y);
			for (k = 0; k < m; k++)
				d->obuf[o++] = (short)g711((int)lrintf(y[k]));
		}
	}
	if (a_in)
		for (i = 0; i < o; i++)
			echo_buf[echo_w++ % ECHO_MAX] = d->obuf[i];
	for (i = 0; i < o * 2; ) {
		k = write(d->out, (char *)d->obuf + i, o * 2 - i);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return -1;
		i += k;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct dir d[2];
	struct pollfd p[2];
	int ra, rb, k;

	if (argc == 2 && !strcmp(argv[1], "test")) {
		/* every code back to itself */
		int c, bad = 0;
		for (c = 0; c < 256; c++) {
			if (c != 0x7F && lin2ulaw(ulaw2lin(c)) != c)
				bad++;
			if (lin2alaw(alaw2lin(c)) != c)
				bad++;
		}
		printf("ulaw FE %d 80 %d, alaw D5 %d AA %d, bad %d\n",
		       ulaw2lin(0xFE), ulaw2lin(0x80), alaw2lin(0xD5),
		       alaw2lin(0xAA), bad);
		return bad != 0;
	}
	if (argc < 8) {
		fprintf(stderr, "usage: netsim <rate a> <rate b> <ulaw|alaw> "
			"<a in> <a out> <b in> <b out>\n");
		return 1;
	}
	ra = atoi(argv[1]);
	rb = atoi(argv[2]);
	alaw = !strcmp(argv[3], "alaw");
	plain = getenv("NET_PLAIN") != NULL;
	if (getenv("NET_GAIN"))
		gain = atof(getenv("NET_GAIN"));
	if (getenv("NET_DROP"))
		sscanf(getenv("NET_DROP"), "%f:%f", &drop_at, &drop_ms);
	if (getenv("NET_NOISE"))
		sscanf(getenv("NET_NOISE"), "%f:%f", &noise_at, &noise_rms);
	if (getenv("NET_STEP")) {
		float db = 0;
		sscanf(getenv("NET_STEP"), "%f:%f", &step_at, &db);
		step_g = powf(10, db / 20);
	}
	if (getenv("NET_ECHO")) {
		float db = 0, ms = 0;
		sscanf(getenv("NET_ECHO"), "%f:%f", &db, &ms);
		echo_g = powf(10, db / 20);
		echo_d = (int)(ms * ra / 1000);
		if (echo_d >= ECHO_MAX)
			echo_d = ECHO_MAX - 1;
	}
	memset(d, 0, sizeof(d));
	/* A -> B */
	d[0].in = atoi(argv[4]);
	d[0].out = atoi(argv[7]);
	d[0].rate_in = ra;
	d[0].rate_out = rb;
	/* B -> A */
	d[1].in = atoi(argv[6]);
	d[1].out = atoi(argv[5]);
	d[1].rate_in = rb;
	d[1].rate_out = ra;
	for (k = 0; k < 2; k++) {
		if (d[k].rate_in != d[k].rate_out &&
		    resampler_init(&d[k].rs, d[k].rate_out / 1600,
				   d[k].rate_in / 1600)) {
			fprintf(stderr, "netsim: rates %d/%d\n",
				d[k].rate_in, d[k].rate_out);
			return 1;
		}
		rc_init(&d[k].rc);
		p[k].fd = d[k].in;
		p[k].events = POLLIN;
	}
	/* NET_DELAY=<ms>: transport delay each way (a VoIP path's
	   packetization and jitter buffers): the streams start with it */
	if (getenv("NET_DELAY"))
		for (k = 0; k < 2; k++) {
			int n = (int)(atof(getenv("NET_DELAY")) *
				      d[k].rate_out / 1000);
			memset(d[k].obuf, 0, sizeof(d[k].obuf));
			while (n > 0) {
				int w = n < 4096 ? n : 4096;
				if (write(d[k].out, d[k].obuf, w * 2) != w * 2)
					return 1;
				n -= w;
			}
		}
	for (;;) {
		if (poll(p, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		for (k = 0; k < 2; k++)
			if (p[k].revents & (POLLIN | POLLHUP))
				if (run(&d[k], k == 0, k == 1))
					return 0;
	}
}
