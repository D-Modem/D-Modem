/*
 *    fax_test.cpp - the fax without a modem core: each class 1
 *    modulation from its transmitter into its receiver, the page coding
 *    (T.4/T.6, TIFF) also against libtiff, and two T.30 machines back to
 *    back sending a document
 *
 *    fax_test --gtest_list_tests    the cases
 *    DSP_DEBUG=<level>              the DSP's log
 */

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
};

class FaxModulation : public testing::TestWithParam<Loopback> {
protected:
	unsigned prbs = 0x1234567;
	std::vector<u8> txb, rxb;
	long ntx = 0, nrx = 0, maxbits = 0;
	Biquad tilt;
	float noise = 0;

	/* the transmitter's next bit: PRBS 2^23-1, kept in txb; FAX_BIT_END
	   after maxbits */
	static int get_bit(void *ctx) {
		FaxModulation *t = static_cast<FaxModulation *>(ctx);
		int b;
		if (t->ntx >= t->maxbits)
			return FAX_BIT_END;
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

/* m<code>, then _snr<dB> and _<rate> if not 30 dB and 9600 Hz */
static std::string loopback_name(const testing::TestParamInfo<Loopback> &i)
{
	std::string n = "m" + std::to_string(i.param.code);
	if (i.param.snr != 30)
		n += "_snr" + std::to_string((int)i.param.snr);
	if (i.param.srate != 9600)
		n += "_" + std::to_string(i.param.srate);
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
	Loopback{ 98, 25, 20000, 8000 }), loopback_name);

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
	testing::Values("none", "g3:1d", "g3:2d", "g4"),
	/* the coding's name, its ':' as '_' */
	[](const testing::TestParamInfo<const char *> &i) {
		std::string n = i.param;
		for (char &c : n)
			if (c == ':')
				c = '_';
		return n;
	});

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

class T30Pair : public testing::TestWithParam<Session> {};

/* the audio through G.711 both ways, with noise for an SNR */
TEST_P(T30Pair, SendsTheDocument)
{
	const Session &s = GetParam();
	std::string in = fax_file("in.tif"), out = fax_file("out.tif");
	Doc tx, rx;
	struct t30_config c = {};
	struct t30_status sa = {}, sb = {};
	short ab[160] = { 0 }, ba[160] = { 0 }, oa[160], ob[160];
	double sigma = s.snr ? 3000 / pow(10, s.snr / 20) : 0;
	long n;

	debug_level();
	ASSERT_TRUE(write_test_doc(in, s.light));
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
	EXPECT_EQ(!!sa.ecm, s.ecm);
	EXPECT_STREQ(sa.remote_ident, "+1 555 0199");
	EXPECT_STREQ(sb.remote_ident, "+1 555 0100");
	printf("%d pages at %d bit/s%s in %.1f s\n", sa.pages, sa.bps,
	       sa.ecm ? " (ECM)" : "", n / 8000.0);
	t30_free(a);
	t30_free(b);
	EXPECT_TRUE(same_pages(in, out));
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
