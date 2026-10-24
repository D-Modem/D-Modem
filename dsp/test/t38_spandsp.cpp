/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    t38_spandsp.cpp - our T.30 machine over T.38 against spandsp's T.38
 *    terminal (t38_terminal_state_t, as Asterisk's SendFAX and ReceiveFAX
 *    use it), back to back in one process: both ways, with and without
 *    error correction mode, T.38 versions 0 and 2
 *
 *    The IFP packets go as they are, one 20 ms tick late, each once.  Both
 *    machines must say the document went through, with ECM as asked, and
 *    the pages must be the same.
 *
 *    SPANDSP_DEBUG=1, DSP_DEBUG=<level>   their logs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <vector>

#include <spandsp.h>

#include "dsp.h"
#include "fax_util.h"
#include "t30.h"
#include "t38.h"

using namespace dsp;

typedef std::vector<unsigned char> Bytes;

struct Ours {
	TiffIn in;
	TiffOut out;
	int version;
	std::deque<Bytes> to_sp;        /* our packets, for spandsp */
};

/* our machine's next page, from our document: 1, 0 when there are no
   more, -1 on an error */
static s16 ours_next_page(void *ctx, T4Page *p)
{
	return static_cast<Ours *>(ctx)->in.read_page(p);
}

/* a page our machine received, written to our TIFF */
static void ours_page_received(void *ctx, T4Page *p)
{
	static_cast<Ours *>(ctx)->out.write_page(p);
}

/* one of our IFP packets, encoded, on its way to spandsp */
static void ours_tx(void *ctx, const struct t38_ifp *p)
{
	Ours *o = static_cast<Ours *>(ctx);
	unsigned char buf[UDPTL_IFP_MAX];
	int n = t38_ifp_encode(p, o->version, buf, sizeof(buf));
	ASSERT_GT(n, 0);
	o->to_sp.push_back(Bytes(buf, buf + n));
}

/* spandsp's packets, on their way to us (each once, whatever its count) */
static int sp_tx(t38_core_state_t *s, void *user, const uint8_t *buf, int len,
		 int count)
{
	(void)s;
	(void)count;
	static_cast<std::deque<Bytes> *>(user)->push_back(Bytes(buf, buf + len));
	return 0;
}

static int sp38_done = -1;

/* spandsp's phase E: its completion code */
static void sp38_phase_e(t30_state_t *s, void *user, int result)
{
	(void)s;
	(void)user;
	sp38_done = result;
}

struct Sp38 {
	bool to;        /* spandsp sends, we receive; else the other way */
	bool ecm;
	int version;
};

/* c, for the test's name: the direction, ECM and the version */
static void PrintTo(const Sp38 &c, std::ostream *os)
{
	*os << (c.to ? "to" : "from") << (c.ecm ? " ecm" : "") << " v"
	    << c.version;
}

class T38Spandsp : public testing::TestWithParam<Sp38> {};

/* one call (10 minutes at most): spandsp told to end once ours is done;
   both OK, ECM as asked, two pages, the same as sent */
