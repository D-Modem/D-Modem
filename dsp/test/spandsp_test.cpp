/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
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
#include <random>
#include <string>
#include <vector>

#include "../fax.h"
#include "../dsp.h"
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
int sp_t4_rx(const char *file, int encoding, int width, int fine, const unsigned char *bits, size_t n);
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
	static void put_bit(void *ctx, s16 bit) {
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
	int bps = GetParam().bps;
	s16 st;
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

/* the light document's first page through our MR (K 2 and 4) and MMR
   encoders into spandsp's T.4 receiver: the same rows */
TEST_P(SpandspT4, OurEncoderIntoSpandsp)
{
	std::string doc = fax_file("doc.tif"), out = fax_file("sp_rx.tif");
	TiffIn ti, back;
	T4Page ref, got;
	T4Bits bits;
	int enc = GetParam().encoding;

	if (enc == 1)
		GTEST_SKIP() << "MH: the other tests";
	ASSERT_TRUE(write_test_doc(doc, true));
	ASSERT_EQ(ti.open(doc.c_str()), 0);
	ASSERT_EQ(ti.read_page(&ref), 1);
	for (int k : {2, 4, 0}) {
		bits.release();
		if (k)
			ref.encode_mr(&bits, (s16)k, (s16)GetParam().min_bits, 0, 1);
		else
			ref.encode_mmr(&bits);
		int rows = sp_t4_rx(out.c_str(), k ? 2 : 3, ref.width, ref.fine,
				    bits.buf.data(), bits.n);
		EXPECT_EQ(rows, ref.rows) << "k " << k;
		ASSERT_EQ(back.open(out.c_str()), 0);
		ASSERT_EQ(back.read_page(&got), 1);
		int diff = 0, first = -1;
		for (int y = 0; y < ref.rows && y < got.rows; y++)
			if (memcmp(ref.bits.data() + (size_t)y * 216, got.bits.data() + (size_t)y * 216, 216)) {
				if (first < 0)
					first = y;
				diff++;
			}
		EXPECT_EQ(got.rows, ref.rows) << "k " << k;
		EXPECT_EQ(diff, 0) << "k " << k << ", the first at row " << first;
		got.release();
		back.close();
	}
	bits.release();
	ref.release();
	ti.close();
}

/* random pages (runs of every length, rows like the row above and
   not) through our MR and MMR encoders into spandsp's receiver: the
   same rows */
TEST(SpandspT4Random, OurEncoderIntoSpandsp)
{
	std::string out = fax_file("sp_rand.tif");
	std::mt19937 rng(7);
	for (int seed = 0; seed < 20; seed++) {
		T4Page ref;
		ref.init(T4_WIDTH, seed & 1);
		std::vector<unsigned char> row(216, 0);
		for (int y = 0; y < 200; y++) {
			int kind = rng() % 4;
			if (kind == 0) {
				/* a few pels changed from the row above */
				for (int k = 0; k < 6; k++) {
					int x = rng() % T4_WIDTH;
					row[x / 8] ^= (unsigned char)(0x80 >> (x % 8));
				}
			} else if (kind == 1) {
				/* random runs */
				int x = 0, c = rng() & 1;
				while (x < T4_WIDTH) {
					int run = rng() % 3 ? (int)(rng() % 12) : (int)(rng() % 2600);
					for (int i = 0; i < run && x < T4_WIDTH; i++, x++)
						if (c)
							row[x / 8] |= (unsigned char)(0x80 >> (x % 8));
						else
							row[x / 8] &= (unsigned char)~(0x80 >> (x % 8));
					c ^= 1;
				}
			} else if (kind == 2) {
				/* the row above moved a pel or two */
				std::vector<unsigned char> r2(216, 0);
				int d = (int)(rng() % 5) - 2;
				for (int x = 0; x < T4_WIDTH; x++) {
					int sx = x - d;
					if (sx >= 0 && sx < T4_WIDTH && (row[sx / 8] >> (7 - sx % 8) & 1))
						r2[x / 8] |= (unsigned char)(0x80 >> (x % 8));
				}
				row = r2;
			}
			ref.add_row(row.data());
		}
		for (int k : {2, 4, 0}) {
			T4Bits bits;
			if (k)
				ref.encode_mr(&bits, (s16)k, 0, 0, 1);
			else
				ref.encode_mmr(&bits);
			/* our own decoders */
			T4Page own;
			own.init(T4_WIDTH, ref.fine);
			own.decode(bits.buf.data(), (bits.n + 7) / 8, k ? 3 : 4, k ? 1 : 0, 1, 0);
			EXPECT_EQ(own.rows, ref.rows) << "seed " << seed << " k " << k;
			EXPECT_TRUE(own.bits == ref.bits) << "seed " << seed << " k " << k;
			int rows = sp_t4_rx(out.c_str(), k ? 2 : 3, T4_WIDTH, ref.fine, bits.buf.data(), bits.n);
			EXPECT_EQ(rows, ref.rows) << "seed " << seed << " k " << k;
			TiffIn back;
			T4Page got;
			ASSERT_EQ(back.open(out.c_str()), 0);
			ASSERT_EQ(back.read_page(&got), 1);
			int first = -1;
			for (int y = 0; y < ref.rows && y < got.rows; y++)
				if (memcmp(ref.bits.data() + (size_t)y * 216, got.bits.data() + (size_t)y * 216, 216)) {
					first = y;
					break;
				}
			EXPECT_EQ(first, -1) << "seed " << seed << " k " << k;
			back.close();
			own.release();
			bits.release();
		}
		ref.release();
	}
}

INSTANTIATE_TEST_SUITE_P(Codings, SpandspT4, testing::Values(
	T4Coding{ 1, 0 }, T4Coding{ 1, 144 }, T4Coding{ 2, 0 },
	T4Coding{ 2, 144 }),
	/* mh or mr, and _min<bits> */
	[](const testing::TestParamInfo<T4Coding> &i) {
		return std::string(i.param.encoding == 1 ? "mh" : "mr") +
			"_min" + std::to_string(i.param.min_bits);
	});
void *sp_v17_rx(int bps, void (*put)(void *, int), void *ctx);
void sp_v17_rx_restart(void *s, int bps, int short_train);
void sp_v17_rx_samples(void *s, const short *amp, int n);
void sp_v17_rx_free(void *s);

/* the bits our V.17 transmitter sends, and those spandsp's receiver
   put out */
struct OursIntoSpandspV17 {
	std::vector<unsigned char> tx, rx;
	size_t pos = 0;
};

/* our transmitter's callback: the next bit to send */
static s16 v17_ours_get(void *ctx)
{
	OursIntoSpandspV17 *t = static_cast<OursIntoSpandspV17 *>(ctx);
	return t->pos < t->tx.size() ? t->tx[t->pos++] : FAX_BIT_END;
}

/* spandsp's receiver's callback: a data bit kept (status codes not) */
static void v17_spandsp_put(void *ctx, int bit)
{
	if (bit == 0 || bit == 1)
		static_cast<OursIntoSpandspV17 *>(ctx)->rx.push_back((unsigned char)bit);
}

/* our V.17 signal of the bits in t (after 5 frames of silence, then
   10 of silence after it) through mu-law into spandsp's receiver */
static void v17_ours_send(OursIntoSpandspV17 *t, void *rx, int bps, s16 short_train)
{
	enum fax_mod mod;
	s16 st;
	FaxTx tx;
	short amp[160];

	t->pos = 0;
	t->rx.clear();
	fax_mod_from_code(static_cast<u32>(bps / 100 + 1 + short_train), &mod, &st);
	tx.init(mod, short_train, 8000, v17_ours_get, t);
	for (long n = 0, tail = 0; tail < 10 && n < 8000L * 60 / 160; n++) {
		for (int k = 0; k < 160; k++)
			amp[k] = static_cast<short>(sp_ulaw(n < 5 || tx.done ? 0 : dsp_clip16(tx.sample())));
		sp_v17_rx_samples(rx, amp, 160);
		if (tx.done)
			tail++;
	}
}

/* our V.17 transmitter into spandsp's receiver as in a fax call: the
   TCF on the long train, then a page (the test document in MR) on the
   short train, through mu-law.  At every rate no data bit may differ
   (at 14400 bit/s the page lost bits at the old sample phase) */
TEST(SpandspV17Ours, ShortTrainPageIntoSpandsp)
{
	std::string doc = fax_file("doc.tif");
	TiffIn ti;
	T4Page page;
	T4Bits bits;

	ASSERT_TRUE(write_test_doc(doc, true));
	ASSERT_EQ(ti.open(doc.c_str()), 0);
	ASSERT_EQ(ti.read_page(&page), 1);
	page.encode_mr(&bits, 2, 0, 0, 1);
	for (int bps : {14400, 12000, 9600, 7200}) {
		OursIntoSpandspV17 t;
		void *rx = sp_v17_rx(bps, v17_spandsp_put, &t);
		ASSERT_TRUE(rx);
		t.tx.assign(static_cast<size_t>(bps) * 3 / 2, 0);
		v17_ours_send(&t, rx, bps, 0);
		sp_v17_rx_restart(rx, bps, 1);
		t.tx.clear();
		for (size_t i = 0; i < bits.n; i++)
			t.tx.push_back(static_cast<unsigned char>(t4_bits_get(&bits, i)));
		v17_ours_send(&t, rx, bps, 1);
		sp_v17_rx_free(rx);
		/* the received bits aligned on 256 sent ones (after the
		   first 1000, past the receiver's start) */
		long best = -1, fewest = 257;
		for (long off = 0; off + 256 <= static_cast<long>(t.rx.size()) && off < 5000; off++) {
			long e = 0;
			for (int i = 0; i < 256; i++)
				e += t.rx[static_cast<size_t>(off + i)] != t.tx[static_cast<size_t>(1000 + i)];
			if (e < fewest) {
				fewest = e;
				best = off;
			}
		}
		ASSERT_EQ(fewest, 0) << bps << " bit/s: the page not found";
		long d = best - 1000, errs = 0, first = -1;
		size_t i;
		for (i = 1000; i < t.tx.size() && static_cast<long>(i) + d < static_cast<long>(t.rx.size()); i++)
			if (t.rx[static_cast<size_t>(static_cast<long>(i) + d)] != t.tx[i]) {
				if (first < 0)
					first = static_cast<long>(i);
				errs++;
			}
		EXPECT_EQ(i, t.tx.size()) << bps << " bit/s: the page cut short";
		EXPECT_EQ(errs, 0) << bps << " bit/s, the first at bit " << first;
	}
	bits.release();
	page.release();
	ti.close();
}
