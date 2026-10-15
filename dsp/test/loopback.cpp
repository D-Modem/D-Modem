/*
 *
 *    loopback.cpp - back to back test of the open data pumps
 *    (loopback.h): a calling and an answering modem against each other
 *    through a simulated line, the bit errors of the data phase counted
 *    in both directions
 *
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../dsp.h"
#include "modem_dp.h"  /* it plays the modem core */
#include "modem_param.h"
#include "dsplibs.h"
#include "loopback.h"

using namespace dsp;

#define FRAG 48
#define MAX_BITS (1 << 20)
/* the share of the bits its rate carries from the CONNECT to the end
   that a call must have delivered (LoopbackSide::ok()) */
#define CARRIED_MIN 0.9

unsigned int dsplibs_debug_level = 0;

/* the pumps' log (DSP_DEBUG): to stderr */
int dsplibs_debug_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	return 0;
}

/* ---- fake modem core ---- */

struct side {
	const char *name;
	struct dp *dp;
	struct dp_runtime *rt;
	int connected;
	long connect_sample;
	int tx_rate, rx_rate;
	unsigned prbs;
	u8 *tx_bits, *rx_bits;
	long ntx, nrx;
	long connect_rx;
	int dp_requested;
};

static struct dp_operations *ops[256];   /* by id: DP_MAX is below some ids */
static struct side sides[2];
/* the line's sample now, and from when what the pumps receive is
   dropped (LoopbackConfig::cut_s; 0: never) */
static long now_sample, drop_from;
/* the pumps' struct modem: only its address matters here */
struct modem {
	int side;
};
static struct modem fake_modems[2] = { { 0 }, { 1 } };
static struct modem *fake[2];

/* the side whose fake modem m is */
static struct side *side_of(struct modem *m)
{
	return m == fake[0] ? &sides[0] : &sides[1];
}

/* a pump's operations by its id */
int modem_dp_register(enum DP_ID id, struct dp_operations *op)
{
	ops[id] = op;
	return 0;
}

/* and gone */
void modem_dp_deregister(enum DP_ID id, struct dp_operations *op)
{
	ops[id] = NULL;
}

/* the S registers the pumps read: S7 50 s, S10 1.4 s, the DTMF
   duration, the dial pause and dial tone wait; 0 for the rest */
int modem_get_sreg(struct modem *m, unsigned int num)
{
	switch (num) {
	case SREG_WAIT_CARRIER_AFTER_DIAL: return 50;
	case SREG_CARRIER_LOSS_DISCONNECT_TIME: return 14;
	case SREG_DTMF_DURATION: return 95;
	case SREG_DIAL_PAUSE_TIME: return 2;
	case SREG_DIAL_TONE_WAIT_TIME: return 5;
	default: return 0;
	}
}

static struct voice_info vinfo;

/* the parameters the pumps read: the side's runtime, no dial string,
   the voice info, the calling tone on; -1 for the rest */
long modem_get_param(struct modem *m, unsigned name)
{
	switch (name) {
	case MDMPRM_DPRUNTIME: return (long)side_of(m)->rt;
	case MDMPRM_DIALSTR: return (long)"";
	case MDMPRM_VOICEINFO: return (long)&vinfo;
	case GetCallingToneFlag: return 1;
	default: return -1;
	}
}

/* what the pumps tell the core: the pump they want next (automode)
   and the rates */
long modem_set_param(struct modem *m, unsigned name, int val)
{
	struct side *s = side_of(m);
	if (name == MDMPRM_DP_REQUESTED)
		s->dp_requested = val;
	else if (name == MDMPRM_TX_RATE)
		s->tx_rate = val;
	else if (name == MDMPRM_RX_RATE)
		s->rx_rate = val;
	return 0;
}

/* n bits for the pump to send, one a byte: PRBS 2^23-1, the first
   MAX_BITS kept in tx_bits; n */
int modem_get_bits(struct modem *m, int nbits, u8 *buf, int n)
{
	struct side *s = side_of(m);
	int i;
	for (i = 0; i < n; i++) {
		/* PRBS 2^23-1 */
		int b = ((s->prbs >> 22) ^ (s->prbs >> 17)) & 1;
		s->prbs = (s->prbs << 1) | b;
		buf[i] = b;
		if (s->ntx < MAX_BITS)
			s->tx_bits[s->ntx++] = b;
	}
	return n;
}

