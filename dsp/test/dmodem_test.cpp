/*
 *    dmodem_test.cpp - libdmodem: two of its modems back to back in one
 *    process (the caller dials, the answerer answers), lines of text both
 *    ways through their DTE socketpairs, several pairs in threads at once;
 *    retrains and rate renegotiations in the middle of the data; V.91
 *    across a G.711 network as it is, with robbed bits, a 3 dB pad, and
 *    mu-law to A-law; bulk data; and the same through dmodem.hpp
 *
 *    DMODEM_DEBUG=<level>   libdmodem's log
 */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "dmodem.h"
#include "dmodem.hpp"

#define FRAME 160               /* 20 ms, as Asterisk */

extern int dmodem_g711_decode(int alaw, unsigned char c);
extern unsigned char dmodem_g711_encode(int alaw, int x);

/* the network between the two modems */
enum Net {
	NET_LINEAR,     /* the 16 bit samples across unchanged */
	NET_G711,       /* G.711 octets as they are */
	NET_RBS,        /* robbed bit signalling: the LSB of every 6th octet
			   is 1 (a T1 trunk; each way at another place in
			   the frame) */
	NET_PAD,        /* a 3 dB digital loss (decoded, scaled, coded
			   again) */
	NET_XLAW,       /* the caller on mu-law, the answerer on A-law,
			   converted */
};

struct PairCase {
	const char *name;
	const char *ms;                 /* the modems' AT commands */
	int pairs = 1;                  /* at once, in threads */
	int seconds = 30;
	std::map<std::string, std::string> env; /* the DSP's test hooks */
	Net net = NET_LINEAR;
	bool alaw = false;
	int delay = 0;                  /* samples more from the caller to
					   the answerer (NET_LINEAR) */
	const char *ms_b = nullptr;     /* the answerer's instead */
	unsigned rate = 0;              /* online at least this fast */
	long bulk = 0;                  /* then that many bytes from the
					   caller to the answerer */
};

/* bulk data that does not compress */
static char bulk_byte(long k)
{
	unsigned h = (unsigned)k;
	h = ((h >> 16) ^ h) * 0x45d9f3b;
	h = ((h >> 16) ^ h) * 0x45d9f3b;
	return (char)((h >> 16) ^ h);
}

/* an octet across the network: from a modem on law from to one on law
   to; k counts the octets of this direction */
static unsigned char net_octet(Net net, unsigned char c, int from, int to,
			       long k, int phase)
{
	switch (net) {
	case NET_RBS:
		if ((k + phase) % 6 == 0)
			c |= 1;
		break;
	case NET_PAD:
		c = dmodem_g711_encode(to,
			(int)(dmodem_g711_decode(from, c) * 0.708f));
		break;
	case NET_XLAW:
		c = dmodem_g711_encode(to, dmodem_g711_decode(from, c));
		break;
	default:
		break;
	}
	return c;
}

