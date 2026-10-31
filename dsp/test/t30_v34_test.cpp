/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t30_v34_test.cpp - two T.30 machines back to back on V.34
 *    half-duplex (T.30 Annex F): V.8, the frames on the control channel,
 *    the ECM blocks on the primary channel; and with a machine without
 *    it, either side, V.21 and V.17 as before
 *
 *    DSP_DEBUG=<level>              the DSP's log
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "dm_types.h"
#include "dsplibs.h"
#include "fax_util.h"
#include "modem_dp.h"
#include "modem_param.h"
#include "t30.h"
#include "t4.h"

using namespace dsp;

/* the modem core the pumps link against: nothing registered, S
   registers and parameters 0, the bits all taken and given */
int modem_dp_register(enum DP_ID, struct dp_operations *) { return 0; }
/* (nothing to deregister) */
void modem_dp_deregister(enum DP_ID, struct dp_operations *) {}
/* an S register: 0 */
int modem_get_sreg(struct modem *, unsigned int) { return 0; }
/* a parameter set: 0 */
long modem_set_param(struct modem *, unsigned, int) { return 0; }
/* a parameter: 0 (none) */
intptr_t modem_get_param(struct modem *, unsigned) { return 0; }
/* the bits asked for: n */
int modem_get_bits(struct modem *, int, u8 *, int n) { return n; }
/* the bits given: n */
int modem_put_bits(struct modem *, int, u8 *, int n) { return n; }
/* no DTE */
int modem_send_to_tty(struct modem *, const char *, int n) { return n; }
/* (nothing from it) */
int modem_recv_from_tty(struct modem *, char *, int) { return 0; }

namespace {

/* DSP_DEBUG=<level>: the DSP's log to stderr */
const int debug_level = [] {
	const char *e = getenv("DSP_DEBUG");
	dsplibs_debug_level = e ? static_cast<unsigned>(atoi(e)) : 0;
	return 0;
}();

struct Doc {
	TiffIn in;
	TiffOut out;
};

/* the sender's next page (t30_config next_page): read_page's result */
s16 next_page(void *ctx, T4Page *p)
{
	return static_cast<Doc *>(ctx)->in.read_page(p);
}

/* a page received, into the receiver's TIFF */
void page_received(void *ctx, T4Page *p)
{
	static_cast<Doc *>(ctx)->out.write_page(p);
}

/* what came of a call */
struct Result {
	t30_status tx, rx;
	double secs;
};

/* a document of two pages from the caller to the answerer through
   G.711 with a little noise, V.34 offered by the caller, the answerer or
   both: both end OK with the pages the same */
Result send_document(bool v34_caller, bool v34_answerer)
{
	Result r = {};
	std::string in = fax_file("in.tif"), out = fax_file("out.tif");
	Doc tx, rx;
	struct t30_config c = {};
	short ab[160] = { 0 }, ba[160] = { 0 }, oa[160], ob[160];
	double sigma = 3000 / pow(10, 40 / 20.0);
	long n;

	EXPECT_TRUE(write_test_doc(in, false));
	EXPECT_EQ(tx.in.open(in.c_str()), 0);
	EXPECT_EQ(rx.out.open(out.c_str()), 0);
	c.srate = 8000;
	c.max_bps = 14400;
	c.ecm = 1;
	c.sending = 1;
	c.ident = "+1 555 0100";
	c.next_page = next_page;
	c.ctx = &tx;
	c.v34 = v34_caller ? t30_v34() : nullptr;
	T30 *a = t30_create(&c);
	c.sending = 0;
	c.ident = "+1 555 0199";
	c.next_page = nullptr;
	c.page_received = page_received;
	c.ctx = &rx;
	c.v34 = v34_answerer ? t30_v34() : nullptr;
	T30 *b = t30_create(&c);

	for (n = 0; n < 8000L * 300; n += 160) {
		t30_process(a, ba, oa, 160);
		t30_process(b, ab, ob, 160);
		for (int i = 0; i < 160; i++) {
			ab[i] = g711_ulaw((short)(oa[i] + sigma * gauss()));
			ba[i] = g711_ulaw((short)(ob[i] + sigma * gauss()));
		}
		t30_get_status(a, &r.tx);
		t30_get_status(b, &r.rx);
		if (r.tx.done && r.rx.done)
			break;
	}
	rx.out.close();
	tx.in.close();
	r.secs = n / 8000.0;
	EXPECT_EQ(r.tx.result, T30_OK) << "sender: " <<
		t30_result_text(r.tx.result);
	EXPECT_EQ(r.rx.result, T30_OK) << "receiver: " <<
		t30_result_text(r.rx.result);
	EXPECT_EQ(r.tx.pages, 2);
	EXPECT_EQ(r.rx.pages, 2);
	EXPECT_STREQ(r.tx.remote_ident, "+1 555 0199");
	EXPECT_STREQ(r.rx.remote_ident, "+1 555 0100");
	printf("%d pages at %d bit/s%s%s in %.1f s\n", r.tx.pages, r.tx.bps,
	       r.tx.ecm ? " (ECM)" : "", r.tx.v34 ? " on V.34" : "", r.secs);
	t30_free(a);
	t30_free(b);
	EXPECT_TRUE(same_pages(in, out));
	return r;
}

/* both have it: the pages on V.34's primary channel */
TEST(T30V34, BothSendOnV34)
{
	Result r = send_document(true, true);
	EXPECT_TRUE(r.tx.v34);
	EXPECT_TRUE(r.rx.v34);
	EXPECT_GE(r.tx.bps, 21600);
	EXPECT_EQ(r.tx.bps, r.rx.bps);
}

/* the answerer has no V.8 (CED, V.21's DIS): V.17 */
TEST(T30V34, AnswererWithoutIt)
{
	Result r = send_document(true, false);
	EXPECT_FALSE(r.tx.v34);
	EXPECT_EQ(r.tx.bps, 14400);
}

/* the caller has none (CNG, no CM): after ANSam, V.21's DIS and V.17 */
TEST(T30V34, CallerWithoutIt)
{
	Result r = send_document(false, true);
	EXPECT_FALSE(r.rx.v34);
	EXPECT_EQ(r.rx.bps, 14400);
}

} // namespace
