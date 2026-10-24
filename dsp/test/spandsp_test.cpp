/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    spandsp_test.cpp - spandsp (libspandsp) as Asterisk's SendFAX has it,
 *    into our receivers: its V.17 transmitter into our fax receiver at
 *    8000 Hz through G.711 mu-law, a long train (as for TCF), then a short
 *    train with its equalizer (a page), PRBS bits or a fax page's; and its
 *    T.4 encoder into our line receiver (t4_rx_bit), the rows the same.
 *    spandsp's side is in spandsp_tx.cpp (its headers clash with ours);
 *    our T.30 against spandsp's is t30_spandsp.cpp.
 *
 *    DSP_DEBUG=<level>   the DSP's log
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "../fax.h"
#include "fax_util.h"
#include "t4.h"

using namespace dsp;

/* spandsp_tx.cpp */
void *sp_v17_tx(int bps, int short_train, int (*get_bit)(void *), void *ctx);
int sp_v17_samples(void *s, short *amp, int n);
void sp_v17_free(void *s);
int sp_ulaw(int x);
int sp_end_of_data(void);
void *sp_t4_tx(const char *file, int page, int encoding, int min_bits);
int sp_t4_bit(void *s);
void sp_t4_free(void *s);

/*
 *    V.17
 *
 */

struct V17 {
	int bps;
	int t4_min;     /* a fax page's bits (that many bits a row at
			   least), -1: PRBS */
	int seed = 1;   /* the page's */
};

class SpandspV17 : public testing::TestWithParam<V17> {
protected:
	unsigned prbs = 0x1234567;
	std::vector<unsigned char> txb, rxb;
	long ntx = 0, nrx = 0, maxbits = 200000;
	void *t4src = nullptr;
	std::string page;

	/* the page's encoder freed */
	~SpandspV17() override { drop_t4(); }
	/* the page's encoder freed */
	void drop_t4()
	{
		if (t4src)
			sp_t4_free(t4src);
		t4src = nullptr;
	}

	/* spandsp's next bit to send: the page's (sp_t4_bit) or PRBS 2^23-1,
	   kept in txb; end of data after maxbits or the page */
	static int get_bit(void *ctx) {
		SpandspV17 *t = static_cast<SpandspV17 *>(ctx);
		int b;
		if (t->ntx >= t->maxbits)
			return sp_end_of_data();
		if (t->t4src) {
			b = sp_t4_bit(t->t4src);
			if (b < 0) {
				t->maxbits = t->ntx;
				return sp_end_of_data();
			}
		} else {
			b = ((t->prbs >> 22) ^ (t->prbs >> 17)) & 1;
			t->prbs = (t->prbs << 1) | b;
		}
		t->txb[t->ntx++] = b;
		return b;
	}
	/* a bit from our receiver into rxb */
	static void put_bit(void *ctx, int bit) {
		SpandspV17 *t = static_cast<SpandspV17 *>(ctx);
		if (t->nrx < (long)t->rxb.size())
			t->rxb[t->nrx++] = bit;
	}
	testing::AssertionResult run(int short_train, FaxRxMemory *mem);
};

/* one transmission: spandsp's V.17 signal (after 5 frames of silence)
   through mu-law into our receiver until its carrier goes (120 s at
   most); mem carries the equalizer from the long train to the short
   one.  The offset of the received bits with the fewest errors in the
   first 256 sent; then no bit may differ at it */
testing::AssertionResult SpandspV17::run(int short_train, FaxRxMemory *mem)
{
	int bps = GetParam().bps, st;
	const char *train = short_train ? "short train: " : "long train: ";
	void *tx;
	FaxRx rx;
	enum fax_mod mod;
	long n, i, off, best = -1000, err = 64;
	short amp[160];

	ntx = nrx = 0;
	prbs = 0x1234567;
	drop_t4();
	if (GetParam().t4_min >= 0) {
		maxbits = 2000000;
		t4src = sp_t4_tx(page.c_str(), 0, 2, GetParam().t4_min);
		if (!t4src)
			return testing::AssertionFailure() << "spandsp cannot read " << page;
	}
	txb.assign(maxbits, 0);
	rxb.assign(2 * maxbits, 0);
	fax_mod_from_code(bps == 14400 ? (short_train ? 146 : 145) :
			  bps == 12000 ? (short_train ? 122 : 121) :
			  bps == 9600 ? (short_train ? 98 : 97) :
			  (short_train ? 74 : 73), &mod, &st);
	tx = sp_v17_tx(bps, short_train, get_bit, this);
	if (!tx)
		return testing::AssertionFailure() << train << "no spandsp "
						   "V.17 transmitter at " << bps;
	if (rx.init(mod, short_train, 8000, put_bit, this, mem)) {
		sp_v17_free(tx);
		return testing::AssertionFailure() << train << "receiver init";
	}
	/* silence, the signal, silence */
	for (n = 0; n < 8000L * 120 / 160; n++) {
		int k, len = n < 5 ? 0 : sp_v17_samples(tx, amp, 160);
		for (k = len; k < 160; k++)
			amp[k] = 0;
		for (k = 0; k < 160; k++)
			rx.sample(sp_ulaw(amp[k]));
		if (len < 160 && n > 5 && !rx.carrier)
			break;
	}
	rx.save(mem);
	rx.release();
	sp_v17_free(tx);
	for (off = -3000; off < 3000 && off + 256 <= nrx; off++) {
		long e = 0;
		if (off < 0)
			continue;
		for (i = 0; i < 256; i++)
			e += rxb[off + i] != txb[i];
		if (e < err) {
			err = e;
			best = off;
		}
	}
	if (best < -3000 || err >= 64)
		return testing::AssertionFailure() << train << "sent " << ntx <<
			", received " << nrx << ", no alignment";
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
	/* (also when the receiver stopped short of the end) */
	if (err || i < ntx - 64)
		return testing::AssertionFailure() << train << "sent " << ntx <<
			", compared " << i << ", errors " << err <<
			" (the first at bit " << first << ", offset " << best << ")";
	return testing::AssertionSuccess();
}

