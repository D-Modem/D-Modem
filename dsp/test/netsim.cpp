/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    netsim.cpp - the digital network between two modem processes:
 *    G.711 at 8000 Hz, with sample rate conversion for a modem that
 *    runs at 9600 Hz (as if its codec were on an analogue line).
 *
 *    netsim <rate a> <rate b> <ulaw|alaw> <a in> <a out> <b in> <b out>
 *
 *    reads A's samples (16 bit) from fd <a in>, writes them to B on
 *    fd <b out>, and the other way round.  Each impairment is a class
 *    of its own (Drop, Step, Noise, Echo, Loop, Moh: NET_DROP, NET_STEP,
 *    NET_NOISE and NET_FLOOR, NET_ECHO, NET_LOOP, NET_MOH), its members
 *    named for it, set from the environment by its init(); the network
 *    (Net) derives from them all and runs a direction's samples through
 *    them in turn.  NET_CODEC=amr:12.2 (or amrwb:23.85, ...: codec_sim.h)
 *    puts a mobile network's speech codec on the 8000 Hz line in place
 *    of G.711, NET_CODEC_LOSS=<percent> erases that many of its frames
 *    at random (NET_CODEC_SEED).  Toward a 9600 Hz modem (an analogue
 *    line's) the codes may cross a digital loss pad (NET_PAD=<dB>: each
 *    sample re-encoded that much lower) and robbed bit signalling
 *    (NET_RBS=<mask>: in the samples whose count mod 6 has its bit in
 *    the mask, the octet's least significant bit forced to 1, as T1
 *    trunks rob it every sixth frame), as V.90's DIL finds them.
 */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "codec_sim.h"
#include "dsp.h"
#include "g711.h"
#include "qam.h"
#include "v34.h"

using namespace dsp;

/* (the DSP code's log: test_util.cpp's, never at level 0) */

static int alaw;

/* NET_DROP=<at s>:<ms>: the line goes silent both ways for a while (a
   hit) */
struct Drop {
	float drop_at = -1, drop_ms = 0;

	void drop_init(const char *e)
	{
		sscanf(e, "%f:%f", &drop_at, &drop_ms);
	}
	/* silent at t */
	bool drop_on(float t) const
	{
		return drop_at >= 0 && t >= drop_at &&
			t < drop_at + drop_ms / 1000;
	}
};

/* NET_STEP=<at s>:<dB>: its loss changes for good (a reroute) */
struct Step {
	float step_at = -1, step_g = 1;

	void step_init(const char *e)
	{
		float db = 0;
		sscanf(e, "%f:%f", &step_at, &db);
		step_g = powf(10, db / 20);
	}
	/* x at t */
	short step(short x, float t) const
	{
		return step_at >= 0 && t >= step_at ?
			(short)lrintf(x * step_g) : x;
	}
};

/* Gaussian (Box-Muller), the second uniform from [0, 1): not
   test_util.h's gauss(), so that the interop tests' noise stays the
   same */
