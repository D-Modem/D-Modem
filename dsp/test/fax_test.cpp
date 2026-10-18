/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    fax_test.cpp - the fax without a modem core: each class 1
 *    modulation from its transmitter into its receiver, the page coding
 *    (T.4/T.6, TIFF) also against libtiff, and two T.30 machines back to
 *    back sending a document
 *
 *    fax_test --gtest_list_tests    the cases
 *    DSP_DEBUG=<level>              the DSP's log
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../fax.h"
#include "fax_util.h"
#include "t30.h"
#include "t4.h"

using namespace dsp;

/* DSP_DEBUG into the DSP's log level */
static void debug_level()
{
	if (getenv("DSP_DEBUG"))
		dsplibs_debug_level = atoi(getenv("DSP_DEBUG"));
}

/*
 *    the modulations, transmitter -> receiver
 *
 */

struct Loopback {
	int code;       /* class 1 modulation code (+FTM) */
	float snr;      /* dB */
	long bits;
	int srate;
	int page;       /* the bits of a light test page of this seed in MH
			   (bits: at least so many a row), 0: PRBS */
};

class FaxModulation : public testing::TestWithParam<Loopback> {
protected:
	unsigned prbs = 0x1234567;
	std::vector<u8> txb, rxb;
	long ntx = 0, nrx = 0, maxbits = 0;
	Biquad tilt;
	float noise = 0;
	T4Bits page = {};

	~FaxModulation() override { page.release(); }
	/* the transmitter's next bit: the page's or PRBS 2^23-1, kept in
	   txb; FAX_BIT_END after maxbits */
	static int get_bit(void *ctx) {
		FaxModulation *t = static_cast<FaxModulation *>(ctx);
		int b;
		if (t->ntx >= t->maxbits)
			return FAX_BIT_END;
		if (t->page.n) {
			t->txb[t->ntx] = (u8)t4_bits_get(&t->page, t->ntx);
			return t->txb[t->ntx++];
		}
		b = ((t->prbs >> 22) ^ (t->prbs >> 17)) & 1;
		t->prbs = (t->prbs << 1) | b;
		t->txb[t->ntx++] = b;
		return b;
	}
	/* a received bit into rxb (2 * maxbits at most) */
	static void put_bit(void *ctx, int bit) {
		FaxModulation *t = static_cast<FaxModulation *>(ctx);
		if (t->nrx < 2 * t->maxbits)
			t->rxb[t->nrx++] = bit;
	}

	/* one transmission: all bits must arrive intact */
	testing::AssertionResult run(enum fax_mod mod, int short_train,
				     int srate, FaxRxMemory *mem);
};

/* 0.1 s of silence, the signal through a delay of 64 samples, the tilt,
   a gain of 0.5 and the noise, and silence until the receiver loses
   the carrier (120 s at most); the offset of the received bits with
   the fewest errors in the first 256 sent (2000 at most), then every
   bit sent must have come at it, the last 64 excepted */
testing::AssertionResult FaxModulation::run(enum fax_mod mod, int short_train,
					    int srate, FaxRxMemory *mem)
{
	FaxTx tx;
	FaxRx rx;
	float delay[64] = { 0 };
	long n, best = -1, err, i, off;
	const char *train = short_train ? " (short train)" : "";

	ntx = nrx = 0;
	prbs = 0x1234567;
	tx.init(mod, short_train, srate, get_bit, this);
	if (rx.init(mod, short_train, srate, put_bit, this, mem))
		return testing::AssertionFailure() << "receiver init" << train;
	/* a little silence, the signal, silence */
	for (n = 0; n < srate * 120L; n++) {
		float y = n > srate / 10 ? tx.sample() : 0;
		float x = delay[n & 63];
		delay[n & 63] = y;
		x = tilt.run(x) * 0.5f + noise * gauss();
		rx.sample(x);
		if (tx.done && n % srate == 0 && !rx.carrier)
			break;
	}
	rx.save(mem);
	rx.release();
	/* the offset with the fewest errors in the first 256 bits */
	for (off = 0, err = 64; off < 2000 && off + 256 <= nrx; off++) {
		long e = 0;
		for (i = 0; i < 256; i++)
			e += rxb[off + i] != txb[i];
		if (e < err) {
			err = e;
			best = off;
		}
	}
	if (best < 0)
		return testing::AssertionFailure() << "sent " << ntx <<
			", received " << nrx << " bits" << train <<
			": no alignment";
	for (err = 0, i = 0; i < ntx && best + i < nrx; i++)
		err += rxb[best + i] != txb[i];
	if (err || i < ntx - 64)
		return testing::AssertionFailure() << train << " sent " << ntx <<
			", received " << nrx << " (offset " << best <<
			"), compared " << i << ", errors " << err;
	return testing::AssertionSuccess();
}

