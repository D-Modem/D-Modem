/*
 *    t30_spandsp.cpp - our T.30 fax machine against spandsp's, back to
 *    back in one process
 *
 *    t30_spandsp to|from <in.tif> <out.tif> [ecm] [hits a minute]
 *
 *    to: spandsp sends in.tif, we receive into out.tif; from: we send,
 *    spandsp receives.  ecm 1: both offer error correction mode (the
 *    call has it), 0: neither.  The audio goes through G.711 (mu-law)
 *    both ways; hits: that many line hits a minute on average, 5 ms of
 *    loud noise each (spandsp takes a steady noise floor much above -50
 *    dBm0 for a carrier, so no noise in between).  The exit status is 0 when
 *    both machines say the document went through and the call had ECM
 *    as asked; compare the files with t4_test.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spandsp.h>

#include "dsp.h"
#include "t30.h"

using namespace dsp;

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

/* the FSK modem in dp_fsk.cpp links against the modem core */
int modem_dp_register(enum DP_ID id, struct dp_operations *op) { return 0; }
void modem_dp_deregister(enum DP_ID id, struct dp_operations *op) {}
int modem_get_sreg(struct modem *m, unsigned int num) { return 0; }
long modem_set_param(struct modem *m, unsigned name, int val) { return 0; }
long modem_get_param(struct modem *m, unsigned name) { return 0; }
int modem_get_bits(struct modem *m, int nbits, u8 *buf, int n) { return n; }
int modem_put_bits(struct modem *m, int nbits, u8 *buf, int n) { return n; }

struct doc {
	TiffIn in;
	TiffOut out;
};

static int next_page(void *ctx, T4Page *p)
{
	return static_cast<doc*>(ctx)->in.read_page(p);
}

static void page_received(void *ctx, T4Page *p)
{
	static_cast<doc*>(ctx)->out.write_page(p);
}

static int sp_done = -1;

static void phase_e(t30_state_t *s, void *user, int result)
{
	(void)s;
	(void)user;
	sp_done = result;
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
	struct doc d;
	struct t30_config c = { 0 };
	struct t30_status st;
	t30_stats_t sps;
	fax_state_t *fax;
	t30_state_t *t30;
	T30 *ours;
	short ab[160] = { 0 }, ba[160] = { 0 }, oa[160];
	int16_t sp_in[160], sp_out[160];
	int to, ecm, i, k, ok;
	double hits = 0;
	long n, hit = 0;

	if (argc < 4 || (strcmp(argv[1], "to") && strcmp(argv[1], "from"))) {
		fprintf(stderr, "usage: t30_spandsp to|from <in.tif> <out.tif> [ecm] [snr]\n");
		return 2;
	}
	to = !strcmp(argv[1], "to");
	ecm = argc > 4 ? atoi(argv[4]) : 1;
	if (argc > 5)
		hits = atof(argv[5]);
	srand(1);
	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	memset(&d, 0, sizeof(d));
	if (!to && d.in.open(argv[2])) {
		fprintf(stderr, "cannot read %s\n", argv[2]);
		return 2;
	}
	if (to && d.out.open(argv[3])) {
		fprintf(stderr, "cannot write %s\n", argv[3]);
		return 2;
	}

	/* spandsp: the caller when it sends */
	fax = fax_init(NULL, to);
	t30 = fax_get_t30_state(fax);
	fax_set_transmit_on_idle(fax, 1);
	t30_set_tx_ident(t30, "+1 555 0177");
	t30_set_ecm_capability(t30, ecm);
	t30_set_supported_modems(t30, T30_SUPPORT_V27TER | T30_SUPPORT_V29 |
				 T30_SUPPORT_V17);
	t30_set_supported_compressions(t30, T30_SUPPORT_T4_1D_COMPRESSION |
				       T30_SUPPORT_T4_2D_COMPRESSION);
	t30_set_phase_e_handler(t30, phase_e, NULL);
	if (to)
		t30_set_tx_file(t30, argv[2], -1, -1);
	else
		t30_set_rx_file(t30, argv[3], -1);
	if (getenv("SPANDSP_DEBUG")) {
		span_log_set_level(fax_get_logging_state(fax),
				   SPAN_LOG_SHOW_TAG | SPAN_LOG_FLOW);
		span_log_set_level(t30_get_logging_state(t30),
				   SPAN_LOG_SHOW_TAG | SPAN_LOG_FLOW);
	}

	/* ours */
	c.srate = 8000;
	c.max_bps = 14400;
	c.ecm = ecm;
	c.sending = !to;
	c.ident = "+1 555 0199";
	c.next_page = next_page;
	c.page_received = page_received;
	c.ctx = &d;
	ours = t30_create(&c);

	for (n = 0; n < 8000L * 600; n += 160) {
		/* spandsp gets what we sent last, and the other way round */
		for (i = 0; i < 160; i++)
			sp_in[i] = ab[i];
		fax_rx(fax, sp_in, 160);
		k = fax_tx(fax, sp_out, 160);
		for (i = k; i < 160; i++)
			sp_out[i] = 0;
		t30_process(ours, ba, oa, 160);
		for (i = 0; i < 160; i++) {
			double x = 0;
			if (hit > 0) {
				hit--;
				x = 3000 * noise();
			} else if (hits > 0 &&
				   rand() < RAND_MAX / (8000 * 60 / hits)) {
				hit = 40;
			}
			ba[i] = g711((short)(sp_out[i] + x));
			ab[i] = g711((short)(oa[i] + x));
		}
		t30_get_status(ours, &st);
		if (st.result != T30_RUNNING && sp_done >= 0)
			break;
		/* spandsp ends with the call: once we are done, it hangs up */
		if (st.result != T30_RUNNING && n % 8000 == 0 &&
		    sp_done < 0 && st.phase && !strcmp(st.phase, "done"))
			t30_terminate(t30);
	}
	t30_get_transfer_statistics(t30, &sps);
	t30_get_status(ours, &st);
	printf("spandsp: %s, %d pages, %d bit/s%s\n",
	       sp_done >= 0 ? t30_completion_code_to_str(sp_done) : "running",
	       to ? sps.pages_tx : sps.pages_rx, sps.bit_rate,
	       sps.error_correcting_mode ? " (ECM)" : "");
	printf("ours: %s, %d pages at %d bit/s%s, remote '%s', %.1f s\n",
	       t30_result_text(st.result), st.pages, st.bps,
	       st.ecm ? " (ECM)" : "", st.remote_ident, n / 8000.0);
	ok = st.result == T30_OK && sp_done == T30_ERR_OK && st.ecm == !!ecm;
	t30_free(ours);
	fax_free(fax);
	if (to)
		d.out.close();
	else
		d.in.close();
	return ok ? 0 : 1;
}