/* n bits the pump received, the first MAX_BITS kept in rx_bits (none
   from drop_from on); n */
int modem_put_bits(struct modem *m, int nbits, u8 *buf, int n)
{
	struct side *s = side_of(m);
	int i;
	if (drop_from && now_sample >= drop_from)
		return n;
	for (i = 0; i < n; i++)
		if (s->nrx < MAX_BITS)
			s->rx_bits[s->nrx++] = buf[i] & 1;
	return n;
}

/* no DTE: what is sent is gone, nothing comes */
int modem_send_to_tty(struct modem *m, const char *buf, int n) { return n; }
int modem_recv_from_tty(struct modem *m, char *buf, int n) { return 0; }

/* ---- line simulation ---- */

/* Gaussian of unit power (Box-Muller) */
static double gauss(void)
{
	double u = (rand() + 1.0) / (RAND_MAX + 2.0);
	double v = (rand() + 1.0) / (RAND_MAX + 2.0);
	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

struct line {
	float delay[256];
	int pos, len;
	float gain, noise;
	Biquad tilt;
	/* sample clock offset (line_clock()): a windowed sinc resampler,
	   step input samples an output, its input kept in ring (mask + 1
	   samples) and read lag samples behind the newest at the start */
	double step, lag;
	float *ring;
	long mask;
	float sig_pwr, ref_pwr;
	long n;
};

/* l's clock offset for total samples of line time: step input samples
   an output.  The reading drifts by (step - 1) total samples over
   them: it starts 256 samples behind the input, more if it would
   overtake it (step > 1), and the ring keeps 1024 samples, more if it
   would fall that far behind (step < 1), so that every output is read
   from the line's own samples.  (Not enough at the start: the old
   reader went on reading the ring's stale samples, or none.) */
static void line_clock(struct line *l, double step, long total)
{
	double drift = fabs(step - 1) * total + 64;
	long size = 1024;

	l->step = step;
	l->lag = step > 1 && drift + 32 > 256 ? drift + 32 : 256;
	while (size < l->lag + (step < 1 ? drift : 0) + 64)
		size *= 2;
	l->ring = static_cast<float *>(calloc(size, sizeof(float)));
	l->mask = size - 1;
}

/* x into the clock offset's resampler; the line at the drifting
   position (l->step input samples an output), 0 at the start */
static float line_resample(struct line *l, float x)
{
	/* one output per input, read at a slowly drifting position that
	   starts l->lag samples behind the newest input */
	double t = l->n * l->step - l->lag;
	long i = (long)floor(t);
	float f = (float)(t - i), y = 0;
	int k;
	l->ring[l->n & l->mask] = x;
	l->n++;
	/* the window (i - 15 to i + 16) in the ring: not before the
	   start, not ahead of the newest sample, not overwritten */
	if (i < 16 || i + 16 > l->n - 1 || l->n - (i - 15) > l->mask + 1)
		return 0;
	/* 32 tap Blackman windowed sinc interpolation */
	for (k = -15; k <= 16; k++) {
		double d = k - f, w, sinc;
		sinc = fabs(d) < 1e-9 ? 1 : sin(M_PI * d) / (M_PI * d);
		w = 0.42 + 0.5 * cos(M_PI * d / 16.5) + 0.08 * cos(2 * M_PI * d / 16.5);
		y += l->ring[(i + k) & l->mask] * sinc * w;
	}
	return y;
}

/* one sample over a line: its delay (len samples), the clock offset,
   the tilt, the gain, and noise at l->noise of the received rms */
static float line_run(struct line *l, float x)
{
	float y = l->delay[l->pos];
	l->delay[l->pos] = x;
	if (++l->pos >= l->len)
		l->pos = 0;
	if (l->step != 1)
		y = line_resample(l, y);
	y = l->tilt.run(y) * l->gain;
	/* noise relative to the actual signal power (slow average, held
	   during silence) */
	l->sig_pwr += (y * y - l->sig_pwr) / 2400.0f;
	if (l->sig_pwr > 0.05f * l->ref_pwr)
		l->ref_pwr = l->sig_pwr;
	y += sqrtf(l->ref_pwr) * l->noise * gauss();
	return y;
}

/* where rx bit r lines up with the transmitted bits (searching from
   tx bit "from"), -1 if nowhere */
static long find_alignment(struct side *tx, struct side *rx, long r,
			   long from)
{
	long off, i;
	if (from < 0)
		from = 0;
	for (off = from; off + 128 < tx->ntx; off++) {
		long err = 0;
		for (i = 0; i < 128 && err < 32 && r + i < rx->nrx; i++)
			err += tx->tx_bits[off + i] != rx->rx_bits[r + i];
		if (err < 8)
			return off;
	}
	return -1;
}

static long resyncs;

/* bit errors over the bits received after the connection (after
   a retrain, which loses bits, the alignment is found again) */
static long count_errors(struct side *tx, struct side *rx, long *checked)
{
	long best, best_err = 0, i, n, run = 0, gap;
	long skip = (rx->nrx - rx->connect_rx) / 4;
	long start_rx = rx->connect_rx + (skip < 2000 ? skip : 2000);
	n = rx->nrx - start_rx - 64;
	*checked = 0;
	if (n > 200000)
		n = 200000;
	if (n < 300)
		return 0;
	best = find_alignment(tx, rx, start_rx, 0);
	if (best < 0) {
		*checked = n;
		return n / 2;
	}
	for (i = 0; i < n && best + i < tx->ntx; i++) {
		int e = tx->tx_bits[best + i] != rx->rx_bits[start_rx + i];
		best_err += e;
		/* errors at random for 64 bits: lost or gained bits */
		run = e ? run + 1 : (run > 0 ? run - 1 : 0);
		if (run > 24) {
			long r = start_rx + i + 1, nb;
			nb = find_alignment(tx, rx, r, best + i - 64 * 1024);
			if (nb >= 0) {
				resyncs++;
				gap = nb - (best + i + 1);
				best = nb - (i + 1);
				best_err -= run;        /* the bits of the slip */
				run = 0;
				if (getenv("SHOW_ERRORS"))
					fprintf(stderr, "%s: realigned by %ld bits at %ld\n",
						rx->name, gap, i);
			}
		}
		if (e && getenv("SHOW_ERRORS"))
			fprintf(stderr, "%s: bit error at %ld\n", rx->name, i);
	}
	*checked = i;
	return best_err;
}

/* (loopback.h) the caller from DP_CALLPROG on (then cfg.caller_dp or
   cfg.dp, or what automode asks for), the answerer at cfg.dp, FRAG
   samples at a time each way through the two lines and the echoes for
   cfg.secs; their CONNECTs and pump changes printed; then the bit
   errors of what each side received after its CONNECT */
LoopbackResult loopback_run(const LoopbackConfig &cfg)
{
	LoopbackResult res;
	int id = cfg.dp;
	float snr = cfg.snr;
	float secs = cfg.secs;
	int srate = cfg.srate;
	float gain = cfg.gain;
	float ppm = cfg.ppm;
	float echo_db = cfg.echo_db;
	float echo_gain = echo_db < 0 ? powf(10, echo_db / 20) : 0;
	float echo_a[64] = { 0 }, echo_c[64] = { 0 };
	Biquad echo_fa, echo_fc;
	int echo_pos = 0;
	float far_db = cfg.far_db;
	float far_gain = far_db < 0 ? powf(10, far_db / 20) : 0;
	int far_delay = cfg.far_delay;
	static float far_a[1 << 15], far_c[1 << 15];
	Biquad far_fa, far_fc;
	int far_pos = 0;
	s16 a_in[FRAG], a_out[FRAG], c_in[FRAG], c_out[FRAG];
	struct line to_a, to_c;
	long t, total = (long)(secs * srate);
	int i, k, ret, id2;
	static bool registered;

	FILE *dump_a = NULL, *dump_c = NULL;

	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	if (cfg.dump) {
		/* what each side hears: <dump>.a (answerer), <dump>.c (caller) */
		char name[256];
		snprintf(name, sizeof(name), "%s.a", cfg.dump);
		dump_a = fopen(name, "wb");
		snprintf(name, sizeof(name), "%s.c", cfg.dump);
		dump_c = fopen(name, "wb");
	}
	srand(1);
	memset(far_a, 0, sizeof(far_a));
	memset(far_c, 0, sizeof(far_c));
	memset(sides, 0, sizeof(sides));
	for (k = 0; k < 2; k++) {
		fake[k] = &fake_modems[k];
		sides[k].name = k ? "answerer" : "caller";
		sides[k].rt = static_cast<dp_runtime*>(dp_runtime_create(fake[k]));
		sides[k].prbs = k ? 0x1234567 : 0x7654321;
		sides[k].tx_bits = static_cast<u8*>(malloc(MAX_BITS));
		sides[k].rx_bits = static_cast<u8*>(malloc(MAX_BITS));
	}
	if (!registered) {
		prop_dp_init();
		registered = true;
	}

	memset(&to_a, 0, sizeof(to_a));
	memset(&to_c, 0, sizeof(to_c));
	to_a.len = 37;
	to_c.len = 53;
	to_a.gain = to_c.gain = gain;
	line_clock(&to_a, 1 + ppm * 1e-6, total);
	line_clock(&to_c, 1 - ppm * 1e-6, total);
	/* white noise at snr dB below the received signal power */
	to_a.noise = to_c.noise = powf(10, -snr / 20);
	to_a.tilt.lowpass(3000, 0.6f, srate);
	echo_fa.bandpass(1500, 0.5f, srate);
	echo_fc.bandpass(1500, 0.5f, srate);
	far_fa.bandpass(1500, 0.4f, srate);
	far_fc.bandpass(1500, 0.4f, srate);
	if (far_delay > (1 << 15) - 200)
		far_delay = (1 << 15) - 200;
	to_c.tilt.lowpass(3000, 0.6f, srate);
	if (cfg.flat) {
		/* no band limitation */
		memset(&to_a.tilt, 0, sizeof(to_a.tilt));
		memset(&to_c.tilt, 0, sizeof(to_c.tilt));
		to_a.tilt.b0 = to_c.tilt.b0 = 1;
	}

	sides[0].dp = ops[DP_CALLPROG]->create(fake[0], DP_CALLPROG, 1, srate, FRAG,
					       ops[DP_CALLPROG]);
	sides[1].dp = ops[id]->create(fake[1], (enum DP_ID)id, 0, srate, FRAG, ops[id]);
	memset(a_in, 0, sizeof(a_in));
	memset(c_in, 0, sizeof(c_in));

	drop_from = cfg.cut_s > 0 ? (long)(cfg.cut_s * srate) : 0;
	for (t = 0; t < total; t += FRAG) {
		now_sample = t;
		for (k = 0; k < 2; k++) {
			struct side *s = &sides[k];
			s16 *in = k ? a_in : c_in, *out = k ? a_out : c_out;
			if (!s->dp) {
				memset(out, 0, sizeof(a_out));
				continue;
			}
			ret = s->dp->op->process(s->dp, in, out, FRAG);
			switch (ret) {
			case DPSTAT_OK:
				break;
			case DPSTAT_CHANGEDP:
				s->dp->op->destroy(s->dp);
				if (k == 0 && s->dp_requested) {
					/* automode change requested */
					id2 = s->dp_requested;
					s->dp_requested = 0;
				} else if (k == 0)
					id2 = cfg.caller_dp ? cfg.caller_dp : id;
				else {
					id2 = s->dp_requested;
					s->dp_requested = 0;
				}
				printf("%6.2fs %s: dp -> %d\n",
				       (double)t / srate, s->name, id2);
				s->dp = ops[id2]->create(fake[k], (enum DP_ID)id2, k == 0,
							 srate, FRAG, ops[id2]);
				break;
			case DPSTAT_CONNECT:
				s->connected = 1;
				s->connect_sample = t;
				s->connect_rx = s->nrx;
				printf("%6.2fs %s: CONNECT tx %d rx %d\n",
				       (double)t / srate, s->name,
				       s->tx_rate, s->rx_rate);
				break;
			default:
				printf("%6.2fs %s: dp status %d\n",
				       (double)t / srate, s->name, ret);
				s->dp->op->destroy(s->dp);
				s->dp = NULL;
				res.dp_failed = true;
				break;
			}
		}
		for (i = 0; i < FRAG; i++) {
			/* own signal echo: 20 sample delay, band limited */
			float ea = echo_fa.run(echo_a[(echo_pos + 44) & 63]);
			float ec = echo_fc.run(echo_c[(echo_pos + 44) & 63]);
			echo_a[echo_pos] = a_out[i];
			echo_c[echo_pos] = c_out[i];
			echo_pos = (echo_pos + 1) & 63;
			/* far echo: both line delays and the far hybrid */
			int fd = to_a.len + to_c.len + 8 + far_delay;
			float fa = far_fa.run(far_a[(far_pos - fd) & 0x7FFF]);
			float fc = far_fc.run(far_c[(far_pos - fd) & 0x7FFF]);
			far_a[far_pos] = a_out[i];
			far_c[far_pos] = c_out[i];
			far_pos = (far_pos + 1) & 0x7FFF;
			a_in[i] = dsp_clip16(line_run(&to_a, c_out[i]) + echo_gain * ea +
					     far_gain * gain * fa);
			c_in[i] = dsp_clip16(line_run(&to_c, a_out[i]) + echo_gain * ec +
					     far_gain * gain * fc);
		}
		if (dump_a) {
			fwrite(a_in, sizeof(a_in[0]), FRAG, dump_a);
			fwrite(c_in, sizeof(c_in[0]), FRAG, dump_c);
		}
	}

	if (dump_a) {
		fclose(dump_a);
		fclose(dump_c);
	}
	for (k = 0; k < 2; k++) {
		struct side *s = &sides[k];
		LoopbackSide &r = res.side[k];
		r.connected = s->connected;
		r.connect_s = (double)s->connect_sample / srate;
		r.tx_rate = s->tx_rate;
		r.rx_rate = s->rx_rate;
		r.received = s->nrx;
		if (s->connected) {
			r.carried = s->nrx - s->connect_rx;
			r.expected = (long)(s->rx_rate * (secs - r.connect_s));
			if (r.expected > MAX_BITS - s->connect_rx)
				r.expected = MAX_BITS - s->connect_rx;
			printf("%s: carried %ld of the %ld bits expected "
			       "(%.0f%%)\n", s->name, r.carried, r.expected,
			       r.expected ? 100.0 * r.carried / r.expected : 0);
		}
		if (s->connected) {
			resyncs = 0;
			r.errors = count_errors(&sides[1 - k], s, &r.checked);
			r.resyncs = resyncs;
		}
	}
	free(to_a.ring);
	free(to_c.ring);
	for (k = 0; k < 2; k++) {
		struct side *s = &sides[k];
		if (s->dp)
			s->dp->op->destroy(s->dp);
		dp_runtime_delete(s->rt);
		free(s->tx_bits);
		free(s->rx_bits);
	}
	return res;
}

/* (loopback.h) connected, CARRIED_MIN of the bits expected carried,
   bits checked, a BER of 1e-3 at most */
bool LoopbackSide::ok() const
{
	return connected && carried >= CARRIED_MIN * expected && checked &&
		(double)errors / checked <= 1e-3;
}

/* (loopback.h) "<name>: received ..., checked ..., errors ... (BER)",
   and the realignments */
std::string LoopbackSide::text(const char *name) const
{
	char buf[200];
	if (!connected)
		return std::string(name) + ": no connection";
	snprintf(buf, sizeof(buf), "%s: received %ld bits (%ld of the %ld "
		 "the call should carry), checked %ld, errors %ld (BER %.2e)",
		 name, received, carried, expected, checked, errors,
		 checked ? (double)errors / checked : 1.0);
	std::string t = buf;
	if (resyncs)
		t += ", realigned " + std::to_string(resyncs) + " times";
	return t;
}