static int nonblock(int fd)
{
	return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void drain(int fd)
{
	char buf[256];
	while (read(fd, buf, sizeof(buf)) > 0)
		;
}

/* one pair's call; what went wrong, if anything */
struct Pair {
	const PairCase *c;
	int id;
	std::vector<std::string> failures;
	unsigned tx_rate = 0, rx_rate = 0;
};

static void *run(void *arg)
{
	Pair *p = static_cast<Pair *>(arg);
	const PairCase &pc = *p->c;
	Net net = pc.net;
	struct dmodem_config c = {};
	struct dmodem *a, *b;
	int sa[2], sb[2], i, frames = pc.seconds * 50, online = 0;
	short ab[FRAME] = { 0 }, ba[FRAME] = { 0 }, oa[FRAME], ob[FRAME];
	short held[FRAME] = { 0 };
	int delay = pc.delay;
	char init[64], init_b[64], msg[] = "The quick brown fox jumps over the lazy dog.\r\n";
	char got_a[4096], got_b[4096];
	int na = 0, nb = 0, sent = 0, len = (int)strlen(msg);
	unsigned char gab[FRAME], gba[FRAME], goa[FRAME], gob[FRAME];
	int law_a, law_b;
	long ka = 0, kb = 0;
	long bulk = pc.bulk;
	long bulk_out = 0, bulk_in = 0, bulk_start = 0, bulk_bad = 0;
	bool fax_seen = false;

	socketpair(AF_UNIX, SOCK_STREAM, 0, sa);
	socketpair(AF_UNIX, SOCK_STREAM, 0, sb);
	nonblock(sa[0]); nonblock(sa[1]); nonblock(sb[0]); nonblock(sb[1]);
	snprintf(init, sizeof(init), "ATE0Q1X3%s\r", pc.ms);
	c.init = init;
	c.law = pc.alaw ? DMODEM_LAW_ALAW : DMODEM_LAW_ULAW;
	c.name = "caller";
	c.dte_fd = sa[0];
	if (net == NET_XLAW)
		c.law = DMODEM_LAW_ULAW;
	law_a = c.law == DMODEM_LAW_ALAW;
	a = dmodem_create(&c);
	c.name = "answerer";
	c.dte_fd = sb[0];
	if (pc.ms_b) {
		snprintf(init_b, sizeof(init_b), "ATE0Q1X3%s\r", pc.ms_b);
		c.init = init_b;
	}
	if (net == NET_XLAW)
		c.law = DMODEM_LAW_ALAW;
	law_b = c.law == DMODEM_LAW_ALAW;
	b = dmodem_create(&c);
	if (delay < 0 || delay >= FRAME)
		delay = 0;
	memset(gab, law_b ? 0xD5 : 0xFF, sizeof(gab));
	memset(gba, law_a ? 0xD5 : 0xFF, sizeof(gba));
	drain(sa[1]);
	drain(sb[1]);
	dmodem_answer(b);
	dmodem_dial(a, "");

	for (i = 0; i < frames; i++) {
		struct dmodem_status s1, s2;
		int n;
		if (net == NET_LINEAR) {
			dmodem_process(a, ba, oa, FRAME);
			dmodem_process(b, ab, ob, FRAME);
			/* ab: the last <delay> samples of the previous frame,
			   then this one's */
			memcpy(ab, held, delay * sizeof(short));
			memcpy(ab + delay, oa, (FRAME - delay) * sizeof(short));
			memcpy(held, oa + FRAME - delay, delay * sizeof(short));
			memcpy(ba, ob, sizeof(ba));
		} else {
			int k;
			dmodem_process_g711(a, gba, goa, FRAME);
			dmodem_process_g711(b, gab, gob, FRAME);
			for (k = 0; k < FRAME; k++) {
				gab[k] = net_octet(net, goa[k], law_a, law_b, ka++, 0);
				gba[k] = net_octet(net, gob[k], law_b, law_a, kb++, 3);
			}
		}
		dmodem_poll_dte(a);
		dmodem_poll_dte(b);
		dmodem_status(a, &s1);
		dmodem_status(b, &s2);
		/* a data call must never look like a fax machine calling */
		if ((s1.fax_caller || s2.fax_caller) && !fax_seen) {
			p->failures.push_back("fax CNG seen in a data call");
			fax_seen = true;
		}
		if (!online && s1.link == DMODEM_ONLINE &&
		    s2.link == DMODEM_ONLINE) {
			online = i;
			p->tx_rate = s1.tx_rate;
			p->rx_rate = s1.rx_rate;
			printf("pair %d: online after %.1f s, %u/%u bit/s\n",
			       p->id, i / 50.0, s1.tx_rate, s1.rx_rate);
			if (s1.tx_rate < pc.rate || s1.rx_rate < pc.rate)
				p->failures.push_back("online at " +
					std::to_string(s1.tx_rate) + "/" +
					std::to_string(s1.rx_rate) +
					" bit/s, slower than " +
					std::to_string(pc.rate));
			drain(sa[1]);
			drain(sb[1]);
		}
		/* bulk data, once the text is through */
		if (bulk && online && sent == 5 && na >= 5 * len &&
		    nb >= 5 * len && bulk_in < bulk) {
			char buf[4096];
			long k;
			if (!bulk_start)
				bulk_start = i;
			while (bulk_out < bulk) {
				int m = (int)(bulk - bulk_out < (long)sizeof(buf) ?
					      bulk - bulk_out : (long)sizeof(buf));
				for (k = 0; k < m; k++)
					buf[k] = bulk_byte(bulk_out + k);
				m = (int)write(sa[1], buf, m);
				if (m <= 0)
					break;
				bulk_out += m;
			}
			while ((n = read(sb[1], buf, sizeof(buf))) > 0) {
				for (k = 0; k < n; k++)
					if (buf[k] != bulk_byte(bulk_in + k))
						bulk_bad++;
				bulk_in += n;
			}
			if (bulk_in >= bulk) {
				double t = (i - bulk_start) / 50.0;
				printf("pair %d: %ld bytes in %.1f s: %.0f bit/s\n",
				       p->id, bulk_in, t, bulk_in * 8 / t);
				if (bulk_bad)
					p->failures.push_back(std::to_string(bulk_bad) +
						" bulk bytes wrong");
			}
			continue;
		}
		/* a line of text each way every second once online */
		if (online && i > online + 50 && (i - online) % 50 == 0 &&
		    sent < 5) {
			if (write(sa[1], msg, len) < 0 || write(sb[1], msg, len) < 0)
				perror("write");
			sent++;
		}
		while ((n = read(sa[1], got_a + na, sizeof(got_a) - na)) > 0)
			na += n;
		while ((n = read(sb[1], got_b + nb, sizeof(got_b) - nb)) > 0)
			nb += n;
	}
	if (!online)
		p->failures.push_back("never online");
	if (bulk && bulk_in < bulk)
		p->failures.push_back("only " + std::to_string(bulk_in) + " of " +
				      std::to_string(bulk) + " bulk bytes");
	if (sent != 5 || na != 5 * len || nb != 5 * len ||
	    memcmp(got_a, msg, len) || memcmp(got_b, msg, len))
		p->failures.push_back("text: " + std::to_string(sent) +
			" lines sent, caller got [" + std::string(got_a, na) +
			"], answerer got [" + std::string(got_b, nb) + "]");
	dmodem_destroy(a);
	dmodem_destroy(b);
	close(sa[0]); close(sa[1]); close(sb[0]); close(sb[1]);
	return NULL;
}

class DmodemPair : public testing::TestWithParam<PairCase> {
protected:
	void SetUp() override {
		for (auto &e : GetParam().env)
			setenv(e.first.c_str(), e.second.c_str(), 1);
	}
	void TearDown() override {
		for (auto &e : GetParam().env)
			unsetenv(e.first.c_str());
	}
};

TEST_P(DmodemPair, DataBothWays)
{
	const PairCase &pc = GetParam();
	std::vector<Pair> p(pc.pairs);
	std::vector<pthread_t> th(pc.pairs);

	dmodem_init();
	if (getenv("DMODEM_DEBUG"))
		dmodem_set_debug(atoi(getenv("DMODEM_DEBUG")));
	for (int i = 0; i < pc.pairs; i++) {
		p[i].c = &pc;
		p[i].id = i;
		ASSERT_EQ(pthread_create(&th[i], NULL, run, &p[i]), 0);
	}
	for (int i = 0; i < pc.pairs; i++)
		pthread_join(th[i], NULL);
	for (auto &q : p)
		for (auto &f : q.failures)
			ADD_FAILURE() << "pair " << q.id << ": " << f;
}

static std::string name(const testing::TestParamInfo<PairCase> &i)
{
	return i.param.name;
}

static PairCase pair(const char *name, const char *ms, int pairs, int seconds,
		     std::map<std::string, std::string> env = {})
{
	PairCase c;
	c.name = name;
	c.ms = ms;
	c.pairs = pairs;
	c.seconds = seconds;
	c.env = env;
	return c;
}

/* a caller that tries V.34 and V.32bis first against an answerer of
   +MS=122: it must find the caller's 100 ms of S1 and connect at 2400 (two
   sample alignments of the two modems) */
static PairCase v8_to_122(const char *name, int delay)
{
	PairCase c = pair(name, "", 1, 30);
	c.ms_b = "+MS=122";
	c.rate = 2400;
	c.delay = delay;
	return c;
}

/* V.91 (or what finds it) on a G.711 network */
static PairCase v91(const char *name, const char *ms, Net net, unsigned rate,
		    int seconds = 25, std::map<std::string, std::string> env = {})
{
	PairCase c = pair(name, ms, 1, seconds, env);
	c.net = net;
	c.rate = rate;
	return c;
}

static PairCase alaw(PairCase c)
{
	c.alaw = true;
	return c;
}

static PairCase bulk(PairCase c, long bytes)
{
	c.bulk = bytes;
	return c;
}

INSTANTIATE_TEST_SUITE_P(Modulations, DmodemPair, testing::Values(
	pair("v22", "+MS=22", 3, 30), pair("v22bis", "+MS=122", 3, 30),
	pair("v32", "+MS=32", 3, 30), pair("v32bis", "+MS=132", 3, 30),
	pair("v34", "+MS=34", 3, 30),
	v8_to_122("v8_to_122_d0", 0), v8_to_122("v8_to_122_d5", 5)), name);

/* retrains in the middle of the data (V.42 carries on): by both modems,
   by the answerer or the caller alone (the other one follows); V.34 rate
   renegotiations (to 19200 bit/s, then back up a step at a time while
   the margin allows), the other one answering */
INSTANTIATE_TEST_SUITE_P(Retrain, DmodemPair, testing::Values(
	pair("v32_retrain", "+MS=132", 1, 30, { { "DSP_V32_RETRAIN", "1500" } }),
	pair("v34_retrain_both", "+MS=34", 1, 60, { { "DSP_V34_RETRAIN", "3000" } }),
	pair("v34_retrain_a", "+MS=34", 1, 60, { { "DSP_V34_RETRAIN", "3000a" } }),
	pair("v34_retrain_c", "+MS=34", 1, 60, { { "DSP_V34_RETRAIN", "3000c" } }),
	pair("v34_reneg_both", "+MS=34", 1, 40, { { "DSP_V34_RENEG", "5000:8" } }),
	pair("v34_reneg_a", "+MS=34", 1, 40, { { "DSP_V34_RENEG", "5000:8a" } }),
	pair("v34_reneg_c", "+MS=34", 1, 40, { { "DSP_V34_RENEG", "5000:8c" } })),
	name);

/* V.91 between two of them on a G.711 network: as it is, with robbed
   bits, a 3 dB pad, mu-law to A-law; the DIL shows the receiver which
   codes come through.  The default AT+MS=92, and K56flex's AT+MS=56
   (V.90), find V.91 as well; 200 kB that do not compress; a retrain by
   the caller in the data (V.42 carries on); the control channel; one
   modem silent in V.91: both escape to V.34 */
INSTANTIATE_TEST_SUITE_P(V91, DmodemPair, testing::Values(
	v91("v91_g711", "+MS=91", NET_G711, 60000),
	v91("v91_rbs", "+MS=91", NET_RBS, 60000),
	v91("v91_pad", "+MS=91", NET_PAD, 60000),
	v91("v91_xlaw", "+MS=91", NET_XLAW, 60000),
	alaw(v91("v91_alaw", "+MS=91", NET_G711, 62666)),
	bulk(v91("v91_bulk", "+MS=92", NET_G711, 62666, 60), 200000),
	v91("ms56", "+MS=56", NET_G711, 0),
	v91("v91_retrain", "+MS=91", NET_G711, 0, 30,
	    { { "DSP_V91_RETRAIN", "3000c" } }),
	v91("v91_cc", "+MS=91", NET_RBS, 60000, 25, { { "DSP_V91_CC", "1" } }),
	v91("v91_escape", "+MS=91", NET_G711, 33600, 35,
	    { { "DSP_V91_MUTE", "a" } })), name);

/*
 *    dmodem.hpp
 *
 */

namespace {

struct Dte {
	int fd[2];
	Dte()
	{
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd))
			throw std::runtime_error("socketpair");
		for (int f : fd)
			fcntl(f, F_SETFL, fcntl(f, F_GETFL) | O_NONBLOCK);
	}
	~Dte()
	{
		close(fd[0]);
		close(fd[1]);
	}
	Dte(const Dte &) = delete;
	Dte &operator=(const Dte &) = delete;
	int modem_end() const { return fd[0]; }
	std::string read_all()
	{
		std::string s;
		char buf[256];
		ssize_t n;
		while ((n = read(fd[1], buf, sizeof(buf))) > 0)
			s.append(buf, n);
		return s;
	}
	void write_str(const std::string &s)
	{
		if (write(fd[1], s.data(), s.size()) !=
		    static_cast<ssize_t>(s.size()))
			throw std::runtime_error("DTE write");
	}
};

