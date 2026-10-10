/*
 *    fax_pair.c - two T.30 fax machines back to back: one sends a TIFF
 *    file, the other receives it into another
 *
 *    fax_pair <in.tif> <out.tif> [max bit/s] [snr dB]
 *
 *    The audio goes through G.711 (mu-law) both ways, with noise if an
 *    SNR is given.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t30.h"
#include "../fax.h"

/* the DSP's log, as the modem core has it */
unsigned int dsplibs_debug_level = 0;

int dsplibs_debug_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	return 0;
}

/* the FSK modem in dp_fsk.c links against the modem core */
int modem_dp_register(enum DP_ID id, struct dp_operations *op) { return 0; }
void modem_dp_deregister(enum DP_ID id, struct dp_operations *op) {}
int modem_get_sreg(struct modem *m, unsigned int num) { return 0; }
long modem_set_param(struct modem *m, unsigned name, int val) { return 0; }
long modem_get_param(struct modem *m, unsigned name) { return 0; }
int modem_get_bits(struct modem *m, int nbits, u8 *buf, int n) { return n; }
int modem_put_bits(struct modem *m, int nbits, u8 *buf, int n) { return n; }

struct doc {
	struct tiff_in in;
	struct tiff_out out;
	int pages;
};

static int next_page(void *ctx, struct t4_page *p)
{
	struct doc *d = ctx;
	return tiff_in_page(&d->in, p);
}

static void page_received(void *ctx, struct t4_page *p)
{
	struct doc *d = ctx;
	tiff_out_page(&d->out, p);
	d->pages++;
}

static short g711(short x)
{
	/* mu-law there and back */
	int s = x < 0, m = s ? -x : x, e, q;
	if (m > 32635)
		m = 32635;
	m += 0x84;
	for (e = 7; e > 0 && !(m & (0x4000 >> (7 - e))); e--)
		;
	q = (m >> (e + 3)) & 15;
	m = ((q << 3) + 0x84) << e;
	m -= 0x84;
	return (short)(s ? -m : m);
}

static double noise(void)
{
	double u = (rand() + 1.0) / (RAND_MAX + 2.0);
	double v = (rand() + 1.0) / (RAND_MAX + 2.0);
	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

int main(int argc, char **argv)
{
	struct doc tx = { 0 }, rx = { 0 };
	struct t30_config c = { 0 };
	struct t30 *a, *b;
	struct t30_status sa, sb;
	short ab[160] = { 0 }, ba[160] = { 0 }, oa[160], ob[160];
	double sigma = 0;
	long n;
	int i;

	if (argc < 3) {
		fprintf(stderr, "usage: fax_pair <in.tif> <out.tif> [bit/s] [snr]\n");
		return 2;
	}
	if (tiff_in_open(&tx.in, argv[1]) || tiff_out_open(&rx.out, argv[2])) {
		fprintf(stderr, "cannot open the files\n");
		return 2;
	}
	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	if (argc > 4)
		sigma = 3000 / pow(10, atof(argv[4]) / 20);   /* vs the signal */
	c.srate = 8000;
	c.max_bps = argc > 3 ? atoi(argv[3]) : 14400;
	c.sending = 1;
	c.ident = "+1 555 0100";
	c.next_page = next_page;
	c.ctx = &tx;
	a = t30_create(&c);
	c.sending = 0;
	c.ident = "+1 555 0199";
	c.next_page = NULL;
	c.page_received = page_received;
	c.ctx = &rx;
	b = t30_create(&c);

	for (n = 0; n < 8000L * 600; n += 160) {
		t30_process(a, ba, oa, 160);
		t30_process(b, ab, ob, 160);
		for (i = 0; i < 160; i++) {
			ab[i] = g711((short)(oa[i] + sigma * noise()));
			ba[i] = g711((short)(ob[i] + sigma * noise()));
		}
		t30_get_status(a, &sa);
		t30_get_status(b, &sb);
		if (sa.result != T30_RUNNING && sb.result != T30_RUNNING)
			break;
	}
	tiff_out_close(&rx.out);
	tiff_in_close(&tx.in);
	printf("sender: %s, %d pages at %d bit/s, remote '%s'\n",
	       t30_result_text(sa.result), sa.pages, sa.bps, sa.remote_ident);
	printf("receiver: %s, %d pages, remote '%s', %.1f s\n",
	       t30_result_text(sb.result), sb.pages, sb.remote_ident, n / 8000.0);
	t30_free(a);
	t30_free(b);
	return sa.result == T30_OK && sb.result == T30_OK ? 0 : 1;
}