static double noise_gauss()
{
	double u1 = (rand() + 1.0) / (RAND_MAX + 2.0);
	double u2 = rand() / (RAND_MAX + 1.0);
	return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

/* NET_NOISE=<at s>:<rms>[:<ramp s>[:<until s>]]: white noise from then
   on (rising over the ramp, stopping at until), both ways.
   NET_NOISE_QUIET=<ms>: the noise stops for good once the signal it is
   added to has been silent that long (a V.92 rate renegotiation's
   silent period: what follows it is to come through clean).
   NET_NOISE_DIR: ab A -> B only, ba B -> A, bacodes B -> A on the codes.
   NET_FLOOR=<rms>: white noise all the time (NET_NOISE adds to it) */
struct Noise {
	float noise_at = -1, noise_rms, noise_ramp, noise_end = -1;
	float noise_quiet_ms;
	long noise_quiet_n;
	int noise_dir;          /* 1 A -> B only, 2 B -> A, 3 B -> A on
				   the codes */
	float floor_rms;

	void noise_init()
	{
		if (getenv("NET_NOISE"))
			sscanf(getenv("NET_NOISE"), "%f:%f:%f:%f", &noise_at,
			       &noise_rms, &noise_ramp, &noise_end);
		if (getenv("NET_NOISE_QUIET"))
			noise_quiet_ms = atof(getenv("NET_NOISE_QUIET"));
		if (getenv("NET_NOISE_DIR"))
			noise_dir = !strcmp(getenv("NET_NOISE_DIR"), "ab") ? 1 :
				!strcmp(getenv("NET_NOISE_DIR"), "ba") ? 2 :
				!strcmp(getenv("NET_NOISE_DIR"), "bacodes") ?
				3 : 0;
		if (getenv("NET_FLOOR"))
			floor_rms = atof(getenv("NET_FLOOR"));
	}
	/* NET_NOISE's at t */
	bool noisy(float t) const
	{
		return noise_at >= 0 && t >= noise_at &&
			(noise_end < 0 || t < noise_end);
	}
	/* the noise's rms at t (noisy: noisy(t)) A -> B (a_out) or B -> A;
	   0 if none */
	float noise_level(float t, int noisy, int a_out) const
	{
		return ((noisy || floor_rms) &&
			!(noise_dir == 1 && !a_out) &&
			!(noise_dir >= 2 && a_out)) ?
			(noisy ?
			 sqrtf(noise_rms * noise_rms *
			       (noise_ramp > 0 && t - noise_at < noise_ramp ?
				(t - noise_at) * (t - noise_at) /
				(noise_ramp * noise_ramp) : 1) +
			       floor_rms * floor_rms) :
			 floor_rms) : 0;
	}
	/* NET_NOISE_QUIET: x, at t of a signal at rate, is the noise's last
	   sample if it ends a silence that long */
	void noise_quiet(short x, float t, int rate)
	{
		noise_quiet_n = abs(x) <= 8 ? noise_quiet_n + 1 : 0;
		if (noise_quiet_n >= noise_quiet_ms * rate / 1000) {
			noise_end = t;
			fprintf(stderr, "netsim: noise off at %.2f s\n", t);
		}
	}
};

/* NET_LOOP=<fraction>: on an 8000 Hz line, B's signal reaches A through
   an analogue loop as A's codec samples it: a delay of that fraction of
   a sample (windowed sinc) and a high pass at 300 Hz; else A gets B's
   G.711 values as they are */
struct Loop {
	enum { LOOP_TAPS = 32 };
	float loop_h[LOOP_TAPS], loop_hist[2 * LOOP_TAPS];
	int loop_on, loop_pos;
	float loop_hp_x, loop_hp_y;

	/* the fractional delay: LOOP_TAPS of Hann windowed sinc, frac of a
	   sample later than its middle */
	void loop_init(float frac)
	{
		int n;
		for (n = 0; n < LOOP_TAPS; n++) {
			double t = n - LOOP_TAPS / 2 + 1 - frac;
			double h = fabs(t) < 1e-9 ? 1 :
				sin(M_PI * t) / (M_PI * t);
			h *= 0.5 - 0.5 * cos(2 * M_PI * (n + 0.5) / LOOP_TAPS);
			loop_h[n] = (float)h;
		}
		loop_on = 1;
	}
	/* a sample of B's through the loop: the fractional delay, then the
	   high pass */
	float loop_run(float x)
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
};

/* NET_ECHO=<dB>:<ms>: the hybrid at A's line reflects what A receives
   back towards B: what A received <ms> before, counted in A's samples
   (from the line, before A's codec clips it); with a soft modem as A,
   that has to be more than A sends ahead of what it receives (some
   20 ms for the HSF), else some samples go without their echo */
struct Echo {
	enum { ECHO_MAX = 16384 };
	float echo_g, echo_buf[ECHO_MAX];
	long echo_d, echo_w, echo_r;    /* samples to A, from A */
	long echo_early;                /* of A's without their echo */

	/* the gain and delay; ra: A's rate */
	void echo_init(const char *e, int ra)
	{
		float db = 0, ms = 0;
		sscanf(e, "%f:%f", &db, &ms);
		echo_g = powf(10, db / 20);
		echo_d = (int)(ms * ra / 1000);
		if (echo_d >= ECHO_MAX)
			echo_d = ECHO_MAX - 1;
	}
	/* A sends its sample k as it receives its sample k: x, A's next
	   sample, with the echo of what it received echo_d before */
	short echo_add(short x)
	{
		long k = echo_r - echo_d;
		/* (A sends ahead of what it has received by more than the
		   echo's delay: no echo then) */
		if (k >= echo_w && ++echo_early &&
		    !(echo_early & (echo_early - 1)))
			fprintf(stderr, "netsim: %ld samples without their "
				"echo (A sends %ld samples ahead; NET_ECHO's "
				"delay is too short)\n", echo_early,
				k - echo_w + echo_d + 1);
		if (k >= 0 && k < echo_w && echo_w - k < ECHO_MAX) {
			/* the codec clips the sum */
			long y = lrintf(x + echo_g * echo_buf[k % ECHO_MAX]);
			x = (short)(y > 32767 ? 32767 :
				    y < -32768 ? -32768 : y);
		}
		return x;
	}
	/* n samples A received (on the line) */
	void echo_received(const float *l, int n)
	{
		for (int i = 0; i < n; i++)
			echo_buf[echo_w++ % ECHO_MAX] = l[i];
	}
};

/* NET_SYNC=1: these times count from a SIGUSR1 (interop_call sends it at
   CONNECT) instead of from the start; nothing happens before it */
static int sync_on;
static volatile sig_atomic_t synced;
/* NET_CLOCK_FD=<fd>: the line's clock for interop_call (the modems
   unpaced keep time by the line, not the wall clock): A's samples so
   far in microseconds, at the start of the 8 bytes that fd maps (a
   memfd) */
static long long *line_clock;

/* x (clipped to 16 bits) through the network's law, there and back */
static int g711_line(int x)
{
	if (x > 32767)
		x = 32767;
	if (x < -32768)
		x = -32768;
	return g711::decode(alaw, g711::encode(alaw, x));
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

/* the raised cosine at 48 kHz (roll-off NET_RC, 0.15 by default), Hann
   windowed, as 6 phases of RC_TAPS taps; the history cleared */
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
	SpeechCodec *codec;     /* NET_CODEC's, in place of the law */
	Resampler rs;
	struct rc_up rc;
	short obuf[8192];
	float lbuf[8192];        /* the same on the line: the hybrid's echo
				   (before A's codec clips it) */
	int on;
	long nin;               /* input samples so far */
	long t0;                /* NET_SYNC: nin at the SIGUSR1, -1 before */
};

/* NET_WAV=<file>: the line as it crosses the network (8000 Hz, after
   G.711), A -> B left and B -> A right, from the first SIGUSR2 to the
   second (interop_call: from the dial to the end of the data); the header
   is kept up to date, so the file is whole wherever it stops */
static FILE *wav;
static volatile sig_atomic_t wav_sig;   /* SIGUSR2s so far */
static short *wav_q[2];
static long wav_n[2], wav_cap[2], wav_frames;

/* the header for wav_frames frames, at the file's start (and back to its
   end) */
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

/* SIGUSR2: the first starts NET_WAV's recording, the second ends it */
static void on_wav(int sig)
{
	(void)sig;
	wav_sig++;
}

/* NET_MOH=ack[:<T1 code>]@<s> or nack@<s>: a V.92 analogue modem's answer
   to the digital modem's modem-on-hold request (9.10.2.1/V.92), for the
   modems we test with do not give one.  Once B's tone B (1200 Hz) has
   been there 50 ms, s seconds or more into the line (phase 2 has tone B
   too; before A answers it with a retrain's
   tone A, from about 160 ms on), A is cut off both ways and this sends
   its tone A for 150 ms, then MHack (T1 code, Table 33/V.92; 1 = 10 s by
   default) or MHnack, repeated, until B is silent 200 ms (on hold) or
   sends ANSam (2100 Hz, its fast reconnect after an MHnack); then
   silence, and A gets the line back with B's ANSam. */
struct Moh {
	int moh_mode;           /* 0 off, 1 MHack, 2 MHnack */
	float moh_at;
	int moh_t1 = 1;
	int moh_st;             /* 0 watching, 1 answering, 2 silent,
				   3 done */
	int moh_tone, moh_quiet, moh_ans, moh_blk;
	float moh_e, moh_g1200[3], moh_g2100[3];
	QamTx moh_tx;
	unsigned char moh_bits[40];
	int moh_pos, moh_n;
	long moh_syms;
	float moh_sym = 1;

	/* the answer and when; ra: A's rate */
	void moh_init(const char *e, int ra)
	{
		moh_mode = !strncmp(e, "ack", 3) ? 1 :
			!strncmp(e, "nack", 4) ? 2 : 0;
		if (moh_mode == 1 && e[3] == ':')
			moh_t1 = atoi(e + 4) & 15;
		if (strchr(e, '@'))
			moh_at = atof(strchr(e, '@') + 1);
		/* A's INFO modulation: 600 baud DPSK at 2400 Hz (the
		   analogue modem answers in phase 2), at INFO's level */
		moh_tx.init(ra, 2400, 600, 0.5f, 4, 1, 2988 * 1.41421f);
	}
	/* A cut off both ways: it is answering, or on hold */
	bool moh_cut() const { return moh_st == 1 || moh_st == 2; }
	/* A's next sample while cut off (a_out: A -> B) */
	short moh_sample(int a_out)
	{
		return a_out && moh_st == 1 ?
			(short)lrintf(moh_tx.sample([this] {
				return moh_symbol();
			})) : 0;
	}

	/* moh_tx's next symbol: 90 (150 ms) of tone A, then moh_make's
	   bits as phase reversals, round and round */
	cplx moh_symbol()
	{
		cplx z = { 0, 0 };
		/* 150 ms of the tone (90 symbols), then the sequence,
		   repeated */
		if (moh_syms++ >= 90) {
			if (moh_pos == moh_n)
				moh_pos = 0;
			if (moh_bits[moh_pos++])
				moh_sym = -moh_sym;
		}
		z.re = moh_sym;
		return z;
	}

	/* Table 32/V.92: fill, frame sync, the signal and its information
	   (first bit first in time), the CRC of 10.1.2.3.2/V.34, fill */
	void moh_make(int sig, int info)
	{
		int i;
		moh_n = 0;
		for (i = 0; i < 4; i++)
			moh_bits[moh_n++] = 1;
		for (i = 7; i >= 0; i--)
			moh_bits[moh_n++] = (0x72 >> i) & 1;
		for (i = 3; i >= 0; i--)
			moh_bits[moh_n++] = (sig >> i) & 1;
		for (i = 3; i >= 0; i--)
			moh_bits[moh_n++] = (info >> i) & 1;
		unsigned crc = v34_crc_bits(moh_bits + 12, moh_n - 12, 0xFFFF);
		for (i = 15; i >= 0; i--)
			moh_bits[moh_n++] = (crc >> i) & 1;
		for (i = 0; i < 4; i++)
			moh_bits[moh_n++] = 1;
		moh_pos = 0;
	}

	/* a Goertzel step at f; g: s1, s2, coefficient */
	static void moh_goertzel(float *g, float x)
	{
		float s0 = x + g[2] * g[0] - g[1];
		g[1] = g[0];
		g[0] = s0;
	}

	/* the power at the Goertzel's frequency over its block; s1, s2
	   reset */
	static float moh_power(float *g)
	{
		float p = g[0] * g[0] + g[1] * g[1] - g[2] * g[0] * g[1];
		g[0] = g[1] = 0;
		return p;
	}

	/* B's signal, on its way to A: what it does about the request */
	void moh_watch(float x, int rate, float t)
	{
		if (t < moh_at)
			return;
		int n = rate / 100;     /* 10 ms blocks */
		if (!moh_g1200[2]) {
			moh_g1200[2] = 2 * cosf(2 * (float)M_PI * 1200 / rate);
			moh_g2100[2] = 2 * cosf(2 * (float)M_PI * 2100 / rate);
		}
		moh_e += x * x;
		moh_goertzel(moh_g1200, x);
		moh_goertzel(moh_g2100, x);
		if (++moh_blk < n)
			return;
		/* the share of the block's energy at the tone */
		float e = moh_e, t1200 = 2 * moh_power(moh_g1200) / n,
			t2100 = 2 * moh_power(moh_g2100) / n;
		moh_blk = 0;
		moh_e = 0;
		moh_tone = e > n * 1e4f && t1200 > 0.6f * e ? moh_tone + 1 : 0;
		moh_ans = e > n * 1e4f && t2100 > 0.6f * e ? moh_ans + 1 : 0;
		moh_quiet = e < n * 1e3f ? moh_quiet + 1 : 0;
		if (moh_st == 0 && moh_tone >= 5) {
			moh_st = 1;
			moh_syms = 0;
			moh_sym = 1;
			moh_make(moh_mode == 1 ? 0x5 : 0x7,
				 moh_mode == 1 ? moh_t1 : 0x7);
			fprintf(stderr, "netsim: modem-on-hold request: %s\n",
				moh_mode == 1 ? "MHack" : "MHnack");
		} else if (moh_st == 1 && moh_quiet >= 20) {
			moh_st = 2;
			fprintf(stderr, "netsim: on hold\n");
		} else if ((moh_st == 1 || moh_st == 2) && moh_ans >= 5) {
			moh_st = 3;
			fprintf(stderr, "netsim: ANSam: the line back to A\n");
		}
	}
};

/* the network: the law (alaw) or a speech codec (NET_CODEC), the far
   codec of a 9600 Hz modem and the gains on the way (NET_PLAIN, NET_GAIN,
   NET_GAIN_UP), the impairments */
struct Net : Drop, Step, Noise, Loop, Echo, Moh {
	int plain;      /* NET_PLAIN=1: 8000 -> 9600 Hz with the short
			   resampler of dsp_util, not the raised cosine (the
			   law still on the 8000 Hz samples) */
	float gain = 1;
	float gain_up = 1;      /* NET_GAIN_UP: on the way up from a 9600 Hz
				   modem */
	long clipped;           /* samples at the far codec */
	const char *codec_spec = nullptr;       /* NET_CODEC */
	double codec_loss = 0;                  /* NET_CODEC_LOSS, percent */
	unsigned codec_seed = 1;                /* NET_CODEC_SEED */
	double pad = 1;         /* NET_PAD: the digital loss, a factor */
	int rbs = 0;            /* NET_RBS: the robbed positions mod 6 */

	/* from the environment; ra: A's rate */
	void init(int ra)
	{
		plain = getenv("NET_PLAIN") != NULL;
		codec_spec = getenv("NET_CODEC");
		if (getenv("NET_CODEC_LOSS"))
			codec_loss = atof(getenv("NET_CODEC_LOSS"));
		if (getenv("NET_CODEC_SEED"))
			codec_seed = (unsigned)atoi(getenv("NET_CODEC_SEED"));
		if (getenv("NET_GAIN"))
			gain = atof(getenv("NET_GAIN"));
		if (getenv("NET_GAIN_UP"))
			gain_up = atof(getenv("NET_GAIN_UP"));
		if (getenv("NET_PAD"))
			pad = pow(10, -atof(getenv("NET_PAD")) / 20);
		if (getenv("NET_RBS"))
			rbs = (int)strtol(getenv("NET_RBS"), NULL, 0) & 63;
		if (getenv("NET_DROP"))
			drop_init(getenv("NET_DROP"));
		noise_init();
		if (getenv("NET_LOOP"))
			loop_init(atof(getenv("NET_LOOP")));
		if (getenv("NET_MOH"))
			moh_init(getenv("NET_MOH"), ra);
		if (getenv("NET_STEP"))
			step_init(getenv("NET_STEP"));
		if (getenv("NET_ECHO"))
			echo_init(getenv("NET_ECHO"), ra);
	}
	/* an 8000 Hz sample across the network: through the law, or the
	   speech codec */
	static int line_sample(struct dir *d, int x)
	{
		return d->codec ? d->codec->sample(x) : g711_line(x);
	}
	/* a code on its way to an analogue line's codec (sample n of the
	   direction): the pad, then the robbed bit */
	int digital(int s, long n) const
	{
		if (pad != 1)
			s = g711_line((int)lrint(s * pad));
		if (rbs & (1 << (n % 6)))
			s = g711::decode(alaw, g711::encode(alaw, s) | 1);
		return s;
	}
	int run(struct dir *d, int a_out, int a_in);
};

static Net net;
static SpeechCodec codecs[2];

/* samples of one direction: to 8000 Hz G.711 and on */
int Net::run(struct dir *d, int a_out, int a_in)
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
		if (drop_on(t))
			ibuf[i] = 0;
		/* NET_MOH: A cut off, both ways, while it answers */
		if (moh_mode && a_in)
			moh_watch(ibuf[i], d->rate_in, t);
		if (moh_cut())
			ibuf[i] = moh_sample(a_out);
		ibuf[i] = step(ibuf[i], t);
		/* the noise of an analogue line: on a 8000 Hz G.711
		   signal on its way to a 9600 Hz modem it comes after the
		   codec (below), not on the codes */
		int is_noisy = noisy(t);
		noise = noise_level(t, is_noisy, a_out);
		if (noise && is_noisy && noise_quiet_ms > 0)
			noise_quiet(ibuf[i], t, d->rate_in);
		if (noise && (noise_dir == 3 ||
			      !(d->rate_in == 8000 && d->rate_out != 8000))) {
			double x = ibuf[i] + noise * noise_gauss();
			ibuf[i] = (short)(x > 32767 ? 32767 : x < -32768 ?
					  -32768 : lrint(x));
		}
		/* A sends its sample k as it receives its sample k: the
		   hybrid's echo comes in A's own sample clock */
		if (a_out && echo_g)
			ibuf[i] = echo_add(ibuf[i]);
		if (a_out)
			echo_r++;
		if (d->rate_in == d->rate_out) {
			int s = line_sample(d, ibuf[i]);
			if (loop_on && a_in) {
				float v = loop_run((float)s);
				d->lbuf[o] = v;
				d->obuf[o++] = (short)(v > 32767 ? 32767 :
						       v < -32768 ? -32768 : v);
			} else {
				d->lbuf[o] = (float)s;
				d->obuf[o++] = (short)s;
			}
			wav_put(!a_out, s);
			continue;
		}
		if (d->rate_in == 8000) {
			/* the network, then the far codec (with the
			   analogue loss or gain NET_GAIN on the way): the
			   raised cosine, or NET_PLAIN's short resampler */
			int s = line_sample(d, ibuf[i]);
			float x;
			if (!d->codec)
				s = digital(s, d->nin - 1);
			x = s * gain;
			wav_put(!a_out, s);
			m = plain ? d->rs.run(x, y) : rc_run(&d->rc, x, y);
			for (k = 0; k < m; k++) {
				if (noise && noise_dir != 3)
					y[k] += noise * (float)noise_gauss();
				if ((y[k] > 32767 || y[k] < -32768) &&
				    ++clipped && !(clipped & (clipped - 1)))
					fprintf(stderr, "netsim: %ld samples "
						"clipped at the far codec\n",
						clipped);
				d->lbuf[o] = y[k];
				d->obuf[o++] = (short)(y[k] > 32767 ? 32767 :
						       y[k] < -32768 ? -32768 : y[k]);
			}
		} else {
			/* up from a 9600 Hz modem: its codec to 8000 Hz,
			   then the network */
			m = d->rs.run(ibuf[i] * gain_up, y);
			for (k = 0; k < m; k++) {
				d->obuf[o] = (short)line_sample(d, (int)lrintf(y[k]));
				d->lbuf[o] = d->obuf[o];
				wav_put(!a_out, d->obuf[o]);
				o++;
			}
		}
	}
	if (a_in)
		echo_received(d->lbuf, o);
	if (a_out && line_clock)
		__atomic_store_n(line_clock, (long long)d->nin * 1000000 /
				 d->rate_in, __ATOMIC_RELAXED);
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

