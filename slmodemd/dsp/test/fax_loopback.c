/*
 *
 *    fax_loopback.c - fax modulation transmitter -> receiver test
 *
 *    usage: fax_loopback <class 1 mod code> [snr_db] [bits] [srate]
 *
 *    V.17 short train codes (74, 98, 122, 146) first run the long train
 *    at the same rate, then the short one with the equalizer kept.
 *
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fax.h"

unsigned int dsplibs_debug_level = 0;

int dsplibs_debug_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	return 0;
}

/* the FSK pump in dp_fsk.c links against the modem core */
int modem_dp_register(enum DP_ID id, struct dp_operations *op) { return 0; }
void modem_dp_deregister(enum DP_ID id, struct dp_operations *op) {}
int modem_get_sreg(struct modem *m, unsigned int num) { return 0; }
long modem_set_param(struct modem *m, unsigned name, int val) { return 0; }
long modem_get_param(struct modem *m, unsigned name) { return 0; }
int modem_get_bits(struct modem *m, int nbits, u8 *buf, int n) { return n; }
int modem_put_bits(struct modem *m, int nbits, u8 *buf, int n) { return n; }

static unsigned prbs = 0x1234567;
static u8 *txb, *rxb;
static long ntx, nrx, maxbits;

static int get_bit(void *ctx)
{
	int b;
	if (ntx >= maxbits)
		return FAX_BIT_END;
	b = ((prbs >> 22) ^ (prbs >> 17)) & 1;
	prbs = (prbs << 1) | b;
	txb[ntx++] = b;
	return b;
}

static void put_bit(void *ctx, int bit)
{
	if (nrx < 2 * maxbits)
		rxb[nrx++] = bit;
}

static double gauss(void)
{
	double u = (rand() + 1.0) / (RAND_MAX + 2.0);
	double v = (rand() + 1.0) / (RAND_MAX + 2.0);
	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static struct biquad tilt;
static float noise;

/* one transmission; returns 0 when all bits arrived intact */
static int run(enum fax_mod mod, int short_train, int srate, int code,
	       struct fax_rx_memory *mem)
{
	struct fax_tx tx;
	struct fax_rx rx;
	float delay[64] = { 0 };
	long n, best = -1, err = 0, i, off;

	ntx = nrx = 0;
	fax_tx_init(&tx, mod, short_train, srate, get_bit, NULL);
	if (fax_rx_init(&rx, mod, short_train, srate, put_bit, NULL, mem))
		return 2;
	/* a little silence, the signal, silence */
	for (n = 0; n < srate * 120L; n++) {
		float y = n > srate / 10 ? fax_tx_sample(&tx) : 0;
		float x = delay[n & 63];
		delay[n & 63] = y;
		x = biquad_run(&tilt, x) * 0.5f + noise * gauss();
		{ int oc = rx.carrier; fax_rx_sample(&rx, x);
		  if (getenv("TRACE_CD") && oc != rx.carrier)
			fprintf(stderr, "%.3f carrier %d det %.0f level %.0f floor %.0f\n", (double)n / srate, rx.carrier, rx.det_pwr, rx.det_level, rx.det_floor); }
		if (tx.done && n % srate == 0 && !rx.carrier)
			break;
	}
	fax_rx_save(&rx, mem);
	fax_rx_free(&rx);
	/* the offset with the fewest errors in the first 256 bits */
	for (off = 0, err = 64; off < 200 && off + 256 <= nrx; off++) {
		long e = 0;
		for (i = 0; i < 256; i++)
			e += rxb[off + i] != txb[i];
		if (e < err) {
			err = e;
			best = off;
		}
	}
	err = 0;
	if (best < 0) {
		printf("mod %d%s: sent %ld, received %ld bits, no alignment: FAIL\n",
		       code, short_train ? " (short)" : "", ntx, nrx);
		return 1;
	}
	for (i = 0; i < ntx && best + i < nrx; i++)
		if (rxb[best + i] != txb[i] && ++err <= 6 && getenv("SHOW_ERRORS"))
			printf("error at bit %ld\n", i);
	printf("mod %d%s (%d bit/s): sent %ld, received %ld (offset %ld), "
	       "compared %ld, errors %ld: %s\n", code,
	       short_train ? " short train" : "", fax_mod_bps(mod), ntx,
	       nrx, best, i, err, err == 0 && i >= ntx - 64 ? "PASS" : "FAIL");
	return !(err == 0 && i >= ntx - 64);
}

int main(int argc, char **argv)
{
	int code = argc > 1 ? atoi(argv[1]) : 96;
	float snr = argc > 2 ? atof(argv[2]) : 30;
	int srate = argc > 4 ? atoi(argv[4]) : 9600;
	enum fax_mod mod;
	int short_train, ret = 0;
	struct fax_rx_memory mem;

	maxbits = argc > 3 ? atol(argv[3]) : 20000;
	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	if (!fax_mod_from_code(code, &mod, &short_train)) {
		fprintf(stderr, "unknown modulation %d\n", code);
		return 2;
	}
	txb = malloc(maxbits);
	rxb = malloc(2 * maxbits);
	srand(1);
	memset(&mem, 0, sizeof(mem));
	noise = fax_tx_rms(mod) * 0.5f * powf(10, -snr / 20);
	biquad_lowpass(&tilt, 3000, 0.6f, srate);
	if (short_train)
		ret = run(mod, 0, srate, code - 1, &mem);
	ret |= run(mod, short_train, srate, code, &mem);
	return ret;
}
