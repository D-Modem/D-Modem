/*
 *
 *    dsp_loopback.c - back to back test of the open data pumps
 *
 *    Runs a calling and an answering modem against each other through
 *    a simulated line (gain, delay, white noise, optional tilt) and
 *    checks the bit error rate of the data phase in both directions.
 *
 *    usage: dsp_loopback <dp id> [snr_db] [seconds] [srate] [gain] [ppm]
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
};

static struct dp_operations *ops[DP_MAX];
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
	if (name == MDMPRM_TX_RATE)
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
	float ring[1024];
	long n;
};

static float line_resample(struct line *l, float x)
{
	/* one output per input, read at a slowly drifting position that
	   starts 256 samples behind the newest input */
	double t = l->n * l->step - 256;
	long i = (long)floor(t);
	float f = (float)(t - i), a, b;
	l->ring[l->n & 1023] = x;
	l->n++;
	if (i < 0 || l->n - i > 1000)
		return 0;
	a = l->ring[i & 1023];
	b = l->ring[(i + 1) & 1023];
	return a + f * (b - a);
}

static float line_run(struct line *l, float x)
{
	float y = l->delay[l->pos];
	l->delay[l->pos] = x;
	if (++l->pos >= l->len)
		l->pos = 0;
	if (l->step != 1)
		y = line_resample(l, y);
	y = biquad_run(&l->tilt, y) * l->gain + l->noise * gauss();
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
	for (i = 0, best_err = 0; i < n && best + i < tx->ntx; i++)
		best_err += tx->tx_bits[best + i] != rx->rx_bits[start_rx + i];
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
	s16 a_in[FRAG], a_out[FRAG], c_in[FRAG], c_out[FRAG];
	struct line to_a, to_c;
	long t, total = (long)(secs * srate);
	int i, k, ret, fail = 0;

	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
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
	/* white noise relative to a DSP_TX_AMPLITUDE signal after the gain */
	to_a.noise = to_c.noise = DSP_TX_AMPLITUDE / sqrtf(2) * gain *
		powf(10, -snr / 20);
	biquad_lowpass(&to_a.tilt, 3000, 0.6f, srate);
	biquad_lowpass(&to_c.tilt, 3000, 0.6f, srate);

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
				printf("%6.2fs %s: change dp -> %d\n",
				       (double)t / srate, s->name, id);
				s->dp->op->delete(s->dp);
				s->dp = ops[id]->create(fake[k], id, 1, srate,
							FRAG, ops[id]);
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
			a_in[i] = dsp_clip16(line_run(&to_a, c_out[i]));
			c_in[i] = dsp_clip16(line_run(&to_c, a_out[i]));
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