/* a class 1 code at its SNR and sample rate, the noise relative to its
   transmit level; a short train code after its long train */
TEST_P(FaxModulation, BitsArriveIntact)
{
	const Loopback &p = GetParam();
	enum fax_mod mod;
	int short_train;
	FaxRxMemory mem;

	debug_level();
	ASSERT_TRUE(fax_mod_from_code(p.code, &mod, &short_train));
	maxbits = p.bits;
	if (p.page) {
		T4Page pg;
		test_page(&pg, 0, p.page, 300);
		pg.encode_mh(&page, p.bits, 0, 1);
		pg.release();
		maxbits = (long)page.n;
	}
	txb.assign(maxbits, 0);
	rxb.assign(2 * maxbits, 0);
	srand(1);
	memset(&mem, 0, sizeof(mem));
	noise = fax_tx_rms(mod) * 0.5f * powf(10, -p.snr / 20);
	tilt.lowpass(3000, 0.6f, p.srate);
	/* V.17 short train codes: the long train at the same rate first,
	   then the short one with the equalizer kept */
	if (short_train) {
		EXPECT_TRUE(run(mod, 0, p.srate, &mem));
	}
	EXPECT_TRUE(run(mod, short_train, p.srate, &mem));
}

/* m<code>, then _snr<dB> and _<rate> if not 30 dB and 9600 Hz, and
   _page<seed>_min<bits> for a page */
static std::string loopback_name(const testing::TestParamInfo<Loopback> &i)
{
	std::string n = "m" + std::to_string(i.param.code);
	if (i.param.snr != 30)
		n += "_snr" + std::to_string((int)i.param.snr);
	if (i.param.srate != 9600)
		n += "_" + std::to_string(i.param.srate);
	if (i.param.page)
		n += "_page" + std::to_string(i.param.page) + "_min" +
			std::to_string(i.param.bits);
	return n;
}

INSTANTIATE_TEST_SUITE_P(Class1, FaxModulation, testing::Values(
	Loopback{ 24, 30, 20000, 9600 }, Loopback{ 48, 30, 20000, 9600 },
	Loopback{ 72, 30, 20000, 9600 }, Loopback{ 73, 30, 20000, 9600 },
	Loopback{ 74, 30, 20000, 9600 }, Loopback{ 96, 30, 20000, 9600 },
	Loopback{ 97, 30, 20000, 9600 }, Loopback{ 98, 30, 20000, 9600 },
	Loopback{ 121, 30, 20000, 9600 }, Loopback{ 122, 30, 20000, 9600 },
	Loopback{ 145, 30, 20000, 9600 }, Loopback{ 146, 30, 20000, 9600 },
	/* noise; V.17 short train at 8000 Hz */
	Loopback{ 48, 20, 20000, 9600 }, Loopback{ 96, 25, 20000, 9600 },
	Loopback{ 98, 25, 20000, 8000 },
	/* V.17 14400, long and short train, with pages that once lost a
	   row: their runs of the same few patterns moved the AGC and the
	   timing loop */
	Loopback{ 146, 30, 0, 9600, 7 }, Loopback{ 146, 30, 144, 9600, 16 },
	Loopback{ 146, 30, 0, 8000, 22 }, Loopback{ 146, 30, 144, 8000, 13 }),
	loopback_name);

