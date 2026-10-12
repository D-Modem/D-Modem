/*
 *    netsim.cpp - the digital network between two modem_half processes:
 *    G.711 at 8000 Hz, with sample rate conversion for a modem that
 *    runs at 9600 Hz (as if its codec were on an analogue line).
 *
 *    netsim <rate a> <rate b> <ulaw|alaw> <a in> <a out> <b in> <b out>
 *
 *    reads A's samples (16 bit) from fd <a in>, writes them to B on
 *    fd <b out>, and the other way round.  The impairments (NET_DROP,
 *    NET_STEP, NET_NOISE, NET_FLOOR, NET_ECHO, NET_LOOP, ...) are
 *    described where they are kept.
 */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dsp.h"

using namespace dsp;

unsigned int dsplibs_debug_level = 0;
int dsplibs_debug_printf(const char *fmt, ...) { return 0; }

static int alaw;
static int plain;       /* NET_PLAIN=1: 8000 -> 9600 Hz with the short
			   resampler of dsp_util, not the raised cosine */
static float gain = 1;
static float gain_up = 1;    /* NET_GAIN_UP: on the way up from a 9600 Hz modem */
/* NET_DROP=<at s>:<ms>: the line goes silent both ways for a while (a
   hit); NET_STEP=<at s>:<dB>: its loss changes for good (a reroute) */
static float drop_at = -1, drop_ms, step_at = -1, step_g = 1;
/* NET_NOISE=<at s>:<rms>[:<ramp s>[:<until s>]]: white noise from then
   on (rising over the ramp, stopping at until), both ways */
static float noise_at = -1, noise_rms, noise_ramp, noise_end = -1;
/* NET_NOISE_QUIET=<ms>: the noise stops for good once the signal it is
   added to has been silent that long (a V.92 rate renegotiation's
   silent period: what follows it is to come through clean) */
static float noise_quiet_ms;
static long noise_quiet_n;
static int noise_dir;           /* NET_NOISE_DIR: 1 A -> B only, 2 B -> A,
				   3 B -> A on the codes */
/* NET_LOOP=<fraction>: on an 8000 Hz line, B's signal reaches A through
   an analogue loop as A's codec samples it: a delay of that fraction of
   a sample (windowed sinc) and a high pass at 300 Hz; else A gets B's
   G.711 values as they are */
#define LOOP_TAPS 32
static float loop_h[LOOP_TAPS], loop_hist[2 * LOOP_TAPS];
static int loop_on, loop_pos;
static float loop_hp_x, loop_hp_y;

static void loop_init(float frac)
{
	int n;
	for (n = 0; n < LOOP_TAPS; n++) {
		double t = n - LOOP_TAPS / 2 + 1 - frac;
		double h = fabs(t) < 1e-9 ? 1 : sin(M_PI * t) / (M_PI * t);
		h *= 0.5 - 0.5 * cos(2 * M_PI * (n + 0.5) / LOOP_TAPS);
		loop_h[n] = (float)h;
	}
	loop_on = 1;
}

static float loop_run(float x)
{
	float y = 0, a = 0.79f;  /* exp(-2 pi 300 / 8000) */
	int k;
	if (--loop_pos < 0)
		loop_pos = LOOP_TAPS - 1;
	loop_hist[loop_pos] = loop_hist[loop_pos + LOOP_TAPS] = x;
	for (k = 0; k < LOOP_TAPS; k++)
		y += loop_h[k] * loop_hist[loop_pos + k];
	/* one pole high pass */
	loop_hp_y = a * (loop_hp_y + y - loop_hp_x);
	loop_hp_x = y;
	return loop_hp_y;
}

/* NET_FLOOR=<rms>: white noise all the time (NET_NOISE adds to it) */
static float floor_rms;
/* NET_SYNC=1: these times count from a SIGUSR1 (interop.py sends it at
   CONNECT) instead of from the start; nothing happens before it */
static int sync_on;
static volatile sig_atomic_t synced;
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
	Resampler rs;
	struct rc_up rc;
	short obuf[8192];
	int on;
	long nin;               /* input samples so far */
	long t0;                /* NET_SYNC: nin at the SIGUSR1, -1 before */
};