/* SIGUSR1: NET_SYNC's times count from now */
static void on_sync(int sig)
{
	(void)sig;
	synced = 1;
}

/* netsim <rate a> <rate b> <law> <fds> (or netsim test: every code
   through its law and back); the two directions as their input comes;
   0 when an input ends, 1 on an error */
int main(int argc, char **argv)
{
	struct dir d[2];
	struct pollfd p[2];
	int ra, rb, k;

	if (argc == 2 && !strcmp(argv[1], "test")) {
		/* every code back to itself */
		int c, bad = 0;
		for (c = 0; c < 256; c++) {
			if (c != 0x7F && g711::lin2ulaw(g711::ulaw2lin(c)) != c)
				bad++;
			if (g711::lin2alaw(g711::alaw2lin(c)) != c)
				bad++;
		}
		printf("ulaw FE %d 80 %d, alaw D5 %d AA %d, bad %d\n",
		       g711::ulaw2lin(0xFE), g711::ulaw2lin(0x80), g711::alaw2lin(0xD5),
		       g711::alaw2lin(0xAA), bad);
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
	net.init(ra);
	if (getenv("NET_CLOCK_FD")) {
		void *m = mmap(NULL, sizeof(*line_clock), PROT_READ |
			       PROT_WRITE, MAP_SHARED,
			       atoi(getenv("NET_CLOCK_FD")), 0);
		if (m == MAP_FAILED)
			perror("netsim: NET_CLOCK_FD");
		else
			line_clock = (long long *)m;
	}
	memset(d, 0, sizeof(d));
	if (net.codec_spec) {
		/* the speech codec, each direction its own, on the 8000 Hz
		   samples: the line (NET=8000:8000) or the far side of a
		   9600 Hz modem's codec (NET=9600:8000) */
		const char *err;
		if (ra == rb && ra != 8000) {
			fprintf(stderr, "netsim: NET_CODEC needs the line at "
				"8000 Hz (NET=8000:8000 or 9600:8000)\n");
			return 1;
		}
		for (k = 0; k < 2; k++) {
			if (codecs[k].init(net.codec_spec, net.codec_loss,
					   net.codec_seed + k, &err)) {
				fprintf(stderr, "netsim: NET_CODEC: %s\n", err);
				return 1;
			}
			d[k].codec = &codecs[k];
		}
		fprintf(stderr, "netsim: %s on the line%s\n",
			codecs[0].name(), net.codec_loss > 0 ?
			", frames erased" : "");
	}
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
				if (net.run(&d[k], k == 0, k == 1))
					return 0;
	}
}