dm::Modem make(const char *name, Dte &dte, const std::string &init)
{
	dmodem_config c{};
	c.name = name;
	c.dte_fd = dte.modem_end();
	c.law = DMODEM_LAW_ULAW;
	c.init = init.c_str();
	return dm::Modem(c);
}

} // namespace

/* two modems through the C++ classes, a line of text each way */
TEST(DmodemCxx, TextBothWays)
{
	const std::string msg = "The quick brown fox jumps over the lazy dog.\r\n";
	const std::string init = "ATE0Q1X3+MS=132\r";

	dm::init();
	Dte da, db;
	dm::Modem a = make("caller", da, init);
	dm::Modem b = make("answerer", db, init);
	da.read_all();
	db.read_all();
	b.answer();
	a.dial("");

	std::array<short, FRAME> ab{}, ba{}, oa{}, ob{};
	std::string got_a, got_b;
	int online = 0;
	for (int i = 0; i < 20 * 50; i++) {
		a.process(ba.data(), oa.data(), FRAME);
		b.process(ab.data(), ob.data(), FRAME);
		ab = oa;
		ba = ob;
		a.poll_dte();
		b.poll_dte();
		if (!online && a.online() && b.online()) {
			online = i;
			da.read_all();
			db.read_all();
			da.write_str(msg);
			db.write_str(msg);
		}
		if (online) {
			got_a += da.read_all();
			got_b += db.read_all();
		}
		if (got_a == msg && got_b == msg)
			break;
	}
	EXPECT_NE(online, 0) << "never online";
	EXPECT_EQ(got_a, msg);
	EXPECT_EQ(got_b, msg);
}
