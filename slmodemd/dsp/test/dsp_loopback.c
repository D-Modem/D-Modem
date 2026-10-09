/*
 *
 *    dsp_loopback.c - back to back test of the open data pumps
 *
 *    Runs a calling and an answering modem against each other through
 *    a simulated line (gain, delay, white noise, optional tilt) and
 *    checks the bit error rate of the data phase in both directions.
 *
 *    usage: dsp_loopback <dp id> [snr_db] [seconds] [srate] [gain] [ppm]
 *                        [echo_db] [far_echo_db]
 *
 *    CALLER_DP=<id> in the environment gives the caller another data
 *    pump than the answerer (automode tests).
 *
 *    far_echo_db adds the echo from the far end's hybrid: each modem's
 *    own signal after a round trip over the line (FAR_DELAY samples more
 *    when set, as over VoIP)
 *    echo_db adds an echo of each modem's own signal to its input
 *    (hybrid echo, about 2 ms delay) at that level relative to the
 *    transmitted signal, e.g. -10.
 *
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../dsp.h"

#define FRAG 48
#define MAX_BITS (1 << 20)

unsigned int dsplibs_debug_level = 0;

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
static struct modem *fake[2];

static struct side *side_of(struct modem *m)
{
	return m == fake[0] ? &sides[0] : &sides[1];
}

int modem_dp_register(enum DP_ID id, struct dp_operations *op)
{
	ops[id] = op;
	return 0;
}

void modem_dp_deregister(enum DP_ID id, struct dp_operations *op)
{
	ops[id] = NULL;
}

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

int modem_put_bits(struct modem *m, int nbits, u8 *buf, int n)
{
	struct side *s = side_of(m);
	int i;
	for (i = 0; i < n; i++)
		if (s->nrx < MAX_BITS)
			s->rx_bits[s->nrx++] = buf[i] & 1;
	return n;
}

int modem_send_to_tty(struct modem *m, const char *buf, int n) { return n; }
int modem_recv_from_tty(struct modem *m, char *buf, int n) { return 0; }

/* ---- line simulation ---- */

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
	struct biquad tilt;
	/* sample clock offset: linear interpolation resampler */
	double step;
	float sig_pwr, ref_pwr;
	float ring[1024];
	long n;
};

static float line_resample(struct line *l, float x)
{
	/* one output per input, read at a slowly drifting position that
	   starts 256 samples behind the newest input */
	double t = l->n * l->step - 256;
	long i = (long)floor(t);
	float f = (float)(t - i), y = 0;
	int k;
	l->ring[l->n & 1023] = x;
	l->n++;
	if (i < 16 || l->n - i > 1000)
		return 0;
	/* 32 tap Blackman windowed sinc interpolation */
	for (k = -15; k <= 16; k++) {
		double d = k - f, w, sinc;
		sinc = fabs(d) < 1e-9 ? 1 : sin(M_PI * d) / (M_PI * d);
		w = 0.42 + 0.5 * cos(M_PI * d / 16.5) + 0.08 * cos(2 * M_PI * d / 16.5);
		y += l->ring[(i + k) & 1023] * sinc * w;
	}
	return y;
}

static float line_run(struct line *l, float x)
{
	float y = l->delay[l->pos];
	l->delay[l->pos] = x;
	if (++l->pos >= l->len)
		l->pos = 0;
	if (l->step != 1)
		y = line_resample(l, y);
	y = biquad_run(&l->tilt, y) * l->gain;
	/* noise relative to the actual signal power (slow average, held
	   during silence) */
	l->sig_pwr += (y * y - l->sig_pwr) / 2400.0f;
	if (l->sig_pwr > 0.05f * l->ref_pwr)
		l->ref_pwr = l->sig_pwr;
	y += sqrtf(l->ref_pwr) * l->noise * gauss();
	return y;
}

/* best alignment of rx against tx, returns bit errors over 'n' bits */
static long count_errors(struct side *tx, struct side *rx, long *checked)
{
	long best = -1, best_err = 0, off, i, n;
	long skip = (rx->nrx - rx->connect_rx) / 4;
	long start_rx = rx->connect_rx + (skip < 2000 ? skip : 2000);
	n = rx->nrx - start_rx - 64;
	if (n <= 0) {
		*checked = 0;
		return 0;
	}
	if (n > 200000)
		n = 200000;
	if (n < 300) {
		*checked = 0;
		return 0;
	}
	for (off = 0; off + n < tx->ntx && off < tx->ntx; off++) {
		long err = 0;
		for (i = 0; i < 128 && err < 32; i++)
			err += tx->tx_bits[off + i] != rx->rx_bits[start_rx + i];
		if (err < 8) {
			best = off;
			break;
		}
	}
	if (best < 0) {
		*checked = n;
		return n / 2;
	}
	for (i = 0, best_err = 0; i < n && best + i < tx->ntx; i++) {
		int e = tx->tx_bits[best + i] != rx->rx_bits[start_rx + i];
		best_err += e;
		if (e && getenv("SHOW_ERRORS"))
			fprintf(stderr, "%s: bit error at %ld\n", rx->name, i);
	}
	*checked = i;
	return best_err;
}