/* Gaussian (Box-Muller) */
static double gauss()
{
	double u1 = (rand() + 1.0) / (RAND_MAX + 2.0);
	double u2 = rand() / (RAND_MAX + 1.0);
	return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

/* NET_WAV=<file>: the line as it crosses the network (8000 Hz, after
   G.711), A -> B left and B -> A right, from the first SIGUSR2 to the
   second (interop.py: from the dial to the end of the data); the header
   is kept up to date, so the file is whole wherever it stops */
static FILE *wav;
static volatile sig_atomic_t wav_sig;   /* SIGUSR2s so far */
static short *wav_q[2];
static long wav_n[2], wav_cap[2], wav_frames;

static void wav_header(void)
{
	unsigned char h[44];
	unsigned long data = (unsigned long)wav_frames * 4;
	static const unsigned char fmt[16] = {
		1, 0, 2, 0, 0x40, 0x1f, 0, 0,   /* PCM, 2 ch, 8000 Hz */
		0, 0x7d, 0, 0, 4, 0, 16, 0 };   /* 32000 B/s, 4, 16 bit */
	memcpy(h, "RIFF", 4);
	h[4] = (data + 36) & 255; h[5] = (data + 36) >> 8 & 255;
	h[6] = (data + 36) >> 16 & 255; h[7] = (data + 36) >> 24 & 255;
	memcpy(h + 8, "WAVEfmt ", 8);
	h[16] = 16; h[17] = h[18] = h[19] = 0;
	memcpy(h + 20, fmt, 16);
	memcpy(h + 36, "data", 4);
	h[40] = data & 255; h[41] = data >> 8 & 255;
	h[42] = data >> 16 & 255; h[43] = data >> 24 & 255;
	fseek(wav, 0, SEEK_SET);
	fwrite(h, 1, 44, wav);
	fseek(wav, 0, SEEK_END);
}

/* a network sample of one direction (ch 0: A -> B) */
static void wav_put(int ch, int v)
{
	if (!wav || wav_sig != 1)
		return;
	if (wav_n[ch] == wav_cap[ch]) {
		wav_cap[ch] = wav_cap[ch] ? 2 * wav_cap[ch] : 8192;
		wav_q[ch] = (short *)realloc(wav_q[ch],
					     wav_cap[ch] * sizeof(short));
	}
	wav_q[ch][wav_n[ch]++] = (short)v;
}

/* both directions' samples so far, side by side; at the end, the one
   behind padded with silence */
static void wav_flush(int end)
{
	long n = wav_n[0] < wav_n[1] ? wav_n[0] : wav_n[1], i;
	if (!wav)
		return;
	if (end)
		n = wav_n[0] > wav_n[1] ? wav_n[0] : wav_n[1];
	for (i = 0; i < n; i++) {
		short f[2];
		f[0] = i < wav_n[0] ? wav_q[0][i] : 0;
		f[1] = i < wav_n[1] ? wav_q[1][i] : 0;
		fwrite(f, 2, 2, wav);
	}
	wav_frames += n;
	for (i = 0; i < 2; i++) {
		long k = wav_n[i] > n ? wav_n[i] - n : 0;
		memmove(wav_q[i], wav_q[i] + n, k * sizeof(short));
		wav_n[i] = k;
	}
	if (n)
		wav_header();
	fflush(wav);
	if (end) {
		fclose(wav);
		wav = NULL;
		fprintf(stderr, "netsim: %.1f s of the line in NET_WAV\n",
			wav_frames / 8000.0);
	}
}

static void on_wav(int sig)
{
	(void)sig;
	wav_sig++;
}

/* samples of one direction: to 8000 Hz G.711 and on */
static int run(struct dir *d, int a_out, int a_in)
{
	short ibuf[2048];
	float noise;
	int n = read(d->in, ibuf, sizeof(ibuf)), i, k, m, o = 0;
	float y[2];
	if (n <= 0)
		return n == 0 || errno != EINTR ? -1 : 0;
	n /= 2;
	for (i = 0; i < n; i++) {
		float t = (float)d->nin++ / d->rate_in;
		if (sync_on) {
			if (synced && d->t0 < 0)
				d->t0 = d->nin;
			t = d->t0 < 0 ? -1e9f : (float)(d->nin - d->t0) /
				d->rate_in;
		}
		if (drop_at >= 0 && t >= drop_at && t < drop_at + drop_ms / 1000)
			ibuf[i] = 0;
		if (step_at >= 0 && t >= step_at)
			ibuf[i] = (short)lrintf(ibuf[i] * step_g);
		/* the noise of an analogue line: on a 8000 Hz G.711
		   signal on its way to a 9600 Hz modem it comes after the
		   codec (below), not on the codes */
		int noisy = noise_at >= 0 && t >= noise_at &&
			(noise_end < 0 || t < noise_end);
		noise = ((noisy || floor_rms) &&
			 !(noise_dir == 1 && !a_out) &&
			 !(noise_dir >= 2 && a_out)) ?
			(noisy ?
			 sqrtf(noise_rms * noise_rms *
			       (noise_ramp > 0 && t - noise_at < noise_ramp ?
				(t - noise_at) * (t - noise_at) /
				(noise_ramp * noise_ramp) : 1) +
			       floor_rms * floor_rms) :
			 floor_rms) : 0;
		if (noise && noisy && noise_quiet_ms > 0) {
			noise_quiet_n = abs(ibuf[i]) <= 8 ?
				noise_quiet_n + 1 : 0;
			if (noise_quiet_n >= noise_quiet_ms * d->rate_in / 1000) {
				noise_end = t;
				fprintf(stderr, "netsim: noise off at %.2f s\n",
					t);
			}
		}
		if (noise && (noise_dir == 3 ||
			      !(d->rate_in == 8000 && d->rate_out != 8000 &&
				!plain))) {
			double x = ibuf[i] + noise * gauss();
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
			if (loop_on && a_in) {
				float v = loop_run((float)g711(ibuf[i]));
				d->obuf[o++] = (short)(v > 32767 ? 32767 :
						       v < -32768 ? -32768 : v);
			} else
				d->obuf[o++] = (short)g711(ibuf[i]);
			wav_put(!a_out, g711(ibuf[i]));
			continue;
		}
		if (d->rate_in == 8000 && !plain) {
			/* the network, then the far codec (with the
			   analogue loss or gain NET_GAIN on the way) */
			wav_put(!a_out, g711(ibuf[i]));
			m = rc_run(&d->rc, g711(ibuf[i]) * gain, y);
			for (k = 0; k < m; k++) {
				if (noise && noise_dir != 3)
					y[k] += noise * (float)gauss();
				static long clipped;
				if ((y[k] > 32767 || y[k] < -32768) &&
				    ++clipped && !(clipped & (clipped - 1)))
					fprintf(stderr, "netsim: %ld samples "
						"clipped at the far codec\n",
						clipped);
				d->obuf[o++] = (short)(y[k] > 32767 ? 32767 :
						       y[k] < -32768 ? -32768 : y[k]);
			}
		} else {
			if (d->rate_in == 8000)
				wav_put(!a_out, g711(ibuf[i]));
			m = d->rs.run(ibuf[i] * gain_up, y);
			for (k = 0; k < m; k++) {
				d->obuf[o++] = (short)g711((int)lrintf(y[k]));
				if (d->rate_out == 8000)
					wav_put(!a_out, d->obuf[o - 1]);
			}
		}
	}
	if (a_in)
		for (i = 0; i < o; i++)
			echo_buf[echo_w++ % ECHO_MAX] = d->obuf[i];
	if (wav)
		wav_flush(wav_sig > 1);
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

static void on_sync(int sig)
{
	(void)sig;
	synced = 1;
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
	if (getenv("NET_WAV")) {
		wav = fopen(getenv("NET_WAV"), "wb");
		if (!wav)
			perror(getenv("NET_WAV"));
		else {
			wav_header();
			signal(SIGUSR2, on_wav);
			atexit([] { wav_flush(1); });
		}
	}
	if (getenv("NET_SYNC")) {
		sync_on = 1;
		signal(SIGUSR1, on_sync);
	}
	if (getenv("NET_GAIN"))
		gain = atof(getenv("NET_GAIN"));
	if (getenv("NET_GAIN_UP"))
		gain_up = atof(getenv("NET_GAIN_UP"));
	if (getenv("NET_DROP"))
		sscanf(getenv("NET_DROP"), "%f:%f", &drop_at, &drop_ms);
	if (getenv("NET_NOISE"))
		sscanf(getenv("NET_NOISE"), "%f:%f:%f:%f", &noise_at,
		       &noise_rms, &noise_ramp, &noise_end);
	if (getenv("NET_NOISE_QUIET"))
		noise_quiet_ms = atof(getenv("NET_NOISE_QUIET"));
	if (getenv("NET_NOISE_DIR"))
		noise_dir = !strcmp(getenv("NET_NOISE_DIR"), "ab") ? 1 :
			!strcmp(getenv("NET_NOISE_DIR"), "ba") ? 2 :
			!strcmp(getenv("NET_NOISE_DIR"), "bacodes") ? 3 : 0;
	if (getenv("NET_LOOP"))
		loop_init(atof(getenv("NET_LOOP")));
	if (getenv("NET_FLOOR"))
		floor_rms = atof(getenv("NET_FLOOR"));
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
		    d[k].rs.init(d[k].rate_out / 1600,
				   d[k].rate_in / 1600)) {
			fprintf(stderr, "netsim: rates %d/%d\n",
				d[k].rate_in, d[k].rate_out);
			return 1;
		}
		rc_init(&d[k].rc);
		d[k].t0 = -1;
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