TEST_P(T38Spandsp, SendsTheDocument)
{
	const Sp38 &call = GetParam();
	std::string in = fax_file("in.tif"), out = fax_file("out.tif");
	std::deque<Bytes> to_us;
	Ours o;
	struct t30_config c = {};
	struct t30_status st = {};
	t30_stats_t sps;
	t38_terminal_state_t *sp;
	t38_core_state_t *core;
	t30_state_t *t30;
	T30 *ours;
	unsigned sp_seq = 0;
	long n;

	ASSERT_TRUE(write_test_doc(in, true));
	sp38_done = -1;
	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	o.version = call.version;
	if (!call.to)
		ASSERT_EQ(o.in.open(in.c_str()), 0) << in;
	else
		ASSERT_EQ(o.out.open(out.c_str()), 0) << out;

	/* spandsp: the caller when it sends */
	sp = t38_terminal_init(NULL, call.to, sp_tx, &to_us);
	ASSERT_NE(sp, nullptr);
	core = t38_terminal_get_t38_core_state(sp);
	t38_set_t38_version(core, call.version);
	t30 = t38_terminal_get_t30_state(sp);
	t30_set_tx_ident(t30, "+1 555 0177");
	t30_set_ecm_capability(t30, call.ecm);
	t30_set_supported_modems(t30, T30_SUPPORT_V27TER | T30_SUPPORT_V29 |
				 T30_SUPPORT_V17);
	t30_set_supported_compressions(t30, T30_SUPPORT_T4_1D_COMPRESSION |
				       T30_SUPPORT_T4_2D_COMPRESSION);
	t30_set_phase_e_handler(t30, sp38_phase_e, NULL);
	if (call.to)
		t30_set_tx_file(t30, in.c_str(), -1, -1);
	else
		t30_set_rx_file(t30, out.c_str(), -1);
	if (getenv("SPANDSP_DEBUG")) {
		span_log_set_level(t38_terminal_get_logging_state(sp),
				   SPAN_LOG_SHOW_TAG | SPAN_LOG_FLOW);
		span_log_set_level(t38_core_get_logging_state(core),
				   SPAN_LOG_SHOW_TAG | SPAN_LOG_FLOW);
		span_log_set_level(t30_get_logging_state(t30),
				   SPAN_LOG_SHOW_TAG | SPAN_LOG_FLOW);
	}

	/* ours */
	c.srate = 8000;
	c.max_bps = 14400;
	c.ecm = call.ecm;
	c.sending = !call.to;
	c.ident = "+1 555 0199";
	c.next_page = ours_next_page;
	c.page_received = ours_page_received;
	c.ctx = &o;
	c.t38 = 1;
	c.t38_tx = ours_tx;
	ours = t30_create(&c);

	for (n = 0; n < 8000L * 600; n += 160) {
		std::deque<Bytes> to_sp, mine;
		to_sp.swap(o.to_sp);
		mine.swap(to_us);
		for (const Bytes &p : to_sp)
			t38_core_rx_ifp_packet(core, p.data(), (int)p.size(),
					       (uint16_t)sp_seq++);
		for (const Bytes &p : mine) {
			struct t38_ifp ifp;
			ASSERT_EQ(t38_ifp_decode(&ifp, call.version, p.data(),
						 (int)p.size()), 0);
			t30_t38_rx(ours, &ifp);
		}
		t38_terminal_send_timeout(sp, 160);
		t30_t38_time(ours, 160);
		t30_get_status(ours, &st);
		if (st.done && sp38_done >= 0)
			break;
		/* spandsp ends with the call: once we are done, it hangs
		   up */
		if (st.done && n % 8000 == 0 && sp38_done < 0)
			t30_terminate(t30);
	}
	t30_get_transfer_statistics(t30, &sps);
	t30_get_status(ours, &st);
	printf("spandsp: %s, %d pages, %d bit/s%s\n",
	       sp38_done >= 0 ? t30_completion_code_to_str(sp38_done) :
	       "running", call.to ? sps.pages_tx : sps.pages_rx, sps.bit_rate,
	       sps.error_correcting_mode ? " (ECM)" : "");
	printf("ours: %s, %d pages at %d bit/s%s, %.1f s\n",
	       t30_result_text(st.result), st.pages, st.bps,
	       st.ecm ? " (ECM)" : "", n / 8000.0);
	EXPECT_EQ(st.result, T30_OK) << "ours: " << t30_result_text(st.result);
	EXPECT_EQ(sp38_done, T30_ERR_OK) << "spandsp: " <<
		(sp38_done >= 0 ? t30_completion_code_to_str(sp38_done) :
		 "running");
	EXPECT_EQ(!!st.ecm, call.ecm);
	EXPECT_EQ(st.pages, 2);
	EXPECT_STREQ(st.remote_ident, "+1 555 0177");
	t30_free(ours);
	t38_terminal_free(sp);
	if (call.to)
		o.out.close();
	else
		o.in.close();
	EXPECT_TRUE(same_pages(in, out));
}

INSTANTIATE_TEST_SUITE_P(Calls, T38Spandsp, testing::Values(
	Sp38{ true, false, 0 }, Sp38{ true, true, 0 }, Sp38{ true, true, 2 },
	Sp38{ false, false, 0 }, Sp38{ false, true, 0 },
	Sp38{ false, false, 2 }),
	/* to or from (spandsp sending or receiving), _ecm, _v<version> */
	[](const testing::TestParamInfo<Sp38> &i) {
		return std::string(i.param.to ? "to" : "from") +
			(i.param.ecm ? "_ecm" : "") + "_v" +
			std::to_string(i.param.version);
	});