int main(int argc, char **argv)
{
	int id = argc > 1 ? atoi(argv[1]) : DP_V22BIS;
	float snr = argc > 2 ? atof(argv[2]) : 40;
	float secs = argc > 3 ? atof(argv[3]) : 20;
	int srate = argc > 4 ? atoi(argv[4]) : 9600;
	float gain = argc > 5 ? atof(argv[5]) : 0.5f;
	float ppm = argc > 6 ? atof(argv[6]) : 0;
	float echo_db = argc > 7 ? atof(argv[7]) : 0;
	float echo_gain = echo_db < 0 ? powf(10, echo_db / 20) : 0;
	float echo_a[64] = { 0 }, echo_c[64] = { 0 };
	struct biquad echo_fa, echo_fc;
	int echo_pos = 0;
	float far_db = argc > 8 ? atof(argv[8]) : 0;
	float far_gain = far_db < 0 ? powf(10, far_db / 20) : 0;
	int far_delay = getenv("FAR_DELAY") ? atoi(getenv("FAR_DELAY")) : 0;
	static float far_a[1 << 15], far_c[1 << 15];
	struct biquad far_fa, far_fc;
	int far_pos = 0;
	s16 a_in[FRAG], a_out[FRAG], c_in[FRAG], c_out[FRAG];
	struct line to_a, to_c;
	long t, total = (long)(secs * srate);
	int i, k, ret, fail = 0, id2;

	FILE *dump_a = NULL, *dump_c = NULL;

	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	if (getenv("DUMP")) {
		/* what each side hears: DUMP.a (answerer), DUMP.c (caller) */
		char name[256];
		snprintf(name, sizeof(name), "%s.a", getenv("DUMP"));
		dump_a = fopen(name, "wb");
		snprintf(name, sizeof(name), "%s.c", getenv("DUMP"));
		dump_c = fopen(name, "wb");
	}
	srand(1);
	for (k = 0; k < 2; k++) {
		fake[k] = calloc(1, sizeof(struct modem) + 64);
		sides[k].name = k ? "answerer" : "caller";
		sides[k].rt = dp_runtime_create(fake[k]);
		sides[k].prbs = k ? 0x1234567 : 0x7654321;
		sides[k].tx_bits = malloc(MAX_BITS);
		sides[k].rx_bits = malloc(MAX_BITS);
	}
	prop_dp_init();

	memset(&to_a, 0, sizeof(to_a));
	memset(&to_c, 0, sizeof(to_c));
	to_a.len = 37;
	to_c.len = 53;
	to_a.gain = to_c.gain = gain;
	to_a.step = 1 + ppm * 1e-6;
	to_c.step = 1 - ppm * 1e-6;
	/* white noise at snr dB below the received signal power */
	to_a.noise = to_c.noise = powf(10, -snr / 20);
	biquad_lowpass(&to_a.tilt, 3000, 0.6f, srate);
	biquad_bandpass(&echo_fa, 1500, 0.5f, srate);
	biquad_bandpass(&echo_fc, 1500, 0.5f, srate);
	biquad_bandpass(&far_fa, 1500, 0.4f, srate);
	biquad_bandpass(&far_fc, 1500, 0.4f, srate);
	if (far_delay > (1 << 15) - 200)
		far_delay = (1 << 15) - 200;
	biquad_lowpass(&to_c.tilt, 3000, 0.6f, srate);
	if (getenv("FLAT")) {
		/* no band limitation */
		memset(&to_a.tilt, 0, sizeof(to_a.tilt));
		memset(&to_c.tilt, 0, sizeof(to_c.tilt));
		to_a.tilt.b0 = to_c.tilt.b0 = 1;
	}

	sides[0].dp = ops[DP_CALLPROG]->create(fake[0], DP_CALLPROG, 1, srate, FRAG,
					       ops[DP_CALLPROG]);
	sides[1].dp = ops[id]->create(fake[1], id, 0, srate, FRAG, ops[id]);
	memset(a_in, 0, sizeof(a_in));
	memset(c_in, 0, sizeof(c_in));

	for (t = 0; t < total; t += FRAG) {
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
				s->dp->op->delete(s->dp);
				if (k == 0 && s->dp_requested) {
					/* automode change requested */
					id2 = s->dp_requested;
					s->dp_requested = 0;
				} else if (k == 0)
					id2 = getenv("CALLER_DP") ?
						atoi(getenv("CALLER_DP")) : id;
				else {
					id2 = s->dp_requested;
					s->dp_requested = 0;
				}
				printf("%6.2fs %s: dp -> %d\n",
				       (double)t / srate, s->name, id2);
				s->dp = ops[id2]->create(fake[k], id2, k == 0,
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
				s->dp->op->delete(s->dp);
				s->dp = NULL;
				fail = 1;
				break;
			}
		}
		for (i = 0; i < FRAG; i++) {
			/* own signal echo: 20 sample delay, band limited */
			float ea = biquad_run(&echo_fa, echo_a[(echo_pos + 44) & 63]);
			float ec = biquad_run(&echo_fc, echo_c[(echo_pos + 44) & 63]);
			echo_a[echo_pos] = a_out[i];
			echo_c[echo_pos] = c_out[i];
			echo_pos = (echo_pos + 1) & 63;
			/* far echo: both line delays and the far hybrid */
			int fd = to_a.len + to_c.len + 8 + far_delay;
			float fa = biquad_run(&far_fa, far_a[(far_pos - fd) & 0x7FFF]);
			float fc = biquad_run(&far_fc, far_c[(far_pos - fd) & 0x7FFF]);
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

	for (k = 0; k < 2; k++) {
		long n, err;
		if (!sides[k].connected) {
			printf("%s: no connection\n", sides[k].name);
			fail = 1;
			continue;
		}
		err = count_errors(&sides[1 - k], &sides[k], &n);
		printf("%s: received %ld bits, checked %ld, errors %ld (BER %.2e)\n",
		       sides[k].name, sides[k].nrx, n, err, n ? (double)err / n : 1.0);
		if (n == 0 || (double)err / n > 1e-3)
			fail = 1;
	}
	printf("%s\n", fail ? "FAIL" : "PASS");
	return fail;
}
