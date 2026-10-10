/*
 *    spandsp_v17.cpp - spandsp's V.17 transmitter (libspandsp) into our fax
 *    receiver at 8000 Hz, through G.711 mu-law as over Asterisk: a long
 *    train (as for TCF), then a short train with its equalizer (a page).
 *
 *    spandsp_v17 [bit/s] [bits] [ulaw: 1]
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fax.h"

using namespace dsp;

unsigned int dsplibs_debug_level = 0;

int dsplibs_debug_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	return 0;
}

int modem_dp_register(enum DP_ID id, struct dp_operations *op) { return 0; }
void modem_dp_deregister(enum DP_ID id, struct dp_operations *op) {}
int modem_get_sreg(struct modem *m, unsigned int num) { return 0; }
long modem_set_param(struct modem *m, unsigned name, int val) { return 0; }
long modem_get_param(struct modem *m, unsigned name) { return 0; }
int modem_get_bits(struct modem *m, int nbits, u8 *buf, int n) { return n; }
int modem_put_bits(struct modem *m, int nbits, u8 *buf, int n) { return n; }

/* spandsp_tx.cpp */
void *sp_v17_tx(int bps, int short_train, int (*get_bit)(void *), void *ctx);
int sp_v17_samples(void *s, short *amp, int n);
void sp_v17_free(void *s);
int sp_ulaw(int x);
int sp_end_of_data(void);

static unsigned prbs = 0x1234567;
static unsigned char *txb, *rxb;
static long ntx, nrx, maxbits;

void *sp_t4_tx(const char *file, int page, int encoding, int min_bits);
int sp_t4_bit(void *s);
static void *t4src;     /* T4_FILE=<tiff>: a fax page's bits, not PRBS */

static int get_bit(void *ctx)
{
	int b;
	if (ntx >= maxbits)
		return sp_end_of_data();
	if (t4src) {
		b = sp_t4_bit(t4src);
		if (b < 0) {
			maxbits = ntx;
			return sp_end_of_data();
		}
		txb[ntx++] = b;
		return b;
	}
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

static int ulaw;

static int run(int bps, int short_train, FaxRxMemory *mem)
{
	void *tx;
	FaxRx rx;
	enum fax_mod mod;
	int st;
	long n, i, off, best = -1000, err = 64;
	short amp[160];

	ntx = nrx = 0;
	prbs = 0x1234567;
	if (getenv("T4_FILE")) {
		maxbits = 2000000;
		t4src = sp_t4_tx(getenv("T4_FILE"), 0, 2,
				 getenv("T4_MIN") ? atoi(getenv("T4_MIN")) : 0);
	}
	fax_mod_from_code(bps == 14400 ? (short_train ? 146 : 145) :
			  bps == 12000 ? (short_train ? 122 : 121) :
			  bps == 9600 ? (short_train ? 98 : 97) :
			  (short_train ? 74 : 73), &mod, &st);
	tx = sp_v17_tx(bps, short_train, get_bit, NULL);
	if (rx.init(mod, short_train, 8000, put_bit, NULL, mem))
		return 2;
	/* silence, the signal, silence */
	for (n = 0; n < 8000L * 120 / 160; n++) {
		int k, len = n < 5 ? 0 : sp_v17_samples(tx, amp, 160);
		for (k = len; k < 160; k++)
			amp[k] = 0;
		for (k = 0; k < 160; k++) {
			int x = amp[k];
			if (ulaw)
				x = sp_ulaw(x);
			rx.sample(x);
		}
		if (len < 160 && n > 5 && !rx.carrier)
			break;
	}
	rx.save(mem);
	rx.release();
	sp_v17_free(tx);
	/* aligned on bits a little in (ALIGN_AT): the start may be lost */
	long at = getenv("ALIGN_AT") ? atol(getenv("ALIGN_AT")) : 0;
	for (off = -3000; off < 3000 && at + off + 256 <= nrx; off++) {
		long e = 0;
		if (at + off < 0)
			continue;
		for (i = 0; i < 256; i++)
			e += rxb[at + off + i] != txb[at + i];
		if (e < err) {
			err = e;
			best = off;
		}
	}
	if (best < -3000 || err >= 64) {
		printf("%d%s: sent %ld, received %ld, no alignment: FAIL\n", bps,
		       short_train ? " short" : "", ntx, nrx);
		return 1;
	}
	{
		long first = -1;
		for (i = 0, err = 0; i < ntx && best + i < nrx; i++) {
			if (best + i < 0)
				continue;
			if (rxb[best + i] != txb[i]) {
				err++;
				if (first < 0)
					first = i;
			}
		}
		if (err)
			printf("first error at bit %ld (offset %ld)\n", first, best);
	}
	printf("%d%s: sent %ld, compared %ld, errors %ld (BER %.1e): %s\n", bps,
	       short_train ? " short train" : "", ntx, i, err,
	       i ? (double)err / i : 1.0, err ? "FAIL" : "PASS");
	return err != 0;
}

int main(int argc, char **argv)
{
	int bps = argc > 1 ? atoi(argv[1]) : 14400, r;
	FaxRxMemory mem;

	maxbits = argc > 2 ? atol(argv[2]) : 200000;
	ulaw = argc > 3 ? atoi(argv[3]) : 1;
	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	txb = static_cast<unsigned char*>(malloc(maxbits > 2000000 ? maxbits : 2000000));
	rxb = static_cast<unsigned char*>(malloc(2 * (maxbits > 2000000 ? maxbits : 2000000)));
	memset(&mem, 0, sizeof(mem));
	r = run(bps, 0, &mem);
	r |= run(bps, 1, &mem);
	return r;
}