/*
 *    the page coding and TIFF files
 *
 */

/* the document read and written again: the same pages */
TEST(T4, TiffRoundTrip)
{
	std::string doc = fax_file("doc.tif"), copy = fax_file("copy.tif");
	TiffIn in;
	TiffOut out;
	T4Page p;
	int pages = 0;

	ASSERT_TRUE(write_test_doc(doc, false));
	ASSERT_EQ(in.open(doc.c_str()), 0);
	ASSERT_EQ(out.open(copy.c_str()), 0);
	while (in.read_page(&p) == 1) {
		out.write_page(&p);
		p.release();
		pages++;
	}
	in.close();
	ASSERT_EQ(out.close(), 0);
	EXPECT_EQ(pages, 2);
	EXPECT_TRUE(same_pages(doc, copy));
}

/* libtiff (tiffcp) codes our document again each way; we read it */
class T4Libtiff : public testing::TestWithParam<const char *> {};

/* the document through tiffcp -c <coding>: the same pages (skipped
   without tiffcp) */
TEST_P(T4Libtiff, ReadsItsCoding)
{
	std::string doc = fax_file("doc.tif"), lib = fax_file("lib.tif");
	std::string cmd;

	if (!*TIFFCP)
		GTEST_SKIP() << "no tiffcp";
	ASSERT_TRUE(write_test_doc(doc, false));
	cmd = std::string(TIFFCP) + " -c " + GetParam() + " " + doc + " " + lib;
	ASSERT_EQ(system(cmd.c_str()), 0) << cmd;
	EXPECT_TRUE(same_pages(doc, lib));
}

INSTANTIATE_TEST_SUITE_P(Compression, T4Libtiff,
	/* also in strips of 16 rows (-r 16) */
	testing::Values("none", "g3:1d", "g3:2d", "g4", "g3:1d -r 16",
			"g3:2d -r 16"),
	/* the coding's name, its ':' and ' -r ' as '_' */
	[](const testing::TestParamInfo<const char *> &i) {
		std::string n;
		for (const char *c = i.param; *c; c++)
			if (isalnum((unsigned char)*c))
				n += *c;
			else if (n.back() != '_')
				n += '_';
		return n;
	});

/* MH bits, as written, into data (the first in the top bit) */
static void put_bits(std::vector<unsigned char> &data, size_t &n, const char *bits)
{
	for (; *bits; bits++, n++) {
		if (n / 8 >= data.size())
			data.push_back(0);
		if (*bits == '1')
			data[n / 8] |= 0x80 >> (n % 8);
	}
}

/* a row of runs of length 0, more than the row has pels: not a row
   (and no write past the decoder's changes) */
TEST(T4, ZeroRunsAreNoRow)
{
	std::vector<unsigned char> data;
	size_t n = 0;
	T4Page p;

	for (int i = 0; i < 100; i++) {
		put_bits(data, n, "00110101");         /* white 0 */
		put_bits(data, n, "0000110111");       /* black 0 */
	}
	put_bits(data, n, "101010");                   /* white 16 */
	ASSERT_EQ(p.init(16, 0), 0);
	p.decode(data.data(), data.size(), 2, 0, 1, 1);
	EXPECT_EQ(p.rows, 0);
	p.release();
}

/* a page whose IFD links back to itself: one page, not the same one
   again and again */
TEST(T4, TiffIfdCycle)
{
	std::string doc = fax_file("cycle.tif");
	TiffIn in;
	T4Page p;
	int pages = 0;
	FILE *f;
	unsigned char b[4];
	unsigned long ifd, n;

	ASSERT_TRUE(write_test_doc(doc, false));
	/* the first IFD's next IFD: itself (the file is "II") */
	ASSERT_TRUE((f = fopen(doc.c_str(), "r+b")) != NULL);
	ASSERT_EQ(fseek(f, 4, SEEK_SET), 0);
	ASSERT_EQ(fread(b, 1, 4, f), 4u);
	ifd = b[0] | b[1] << 8 | b[2] << 16 | (unsigned long)b[3] << 24;
	ASSERT_EQ(fseek(f, ifd, SEEK_SET), 0);
	ASSERT_EQ(fread(b, 1, 2, f), 2u);
	n = b[0] | b[1] << 8;
	ASSERT_EQ(fseek(f, ifd + 2 + n * 12, SEEK_SET), 0);
	for (int i = 0; i < 4; i++)
		b[i] = ifd >> (8 * i);
	ASSERT_EQ(fwrite(b, 1, 4, f), 4u);
	fclose(f);

	ASSERT_EQ(in.open(doc.c_str()), 0);
	while (pages < 10 && in.read_page(&p) == 1) {
		p.release();
		pages++;
	}
	in.close();
	EXPECT_EQ(pages, 1);
}