/* the long train, then the short one with what the long one left */
TEST_P(SpandspV17, IntoOurReceiver)
{
	FaxRxMemory mem;

	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
	if (GetParam().t4_min >= 0) {
		page = fax_file("page.tif");
		ASSERT_TRUE(write_test_doc(page, true, false,
					   GetParam().seed));
	}
	memset(&mem, 0, sizeof(mem));
	EXPECT_TRUE(run(0, &mem));
	EXPECT_TRUE(run(1, &mem));
}

INSTANTIATE_TEST_SUITE_P(Rates, SpandspV17, testing::Values(
	V17{ 14400, -1 }, V17{ 12000, -1 }, V17{ 9600, -1 }, V17{ 7200, -1 },
	/* a page, without and with fill bits */
	V17{ 14400, 0 }, V17{ 14400, 144 },
	/* pages that once lost a row: their runs of the same few patterns
	   moved the AGC and the timing loop */
	V17{ 14400, 0, 5 }, V17{ 14400, 0, 11 }, V17{ 14400, 0, 25 },
	V17{ 14400, 144, 16 }),
	/* <bps>, and _page_min<bits> for a page (_seed<n> if not 1) */
	[](const testing::TestParamInfo<V17> &i) {
		std::string n = std::to_string(i.param.bps);
		if (i.param.t4_min >= 0)
			n += "_page_min" + std::to_string(i.param.t4_min);
		if (i.param.seed != 1)
			n += "_seed" + std::to_string(i.param.seed);
		return n;
	});

/*
 *    T.4
 *
 */

struct T4Coding {
	int encoding;   /* 1 MH, 2 MR */
	int min_bits;   /* a row */
};

class SpandspT4 : public testing::TestWithParam<T4Coding> {};

/* the light document's first page through spandsp's T.4 encoder (MH
   or MR, at least min_bits a row) into T4Rx: the same rows */
TEST_P(SpandspT4, IntoOurLineReceiver)
{
	std::string doc = fax_file("doc.tif");
	TiffIn ti;
	T4Page ref, got;
	T4Rx rx;
	int enc = GetParam().encoding, b, y, diff = 0, first = -1;
	void *s;

	ASSERT_TRUE(write_test_doc(doc, true));
	ASSERT_EQ(ti.open(doc.c_str()), 0);
	ASSERT_EQ(ti.read_page(&ref), 1);
	s = sp_t4_tx(doc.c_str(), 0, enc, GetParam().min_bits);
	ASSERT_NE(s, nullptr);
	std::unique_ptr<void, void (*)(void *)> s_free(s, sp_t4_free);
	got.init(ref.width, ref.fine);
	rx.init(&got, enc == 2);
	while ((b = sp_t4_bit(s)) >= 0)
		if (rx.bit(b))
			break;
	rx.end();
	for (y = 0; y < ref.rows && y < got.rows; y++)
		if (memcmp(ref.bits.data() + (size_t)y * 216, got.bits.data() + (size_t)y * 216, 216)) {
			if (first < 0)
				first = y;
			diff++;
		}
	EXPECT_EQ(got.rows, ref.rows);
	EXPECT_EQ(diff, 0) << "the first at row " << first;
	rx.release();
	got.release();
	ref.release();
	ti.close();
}

INSTANTIATE_TEST_SUITE_P(Codings, SpandspT4, testing::Values(
	T4Coding{ 1, 0 }, T4Coding{ 1, 144 }, T4Coding{ 2, 0 },
	T4Coding{ 2, 144 }),
	/* mh or mr, and _min<bits> */
	[](const testing::TestParamInfo<T4Coding> &i) {
		return std::string(i.param.encoding == 1 ? "mh" : "mr") +
			"_min" + std::to_string(i.param.min_bits);
	});
