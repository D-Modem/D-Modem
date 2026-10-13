/*
 *    t30_spandsp.cpp - our T.30 fax machine against spandsp's, back to
 *    back in one process, both ways, with and without error correction
 *    mode, ECM also with line hits
 *
 *    The audio goes through G.711 (mu-law) both ways; hits: that many
 *    line hits a minute on average, 5 ms of loud noise each (spandsp
 *    takes a steady noise floor much above -50 dBm0 for a carrier, so no
 *    noise in between).  Both machines must say the document went
 *    through, the call must have ECM as asked, and the pages must be the
 *    same.
 *
 *    SPANDSP_DEBUG=1, DSP_DEBUG=<level>   their logs
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spandsp.h>

#include "dsp.h"
#include "fax_util.h"
#include "t30.h"

using namespace dsp;

struct doc {
	TiffIn in;
	TiffOut out;
};

/* our sender's next page: read_page's result */
static int next_page(void *ctx, T4Page *p)
{
	return static_cast<doc*>(ctx)->in.read_page(p);
}

/* a page our receiver got, into the TIFF */
static void page_received(void *ctx, T4Page *p)
{
	static_cast<doc*>(ctx)->out.write_page(p);
}

static int sp_done = -1;

/* spandsp's phase E: its completion code into sp_done */
static void phase_e(t30_state_t *s, void *user, int result)
{
	(void)s;
	(void)user;
	sp_done = result;
}

struct Call {
	bool to;        /* spandsp sends, we receive; else the other way */
	bool ecm;       /* both offer error correction mode */
	double hits;    /* line hits a minute */
};

class T30Spandsp : public testing::TestWithParam<Call> {};

/* one call (10 minutes of line at most): spandsp's machine and ours
   through mu-law both ways, the hits on both; spandsp told to end once
   ours is done; both must say OK, with ECM as asked, two pages, the
   same as sent */
TEST_P(T30Spandsp, SendsTheDocument)
{
	const Call &call = GetParam();
	std::string in = fax_file("in.tif"), out = fax_file("out.tif");
	struct doc d;
	struct t30_config c = {};
	struct t30_status st = {};
	t30_stats_t sps;
	fax_state_t *fax;
	t30_state_t *t30;
	T30 *ours;
	short ab[160] = { 0 }, ba[160] = { 0 }, oa[160];
	int16_t sp_in[160], sp_out[160];
	int to = call.to, ecm = call.ecm, i, k;
	double hits = call.hits;
	long n, hit = 0;

	ASSERT_TRUE(write_test_doc(in, true));
	sp_done = -1;   /* (not the case before's) */
	srand(1);
	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	if (!to) {
		ASSERT_EQ(d.in.open(in.c_str()), 0) << in;
	} else {
		ASSERT_EQ(d.out.open(out.c_str()), 0) << out;
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
		t30_set_tx_file(t30, in.c_str(), -1, -1);
	else
		t30_set_rx_file(t30, out.c_str(), -1);
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
				x = 3000 * gauss();
			} else if (hits > 0 &&
				   rand() < RAND_MAX / (8000 * 60 / hits)) {
				hit = 40;
			}
			ba[i] = g711_ulaw((short)(sp_out[i] + x));
			ab[i] = g711_ulaw((short)(oa[i] + x));
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
	printf("ours: %s, %d pages at %d bit/s%s, %.1f s\n",
	       t30_result_text(st.result), st.pages, st.bps,
	       st.ecm ? " (ECM)" : "", n / 8000.0);
	EXPECT_EQ(st.result, T30_OK) << "ours: " << t30_result_text(st.result);
	EXPECT_EQ(sp_done, T30_ERR_OK) << "spandsp: " <<
		(sp_done >= 0 ? t30_completion_code_to_str(sp_done) : "running");
	EXPECT_EQ(!!st.ecm, !!ecm);
	EXPECT_EQ(st.pages, 2);
	t30_free(ours);
	fax_free(fax);
	if (to)
		d.out.close();
	else
		d.in.close();
	EXPECT_TRUE(same_pages(in, out));
}

INSTANTIATE_TEST_SUITE_P(Calls, T30Spandsp, testing::Values(
	Call{ true, false, 0 }, Call{ true, true, 0 }, Call{ true, true, 30 },
	Call{ false, false, 0 }, Call{ false, true, 0 },
	Call{ false, true, 30 }),
	/* to or from (spandsp sending or receiving), _ecm, _hits */
	[](const testing::TestParamInfo<Call> &i) {
		return std::string(i.param.to ? "to" : "from") +
			(i.param.ecm ? "_ecm" : "") +
			(i.param.hits ? "_hits" : "");
	});