/*
 *    two T.30 machines back to back
 *
 */

struct Session {
	int bps;        /* the highest rate */
	bool ecm;       /* both with error correction mode */
	double snr;     /* dB, 0: no noise */
	bool light;     /* the lighter document */
};

struct Doc {
	TiffIn in;
	TiffOut out;
};

/* the sender's next page (t30_config next_page): read_page's result */
static int next_page(void *ctx, T4Page *p)
{
	return static_cast<Doc *>(ctx)->in.read_page(p);
}

/* a page received, into the receiver's TIFF */
static void page_received(void *ctx, T4Page *p)
{
	static_cast<Doc *>(ctx)->out.write_page(p);
}

/* the document in from a sender to a receiver, the audio through G.711
   both ways with noise for the SNR: both end OK with its 2 pages, the
   receiver's copy the same */
static void send_document(const Session &s, const std::string &in)
{
	std::string out = fax_file("out.tif");
	Doc tx, rx;
	struct t30_config c = {};
	struct t30_status sa = {}, sb = {};
	short ab[160] = { 0 }, ba[160] = { 0 }, oa[160], ob[160];
	double sigma = s.snr ? 3000 / pow(10, s.snr / 20) : 0;
	long n;

	ASSERT_EQ(tx.in.open(in.c_str()), 0);
	ASSERT_EQ(rx.out.open(out.c_str()), 0);
	c.srate = 8000;
	c.max_bps = s.bps;
	c.ecm = s.ecm;
	c.sending = 1;
	c.ident = "+1 555 0100";
	c.next_page = next_page;
	c.ctx = &tx;
	T30 *a = t30_create(&c);
	c.sending = 0;
	c.ident = "+1 555 0199";
	c.next_page = NULL;
	c.page_received = page_received;
	c.ctx = &rx;
	T30 *b = t30_create(&c);

	for (n = 0; n < 8000L * 600; n += 160) {
		t30_process(a, ba, oa, 160);
		t30_process(b, ab, ob, 160);
		for (int i = 0; i < 160; i++) {
			ab[i] = g711_ulaw((short)(oa[i] + sigma * gauss()));
			ba[i] = g711_ulaw((short)(ob[i] + sigma * gauss()));
		}
		t30_get_status(a, &sa);
		t30_get_status(b, &sb);
		if (sa.result != T30_RUNNING && sb.result != T30_RUNNING)
			break;
	}
	rx.out.close();
	tx.in.close();
	EXPECT_EQ(sa.result, T30_OK) << "sender: " << t30_result_text(sa.result);
	EXPECT_EQ(sb.result, T30_OK) << "receiver: " << t30_result_text(sb.result);
	EXPECT_EQ(sa.pages, 2);
	EXPECT_EQ(sb.pages, 2);
	EXPECT_EQ(!!sa.ecm, s.ecm);
	EXPECT_STREQ(sa.remote_ident, "+1 555 0199");
	EXPECT_STREQ(sb.remote_ident, "+1 555 0100");
	printf("%d pages at %d bit/s%s in %.1f s\n", sa.pages, sa.bps,
	       sa.ecm ? " (ECM)" : "", n / 8000.0);
	t30_free(a);
	t30_free(b);
	EXPECT_TRUE(same_pages(in, out));
}

class T30Pair : public testing::TestWithParam<Session> {};

TEST_P(T30Pair, SendsTheDocument)
{
	const Session &s = GetParam();
	std::string in = fax_file("in.tif");

	debug_level();
	ASSERT_TRUE(write_test_doc(in, s.light));
	send_document(s, in);
}

/* error correction mode: the page in blocks of HDLC frames; on a line of
   19 dB the frames errors spoil are sent again (without ECM the page
   comes with bad rows) */
INSTANTIATE_TEST_SUITE_P(Rates, T30Pair, testing::Values(
	Session{ 14400, false, 0, true }, Session{ 12000, false, 0, true },
	Session{ 9600, false, 0, true }, Session{ 7200, false, 0, true },
	Session{ 4800, false, 0, true }, Session{ 2400, false, 0, true },
	Session{ 14400, true, 0, false }, Session{ 9600, true, 0, false },
	Session{ 14400, true, 19, true }),
	/* <bps>, then _ecm and _snr<dB> as the case has them */
	[](const testing::TestParamInfo<Session> &i) {
		std::string n = std::to_string(i.param.bps);
		if (i.param.ecm)
			n += "_ecm";
		if (i.param.snr)
			n += "_snr" + std::to_string((int)i.param.snr);
		return n;
	});

/*
 *    a response lost on the line (DSP_T30_LOSE)
 *
 */

struct Lost {
	const char *lose;       /* DSP_T30_LOSE: <fcf>[:<n>], the nth lost */
	bool same;              /* both pages standard: MPS between them */
	const char *name;
};

class T30Lost : public testing::TestWithParam<Lost> {};

/* the test document's first (standard) page twice */
static testing::AssertionResult write_standard_twice(const std::string &path)
{
	std::string doc = fax_file("doc.tif");
	TiffIn in;
	TiffOut out;
	T4Page p;

	if (!write_test_doc(doc, true) || in.open(doc.c_str()))
		return testing::AssertionFailure() << "cannot write " << doc;
	if (out.open(path.c_str()) || in.read_page(&p) != 1) {
		in.close();
		return testing::AssertionFailure() << "cannot write " << path;
	}
	out.write_page(&p);
	out.write_page(&p);
	p.release();
	in.close();
	if (out.close())
		return testing::AssertionFailure() << "cannot write " << path;
	return testing::AssertionSuccess();
}

/* without ECM our MCF lost: the sender's post-page command again after
   T4 gets MCF again, not RTN (with which the sender would send the page
   once more, and the receiver have it twice) */
TEST_P(T30Lost, McfAgain)
{
	const Lost &l = GetParam();
	std::string in = fax_file("in.tif"), log;

	if (l.same)
		ASSERT_TRUE(write_standard_twice(in));
	else
		ASSERT_TRUE(write_test_doc(in, true));
	setenv("DSP_T30_LOSE", l.lose, 1);
	dsplibs_debug_level = 2;
	testing::internal::CaptureStderr();
	send_document(Session{ 9600, false, 0, true }, in);
	log = testing::internal::GetCapturedStderr();
	dsplibs_debug_level = 0;
	debug_level();
	unsetenv("DSP_T30_LOSE");
	EXPECT_NE(log.find("t30: (test) this one lost"), std::string::npos);
	EXPECT_NE(log.find("the post-page command again: our 8c again"),
		  std::string::npos);
	if (HasFailure())       /* the T.30 lines of the log */
		for (size_t i = 0, e; i < log.size(); i = e + 1) {
			e = log.find('\n', i);
			if (e == std::string::npos)
				e = log.size();
			if (log.compare(i, 9, "dsp: t30:") == 0)
				printf("%.*s\n", (int)(e - i), &log[i]);
		}
}

/* the MCF to the first page's EOM (a fine page next) or MPS, and to the
   last page's EOP */
INSTANTIATE_TEST_SUITE_P(Mcf, T30Lost, testing::Values(
	Lost{ "8c", false, "eom" }, Lost{ "8c", true, "mps" },
	Lost{ "8c:2", false, "eop" }),
	[](const testing::TestParamInfo<Lost> &i) {
		return std::string(i.param.name);
	});
