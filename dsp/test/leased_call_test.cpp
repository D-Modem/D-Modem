/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    leased_call_test.cpp - leased lines and dry pairs in the voice band:
 *    two of libdmodem's modems at 9600 Hz on a simulated circuit (each
 *    way's delay and loss, white noise, and on a 2-wire one each end's
 *    own signal through its hybrid), with no exchange: AT&L1 (2-wire)
 *    or AT&L2 (4-wire), ATD calling at once and S0=1 answering with no
 *    ring.  Each voice band modulation on AT&L1, the 4-wire pumps (V.29,
 *    V.33, V.27ter) on AT&L2: CONNECT and data both ways through V.42,
 *    byte for byte; their rates each way, a dropout they resynchronize
 *    through, noise that brings V.33 down to 12000, an answerer that
 *    comes late, a long cut and the call again by itself.  V.91 on a
 *    4-wire digital circuit (G.711 octets each way at 8000 Hz, with no
 *    V.8: 8.8/V.91): both laws, robbed bits, a late answerer, a cut
 *
 *    LEASED_DEBUG=<level>   libdmodem's log
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "dmodem.h"
#include "portable.h"

namespace {

struct Dte {
	int fd[2];
	/* a socket pair, both ends non-blocking */
	Dte()
	{
		if (dmd::stream_pair(fd))
			throw std::runtime_error("socketpair");
		for (int f : fd)
			dmd::set_nonblocking(f);
	}
	/* (both ends closed) */
	~Dte()
	{
		dmd::fd_close(fd[0]);
		dmd::fd_close(fd[1]);
	}
	/* what the modem wrote to the DTE so far */
	std::string read_all()
	{
		std::string s;
		char buf[4096];
		ssize_t n;
		while ((n = dmd::fd_read(fd[1], buf, sizeof(buf))) > 0)
			s.append(buf, static_cast<size_t>(n));
		return s;
	}
	/* s from at on to the modem, what it took */
	size_t write_some(const std::string &s, size_t at)
	{
		if (at >= s.size())
			return 0;
		ssize_t n = dmd::fd_write(fd[1], s.data() + at,
					  std::min<size_t>(s.size() - at, 4096));
		return n > 0 ? static_cast<size_t>(n) : 0;
	}
};

struct End {
	Dte dte;
	struct dmodem *m = nullptr;
	std::string text, data;
	bool online = false;
	int connects = 0, no_carriers = 0;
	size_t sent = 0;
	/* a modem at rate Hz with its init line (and on a digital
	   circuit its G.711 law) */
	End(const char *name, const char *init, int32_t rate,
	    dmodem_law law = DMODEM_LAW_UNKNOWN)
	{
		dmodem_config c{};
		c.name = name;
		c.dte_fd = dte.fd[0];
		c.init = init;
		c.law = law;
		m = dmodem_create_rate(&c, rate);
	}
	/* (the modem destroyed) */
	~End() { dmodem_destroy(m); }
	/* the DTE's text so far: CONNECT, data, NO CARRIER */
	void take()
	{
		dmodem_poll_dte(m);
		std::string s = dte.read_all();
		if (s.empty())
			return;
		text += s;
		if (!online) {
			size_t at = s.find("CONNECT");
			if (at == std::string::npos)
				return;
			size_t e = s.find("\r\n", at);
			connects++;
			online = true;
			data = e == std::string::npos ? std::string() :
				s.substr(e + 2);
			return;
		}
		struct dmodem_status st;
		dmodem_status(m, &st);
		if (st.link != DMODEM_ONLINE &&
		    s.find("NO CARRIER") != std::string::npos) {
			no_carriers++;
			online = false;
			return;
		}
		data += s;
	}
};

/* a voice band line between the two: each way delay samples, the far
   end's signal at gain, each end's own at echo (2-wire: its hybrid's
   leak; 4-wire: none), white noise */
struct Line {
	double gain = 0.5, echo = 0, noise = 3;     /* noise: rms */
	size_t delay = 96;
	std::deque<s16> ca, ac, ce, ae;
	std::mt19937 rng{1};
	std::normal_distribution<float> g{0, 1};
	double drop_at = -1, drop_s = 0;    /* far end cut (s) */
	double hit_at = -1, hit_s = 0, hit_noise = 0;  /* a burst of
							  noise (s, rms) */
	double change_at = -1, new_echo = 0;  /* the hybrid's echo from
						 then (s) */
	size_t new_delay = 0;                 /* and each way's delay */
	long t = 0;
	s32 rate;
	bool changed = false;
	/* a line at r Hz */
	explicit Line(s32 r) : rate(r) {}
	/* an end's sample in: the far end's and its own, and the noise */
	s16 mix(s16 remote, s16 own)
	{
		double s = static_cast<double>(t) / rate;
		if (s >= drop_at && s < drop_at + drop_s)
			remote = 0;
		double v = gain * remote + echo * own + noise * g(rng);
		if (s >= hit_at && s < hit_at + hit_s)
			v += hit_noise * g(rng);
		if (change_at >= 0 && s >= change_at) {
			echo = new_echo;
			change_at = -1;
			changed = true;
		}
		return static_cast<s16>(std::lround(std::max(-32767.0,
					std::min(32767.0, v))));
	}
};

template <class Stop>
/* secs of the line between c and a, or until stop() */
void run(Line &l, End &c, End &a, double secs, Stop stop)
{
	const s32 n = l.rate / 200;
	std::vector<s16> ci(n), ai(n), co(n), ao(n);
	long end = l.t + static_cast<long>(secs * l.rate);
	if (l.ca.empty()) {
		l.ca.assign(l.delay, 0);
		l.ac.assign(l.delay, 0);
		l.ce.assign(l.delay, 0);
		l.ae.assign(l.delay, 0);
	}
	while (l.t < end) {
		for (s32 i = 0; i < n; i++) {
			ci[i] = l.mix(l.ac.front(), l.ce.front());
			ai[i] = l.mix(l.ca.front(), l.ae.front());
			l.ac.pop_front(); l.ce.pop_front();
			l.ca.pop_front(); l.ae.pop_front();
			l.t++;
		}
		if (l.changed) {
			/* the circuit rerouted: each way's delay */
			l.changed = false;
			while (l.ca.size() < l.new_delay) {
				l.ca.push_front(0);
				l.ac.push_front(0);
			}
		}
		dmodem_process(c.m, ci.data(), co.data(), n);
		dmodem_process(a.m, ai.data(), ao.data(), n);
		for (s32 i = 0; i < n; i++) {
			l.ca.push_back(co[i]);
			l.ce.push_back(co[i]);
			l.ac.push_back(ao[i]);
			l.ae.push_back(ao[i]);
		}
		c.take();
		a.take();
		if (stop())
			break;
	}
}

/* n bytes of data, a different stream for each side */
std::string bulk(size_t n, unsigned side)
{
	std::string s(n, '\0');
	for (size_t k = 0; k < n; k++) {
		unsigned h = static_cast<unsigned>(k) * 2654435761u + side;
		h = ((h >> 16) ^ h) * 0x45d9f3b;
		h = ((h >> 16) ^ h) * 0x45d9f3b;
		s[k] = static_cast<char>((h >> 16) ^ h);
	}
	return s;
}


/* a test's setting in the environment (NULL: none) */
void test_env(const char *var, const char *val)
{
#ifdef _WIN32
	_putenv_s(var, val ? val : "");
#else
	if (val)
		setenv(var, val, 1);
	else
		unsetenv(var);
#endif
}

/* the log level from LEASED_DEBUG */
void debug()
{
	if (getenv("LEASED_DEBUG"))
		dmodem_set_debug(atoi(getenv("LEASED_DEBUG")));
}

/* two modems on a leased line: the caller's commands and the
   answerer's (S0=1 added), each after ATE0X4 and the line's AT&L */
struct Pair {
	Line line{9600};
	End c, a;
	/* the two modems, set up */
	Pair(const std::string &l, const std::string &ci,
	     const std::string &ai)
		: c("caller", ("ATE0X4" + l + ci + "\r").c_str(), 9600),
		  a("answerer", ("ATE0X4" + l + "S0=1" + ai + "\r").c_str(),
		    9600)
	{
		line.echo = l == "&L1" ? 0.1 : 0;  /* 2-wire: -20 dB */
	}
	/* ATD, then CONNECT at both ends within secs */
	bool connect(double secs = 40)
	{
		if (dmodem_dial(c.m, ""))
			return false;
		run(line, c, a, secs, [&] { return c.online && a.online; });
		return c.online && a.online;
	}
	/* n bytes each way at once within secs: whether all came, byte
	   for byte */
	bool exchange(size_t n, double secs, unsigned seed = 1)
	{
		std::string to_a = bulk(n, seed), to_c = bulk(n, seed + 1);
		c.sent = a.sent = 0;
		c.data.clear();
		a.data.clear();
		run(line, c, a, secs, [&] {
			c.sent += c.dte.write_some(to_a, c.sent);
			a.sent += a.dte.write_some(to_c, a.sent);
			return a.data.size() >= n && c.data.size() >= n;
		});
		return a.data == to_a && c.data == to_c;
	}
	/* an end's rates now */
	static std::pair<uint32_t, uint32_t> rates(const End &e)
	{
		struct dmodem_status st;
		dmodem_status(e.m, &st);
		return { st.tx_rate, st.rx_rate };
	}
};

/* "+MS=..." of a test's name: what comes after the '=' */
struct Mod {
	const char *ms;
	const char *name;
	uint32_t rate;          /* the CONNECT's */
};

/* the test's name: the modulation's */
std::string mod_name(const ::testing::TestParamInfo<Mod> &i)
{
	return i.param.name;
}

} // namespace

/* AT&L1, a 2-wire leased line: each voice band modulation connects
   with no exchange (the caller's call progress listens for the answer
   tone and nothing else), and 5 s of data goes both ways */
class TwoWire : public ::testing::TestWithParam<Mod> {};

/* (see TwoWire) */
TEST_P(TwoWire, Connects)
{
	dmodem_init();
	debug();
	const Mod &m = GetParam();
	Pair p("&L1", m.ms, m.ms);
	ASSERT_TRUE(p.c.m && p.a.m);
	ASSERT_TRUE(p.connect()) << p.c.text << p.a.text;
	auto r = Pair::rates(p.c);
	EXPECT_EQ(std::max(r.first, r.second), m.rate) << p.c.text;
	size_t n = std::max<size_t>(200, std::min(r.first, r.second) / 2);
	EXPECT_TRUE(p.exchange(n, 30)) << p.c.data.size() << " "
				       << p.a.data.size();
}

INSTANTIATE_TEST_SUITE_P(Leased, TwoWire, ::testing::Values(
	Mod{ "", "V34", 33600 }, Mod{ "+MS=132", "V32bis", 14400 },
	Mod{ "+MS=32", "V32", 9600 }, Mod{ "+MS=122", "V22bis", 2400 },
	Mod{ "+MS=22", "V22", 1200 }, Mod{ "+MS=212", "Bell212A", 1200 },
	Mod{ "+MS=21", "V21", 300 }, Mod{ "+MS=23", "V23", 1200 },
	Mod{ "+MS=103", "Bell103", 300 }), mod_name);

/* AT&L2, a 4-wire leased line: V.29, V.33 and V.27ter at each of their
   rates, duplex on a continuous carrier */
class FourWire : public ::testing::TestWithParam<Mod> {};

/* (see FourWire) */
TEST_P(FourWire, Connects)
{
	dmodem_init();
	debug();
	const Mod &m = GetParam();
	Pair p("&L2", m.ms, m.ms);
	ASSERT_TRUE(p.connect()) << p.c.text << p.a.text;
	auto rc = Pair::rates(p.c), ra = Pair::rates(p.a);
	EXPECT_EQ(rc.first, m.rate);
	EXPECT_EQ(rc.second, m.rate);
	EXPECT_EQ(ra.first, m.rate);
	EXPECT_EQ(ra.second, m.rate);
	size_t n = m.rate / 2;
	EXPECT_TRUE(p.exchange(n, 30)) << p.c.data.size() << " "
				       << p.a.data.size();
}

INSTANTIATE_TEST_SUITE_P(Leased, FourWire, ::testing::Values(
	Mod{ "+MS=29", "V29_9600", 9600 },
	Mod{ "+MS=29,0,4800,7200", "V29_7200", 7200 },
	Mod{ "+MS=29,0,4800,4800", "V29_4800", 4800 },
	Mod{ "+MS=V33", "V33_14400", 14400 },
	Mod{ "+MS=33,0,12000,12000", "V33_12000", 12000 },
	Mod{ "+MS=V27TC", "V27ter_4800", 4800 },
	Mod{ "+MS=27,0,2400,2400", "V27ter_2400", 2400 }), mod_name);

/* the two ends at different rates: the receivers find the far end's
   rate by themselves, and the faster end comes down to the slower's
   (V.33's rate signal; V.29's and V.27ter's rate) */
TEST(FourWireRates, FasterEndComesDown)
{
	dmodem_init();
	debug();
	const struct {
		const char *c, *a;
		uint32_t rate;
	} cases[] = {
		{ "+MS=29", "+MS=29,0,4800,4800", 4800 },
		{ "+MS=29,0,4800,7200", "+MS=29", 7200 },
		{ "+MS=33", "+MS=33,0,12000,12000", 12000 },
		{ "+MS=27,0,2400,2400", "+MS=27", 2400 },
	};
	for (const auto &k : cases) {
		Pair p("&L2", k.c, k.a);
		ASSERT_TRUE(p.connect()) << k.c << " " << k.a;
		/* (the faster one's synchronizing signal at the slower
		   rate after its CONNECT) */
		run(p.line, p.c, p.a, 5, [] { return false; });
		auto rc = Pair::rates(p.c), ra = Pair::rates(p.a);
		EXPECT_EQ(rc.first, k.rate) << k.c << " " << k.a;
		EXPECT_EQ(rc.second, k.rate) << k.c << " " << k.a;
		EXPECT_EQ(ra.first, k.rate) << k.c << " " << k.a;
		EXPECT_EQ(ra.second, k.rate) << k.c << " " << k.a;
		EXPECT_TRUE(p.exchange(k.rate / 4, 20)) << k.c << " " << k.a;
	}
}

/* the circuit cut for 0.4 s in the middle of the data (S10, 0.7 s, not
   reached): each receiver finds the far end's synchronizing signal
   again or sends its own, LAPM sends again what was lost; no NO
   CARRIER, every byte */
TEST(FourWireResync, DropoutUnderLapm)
{
	dmodem_init();
	debug();
	for (const char *ms : { "+MS=29", "+MS=33", "+MS=27" }) {
		Pair p("&L2", ms, ms);
		ASSERT_TRUE(p.connect()) << ms;
		p.line.drop_at = static_cast<double>(p.line.t) / 9600 + 2;
		p.line.drop_s = 0.4;
		EXPECT_TRUE(p.exchange(10000, 60, 7)) << ms << ": "
			<< p.a.data.size() << " " << p.c.data.size();
		EXPECT_EQ(p.c.no_carriers + p.a.no_carriers, 0)
			<< ms << ": " << p.c.text << p.a.text;
		EXPECT_EQ(p.c.connects, 1) << ms;
	}
}

/* noise (about 23 dB) that 14400 does not stand: the receivers lose
   their equalization at it, and V.33 comes down to 12000 both ways */
TEST(FourWireResync, NoiseBringsV33Down)
{
	dmodem_init();
	debug();
	Pair p("&L2", "+MS=33", "+MS=33");
	p.line.noise = 200;
	ASSERT_TRUE(p.connect());
	run(p.line, p.c, p.a, 20, [&] {
		return Pair::rates(p.c).first == 12000 &&
			Pair::rates(p.a).first == 12000;
	});
	run(p.line, p.c, p.a, 5, [] { return false; });
	auto rc = Pair::rates(p.c), ra = Pair::rates(p.a);
	EXPECT_EQ(rc.first, 12000u);
	EXPECT_EQ(rc.second, 12000u);
	EXPECT_EQ(ra.first, 12000u);
	EXPECT_EQ(ra.second, 12000u);
	EXPECT_TRUE(p.exchange(6000, 30, 9));
}

/* the answerer set up 8 s after the caller has started: the caller's
   synchronizing signals every 1.2 s after its last, until the
   answerer's comes */
TEST(FourWireResync, LateAnswerer)
{
	dmodem_init();
	debug();
	Line line(9600);
	End c("caller", "ATE0X4&L2+MS=29\r", 9600);
	ASSERT_EQ(dmodem_dial(c.m, ""), 0);
	/* the answerer silent meanwhile: a modem of its own with no AT&L
	   (it does not answer) */
	End idle("idle", "ATE0X4\r", 9600);
	run(line, c, idle, 8, [] { return false; });
	EXPECT_FALSE(c.online);
	End a("answerer", "ATE0X4&L2S0=1+MS=29\r", 9600);
	run(line, c, a, 15, [&] { return c.online && a.online; });
	ASSERT_TRUE(c.online && a.online) << c.text << a.text;
	std::string msg = "late but here\r\n";
	c.dte.write_some(msg, 0);
	run(line, c, a, 5, [&] {
		return a.data.find(msg) != std::string::npos;
	});
	EXPECT_NE(a.data.find(msg), std::string::npos);
}

/* AT&L2: a cut longer than S10: NO CARRIER at both ends, and each
   starts the call again by itself; CONNECT again once the circuit is
   back, and data */
TEST(FourWireResync, CallsAgainAfterACut)
{
	dmodem_init();
	debug();
	Pair p("&L2", "S7=10+MS=29", "S7=10+MS=29");
	ASSERT_TRUE(p.connect());
	p.line.drop_at = static_cast<double>(p.line.t) / 9600 + 1;
	p.line.drop_s = 12;
	run(p.line, p.c, p.a, 60, [&] {
		return p.c.connects >= 2 && p.a.connects >= 2 &&
			p.c.online && p.a.online;
	});
	EXPECT_GE(p.c.no_carriers, 1) << p.c.text;
	EXPECT_GE(p.a.no_carriers, 1) << p.a.text;
	ASSERT_GE(p.c.connects, 2) << p.c.text;
	ASSERT_GE(p.a.connects, 2) << p.a.text;
	EXPECT_TRUE(p.exchange(2000, 20, 11));
}

/* AT&L1: the circuit rerouted in the middle of the data (each end's
   echo and each way's delay new): the echo cancellers and equalizers
   are lost, each receiver finds out from its error and retrains by
   itself (5.5/V.32, 11.5/V.34), the call goes on and LAPM sends again
   what was lost.  Without V.32's own retrain (DSP_V32_LOST=0) the
   call is lost */
class TwoWireRetrain : public ::testing::TestWithParam<Mod> {};

/* (see TwoWireRetrain) */
TEST_P(TwoWireRetrain, AfterAReroute)
{
	dmodem_init();
	debug();
	auto call = [](const char *ms, std::string *why) {
		Pair p("&L1", ms, ms);
		if (!p.connect()) {
			*why = "no CONNECT";
			return false;
		}
		p.line.change_at = static_cast<double>(p.line.t) / 9600 + 2;
		p.line.new_echo = -0.2;
		p.line.new_delay = 200;
		bool ok = p.exchange(20000, 60, 13);
		*why = std::to_string(p.a.data.size()) + " " +
			std::to_string(p.c.data.size()) + "\n" + p.c.text +
			p.a.text;
		return ok && p.c.no_carriers + p.a.no_carriers == 0;
	};
	std::string why;
	EXPECT_TRUE(call(GetParam().ms, &why)) << why;
	if (std::string(GetParam().name) == "V32bis") {
		test_env("DSP_V32_LOST", "0");
		EXPECT_FALSE(call(GetParam().ms, &why)) << why;
		test_env("DSP_V32_LOST", nullptr);
	}
}

INSTANTIATE_TEST_SUITE_P(Leased, TwoWireRetrain, ::testing::Values(
	Mod{ "", "V34", 33600 }, Mod{ "+MS=132", "V32bis", 14400 }),
	mod_name);

/* AT&L1, V.32bis: a second of loud noise (about 0 dB) in the data: the
   equalizers come back by themselves after it, with no retrain into
   the noise, and the call goes on */
TEST(TwoWireNoise, V32bisThroughABurst)
{
	dmodem_init();
	debug();
	Pair p("&L1", "+MS=132", "+MS=132");
	ASSERT_TRUE(p.connect());
	p.line.hit_at = static_cast<double>(p.line.t) / 9600 + 2;
	p.line.hit_s = 1;
	p.line.hit_noise = 6000;
	EXPECT_TRUE(p.exchange(20000, 60, 15)) << p.a.data.size() << " "
					       << p.c.data.size();
	EXPECT_EQ(p.c.no_carriers + p.a.no_carriers, 0)
		<< p.c.text << p.a.text;
}

namespace {

/* a command line from e's DTE, then the line until e's text after it
   has want (within secs): whether it came */
bool command(Pair &p, End &e, const std::string &cmd,
	     const std::string &want, double secs = 15)
{
	size_t from = e.text.size();
	e.dte.write_some(cmd + "\r", 0);
	run(p.line, p.c, p.a, secs, [&] {
		return e.text.find(want, from) != std::string::npos;
	});
	return e.text.find(want, from) != std::string::npos;
}

/* +++ after a second of quiet, and its OK */
bool escape(Pair &p, End &e)
{
	run(p.line, p.c, p.a, 1.2, [] { return false; });
	size_t from = e.text.size();
	e.dte.write_some("+++", 0);
	run(p.line, p.c, p.a, 3, [&] {
		return e.text.find("OK", from) != std::string::npos;
	});
	return e.text.find("OK", from) != std::string::npos;
}

/* the text after from */
std::string since(const End &e, size_t from)
{
	return e.text.substr(std::min(from, e.text.size()));
}

} // namespace

/* V.54's loop 2 (the remote digital loop) with V.52's self-test,
   AT&T7, on a call with no error correction: the far end, AT&T4,
   recognises the PREP and loops after its ACK; the 511-bit pattern
   comes back with no errors on a clean line; AT&T0 sends the TERM,
   reports 000 and OK; the far end is out of its loop, and data goes
   both ways again after ATO */
class RemoteLoop : public ::testing::TestWithParam<Mod> {};

/* (see RemoteLoop) */
TEST_P(RemoteLoop, SelfTest)
{
	dmodem_init();
	debug();
	const Mod &m = GetParam();
	std::string l = m.rate == 9600 ? "&L2" : "&L1";
	Pair p(l, std::string(m.ms) + "\\N0", std::string(m.ms) + "\\N0&T4");
	ASSERT_TRUE(p.connect()) << p.c.text << p.a.text;
	ASSERT_TRUE(escape(p, p.c)) << p.c.text;
	ASSERT_TRUE(command(p, p.c, "AT&T7", "OK")) << p.c.text;
	run(p.line, p.c, p.a, 3, [] { return false; });
	size_t from = p.c.text.size();
	ASSERT_TRUE(command(p, p.c, "AT&T0", "OK", 60)) << since(p.c, from);
	EXPECT_NE(since(p.c, from).find("000\r\n"), std::string::npos)
		<< since(p.c, from);
	/* (the far end's DTE saw nothing of the test) */
	EXPECT_EQ(p.a.no_carriers, 0);
	ASSERT_TRUE(command(p, p.c, "ATO", "CONNECT"));
	run(p.line, p.c, p.a, 1, [] { return false; });
	EXPECT_TRUE(p.exchange(m.rate / 8, 30, 21))
		<< p.c.data.size() << " " << p.a.data.size();
}

INSTANTIATE_TEST_SUITE_P(Leased, RemoteLoop, ::testing::Values(
	Mod{ "+MS=29", "V29", 9600 }, Mod{ "+MS=132", "V32bis", 14400 },
	Mod{ "+MS=122", "V22bis", 2400 }), mod_name);

/* AT&T6: loop 2 for the DTE's own data, CONNECT, and what it sends
   comes back to it (the far end's DTE gets none of it); AT&T0 after
   the escape ends it */
TEST(RemoteLoopData, EchoesTheDte)
{
	dmodem_init();
	debug();
	Pair p("&L2", "+MS=29\\N0", "+MS=29\\N0&T4");
	ASSERT_TRUE(p.connect());
	ASSERT_TRUE(escape(p, p.c));
	ASSERT_TRUE(command(p, p.c, "AT&T6", "CONNECT")) << p.c.text;
	p.c.data.clear();
	size_t a_had = p.a.data.size();
	std::string msg = "round the loop and back\r\n";
	p.c.dte.write_some(msg, 0);
	run(p.line, p.c, p.a, 5, [&] {
		return p.c.data.find(msg) != std::string::npos;
	});
	EXPECT_NE(p.c.data.find(msg), std::string::npos) << p.c.data;
	EXPECT_EQ(p.a.data.find(msg, a_had), std::string::npos);
	ASSERT_TRUE(escape(p, p.c));
	EXPECT_TRUE(command(p, p.c, "AT&T0", "OK", 30)) << p.c.text;
}

/* the far end refuses loop 2 (AT&T5, as after ATZ): no ACK, ERROR */
TEST(RemoteLoopData, Refused)
{
	dmodem_init();
	debug();
	Pair p("&L2", "+MS=29\\N0", "+MS=29\\N0&T5");
	ASSERT_TRUE(p.connect());
	ASSERT_TRUE(escape(p, p.c));
	EXPECT_TRUE(command(p, p.c, "AT&T7", "ERROR")) << p.c.text;
	/* and none on a call with error correction */
	Pair q("&L2", "+MS=29", "+MS=29&T4");
	ASSERT_TRUE(q.connect());
	ASSERT_TRUE(escape(q, q.c));
	EXPECT_TRUE(command(q, q.c, "AT&T7", "ERROR")) << q.c.text;
}

/* AT&T3: loop 2 at our own end (the far end's data back to it) */
TEST(RemoteLoopData, LocalDigitalLoop)
{
	dmodem_init();
	debug();
	Pair p("&L2", "+MS=29\\N0", "+MS=29\\N0");
	ASSERT_TRUE(p.connect());
	ASSERT_TRUE(escape(p, p.a));
	ASSERT_TRUE(command(p, p.a, "AT&T3", "OK"));
	p.c.data.clear();
	std::string msg = "back from the far end\r\n";
	p.c.dte.write_some(msg, 0);
	run(p.line, p.c, p.a, 5, [&] {
		return p.c.data.find(msg) != std::string::npos;
	});
	EXPECT_NE(p.c.data.find(msg), std::string::npos) << p.c.data;
	EXPECT_TRUE(command(p, p.a, "AT&T0", "OK"));
}

/* V.54's loop 3 (the local analogue loop) on a 4-wire pump, offline:
   AT&T8 with the self-test (OK, then 000 at AT&T0); AT&T1 for the
   DTE's data (CONNECT, and its data back); ERROR without AT&L or with
   a 2-wire modulation */
TEST(LocalLoop, SelfTestAndData)
{
	dmodem_init();
	debug();
	for (const char *ms : { "+MS=29", "+MS=33", "+MS=27" }) {
		Pair p("&L2", std::string(ms) + "\\N0", "S0=0");
		ASSERT_TRUE(command(p, p.c, "AT&T8", "OK", 20))
			<< ms << p.c.text;
		run(p.line, p.c, p.a, 2, [] { return false; });
		size_t from = p.c.text.size();
		ASSERT_TRUE(command(p, p.c, "AT&T0", "OK")) << ms;
		EXPECT_NE(since(p.c, from).find("000\r\n"),
			  std::string::npos) << ms << since(p.c, from);
		run(p.line, p.c, p.a, 1, [] { return false; });
		ASSERT_TRUE(command(p, p.c, "AT&T1", "CONNECT", 20))
			<< ms << p.c.text;
		p.c.online = true;
		p.c.data.clear();
		std::string msg = "through my own modulator\r\n";
		p.c.dte.write_some(msg, 0);
		run(p.line, p.c, p.a, 5, [&] {
			return p.c.data.find(msg) != std::string::npos;
		});
		EXPECT_NE(p.c.data.find(msg), std::string::npos) << ms;
		ASSERT_TRUE(escape(p, p.c));
		EXPECT_TRUE(command(p, p.c, "AT&T0", "OK"));
		/* (the far end's line heard nothing) */
		EXPECT_EQ(p.a.connects, 0);
	}
	Pair d("&L0", "\\N0+MS=29", "S0=0");
	EXPECT_TRUE(command(d, d.c, "AT&T8", "ERROR"));
	Pair v("&L2", "\\N0+MS=132", "S0=0");
	EXPECT_TRUE(command(v, v.c, "AT&T8", "ERROR"));
}

/* S18: the test ends by itself after its seconds */
TEST(LocalLoop, TestTimer)
{
	dmodem_init();
	debug();
	Pair p("&L2", "+MS=29\\N0S18=3", "S0=0");
	ASSERT_TRUE(command(p, p.c, "AT&T1", "CONNECT", 20)) << p.c.text;
	size_t from = p.c.text.size();
	run(p.line, p.c, p.a, 5, [] { return false; });
	EXPECT_NE(since(p.c, from).find("OK"), std::string::npos)
		<< since(p.c, from);
	struct dmodem_status st;
	dmodem_status(p.c.m, &st);
	EXPECT_NE(st.link, DMODEM_ONLINE);
}

/* V.13's simulated carrier control (S48=1) on a 4-wire line with no
   error correction: between the DTE's bursts the line carries the idle
   pattern, which the far end's V.13 takes for 109 OFF (its DTE gets
   nothing); each burst's ON pattern turns it ON again, and every burst
   comes through whole.  A far end without V.13 gets the idle pattern
   as characters */
TEST(SimulatedCarrier, BurstsAndIdle)
{
	dmodem_init();
	debug();
	Pair p("&L2", "+MS=29\\N0S48=1", "+MS=29\\N0S48=1");
	ASSERT_TRUE(p.connect()) << p.c.text << p.a.text;
	run(p.line, p.c, p.a, 2, [] { return false; });
	p.a.data.clear();
	p.c.data.clear();
	run(p.line, p.c, p.a, 3, [] { return false; });
	EXPECT_TRUE(p.a.data.empty()) << p.a.data.size();
	EXPECT_TRUE(p.c.data.empty()) << p.c.data.size();
	std::string all, back;
	for (int k = 0; k < 5; k++) {
		std::string msg = "burst " + std::to_string(k) + " " +
			bulk(300, k) + "\r\n";
		std::string rep = "reply " + std::to_string(k) + "\r\n";
		all += msg;
		back += rep;
		p.c.dte.write_some(msg, 0);
		p.a.dte.write_some(rep, 0);
		run(p.line, p.c, p.a, 1.5, [] { return false; });
	}
	EXPECT_EQ(p.a.data, all);
	EXPECT_EQ(p.c.data, back);
	EXPECT_EQ(p.c.no_carriers + p.a.no_carriers, 0);

	/* the far end without it: the idle pattern reaches its DTE */
	Pair q("&L2", "+MS=29\\N0S48=1", "+MS=29\\N0");
	ASSERT_TRUE(q.connect());
	run(q.line, q.c, q.a, 1, [] { return false; });
	q.a.data.clear();
	run(q.line, q.c, q.a, 2, [] { return false; });
	EXPECT_GT(q.a.data.size(), 100u);
}

namespace {

/* a 4-wire digital circuit (64 kbit/s each way, as a leased line's
   DS0 or E1 time slot): each way's G.711 octets delay octets later
   (at least a block's, 160),
   unchanged; robbed: the LSB of every 6th octet set (robbed-bit
   signalling); a cut: the idle code instead */
struct Digital {
	dmodem_law law;
	size_t delay = 240;
	bool robbed = false;
	std::deque<unsigned char> ca, ac;
	double drop_at = -1, drop_s = 0;
	long t = 0;
	/* (a circuit of law) */
	explicit Digital(dmodem_law l) : law(l) {}
	/* an octet as it comes out at the far end */
	unsigned char pass(unsigned char o, long k)
	{
		double s = static_cast<double>(k) / 8000;
		if (s >= drop_at && s < drop_at + drop_s)
			return law == DMODEM_LAW_ALAW ? 0xD5 : 0xFF;
		if (robbed && k % 6 == 5)
			o |= 1;
		return o;
	}
};

template <class Stop>
/* secs of the circuit between c and a, or until stop() */
void run91(Digital &l, End &c, End &a, double secs, Stop stop)
{
	const s32 n = 160;
	std::vector<unsigned char> ci(n), ai(n), co(n), ao(n);
	long end = l.t + static_cast<long>(secs * 8000);
	unsigned char idle = l.law == DMODEM_LAW_ALAW ? 0xD5 : 0xFF;
	if (l.ca.empty()) {
		l.ca.assign(l.delay, idle);
		l.ac.assign(l.delay, idle);
	}
	while (l.t < end) {
		for (s32 i = 0; i < n; i++) {
			ci[i] = l.pass(l.ac.front(), l.t + i);
			ai[i] = l.pass(l.ca.front(), l.t + i);
			l.ac.pop_front();
			l.ca.pop_front();
		}
		l.t += n;
		dmodem_process_g711(c.m, ci.data(), co.data(), n);
		dmodem_process_g711(a.m, ai.data(), ao.data(), n);
		l.ca.insert(l.ca.end(), co.begin(), co.end());
		l.ac.insert(l.ac.end(), ao.begin(), ao.end());
		c.take();
		a.take();
		if (stop())
			break;
	}
}

/* two modems on a digital 4-wire circuit with AT&L2+MS=V91 (and the
   answerer's S0=1), each its init's extra */
struct Pair91 {
	Digital line;
	End c, a;
	/* (the two, set up) */
	Pair91(dmodem_law law, const std::string &ci = "",
	       const std::string &ai = "")
		: line(law),
		  c("caller", ("ATE0X4&L2+MS=V91" + ci + "\r").c_str(),
		    8000, law),
		  a("answerer", ("ATE0X4&L2S0=1+MS=V91" + ai + "\r").c_str(),
		    8000, law) {}
	/* ATD, then CONNECT at both ends within secs */
	bool connect(double secs = 20)
	{
		if (dmodem_dial(c.m, ""))
			return false;
		run91(line, c, a, secs, [&] { return c.online && a.online; });
		return c.online && a.online;
	}
	/* n bytes each way within secs, byte for byte */
	bool exchange(size_t n, double secs, unsigned seed = 1)
	{
		std::string to_a = bulk(n, seed), to_c = bulk(n, seed + 1);
		c.sent = a.sent = 0;
		c.data.clear();
		a.data.clear();
		run91(line, c, a, secs, [&] {
			c.sent += c.dte.write_some(to_a, c.sent);
			a.sent += a.dte.write_some(to_c, a.sent);
			return a.data.size() >= n && c.data.size() >= n;
		});
		return a.data == to_a && c.data == to_c;
	}
};

/* a test's name: the law's */
std::string law_name(const ::testing::TestParamInfo<dmodem_law> &i)
{
	return i.param == DMODEM_LAW_ALAW ? "Alaw" : "ulaw";
}

} // namespace

/* AT&L2+MS=V91 on a clean digital circuit: V.91 with no V.8, CONNECT
   within 3 s of ATD, the same rate both ways (62667: K = 41, 124 or
   125 codes in each interval), 200 kB each way through V.42 */
class FourWireV91 : public ::testing::TestWithParam<dmodem_law> {};

/* (see FourWireV91) */
TEST_P(FourWireV91, Symmetric)
{
	dmodem_init();
	debug();
	Pair91 p(GetParam());
	ASSERT_TRUE(p.connect(3)) << p.c.text << p.a.text;
	auto rc = Pair::rates(p.c), ra = Pair::rates(p.a);
	EXPECT_EQ(rc.first, rc.second);
	EXPECT_EQ(rc.first, ra.first);
	EXPECT_EQ(rc.second, ra.second);
	EXPECT_GE(rc.first, 62000u) << p.c.text;
	/* (CONNECT names V.91's rate) */
	EXPECT_NE(p.c.text.find("CONNECT 6"), std::string::npos) << p.c.text;
	EXPECT_TRUE(p.exchange(200000, 60)) << p.c.data.size() << " "
					    << p.a.data.size();
}

INSTANTIATE_TEST_SUITE_P(Leased, FourWireV91, ::testing::Values(
	DMODEM_LAW_ULAW, DMODEM_LAW_ALAW), law_name);

/* robbed-bit signalling on the circuit: a code fewer in one interval
   of six, both ends ask for less, and still the same rate both ways */
TEST(FourWireV91Line, RobbedBits)
{
	dmodem_init();
	debug();
	Pair91 p(DMODEM_LAW_ULAW);
	p.line.robbed = true;
	ASSERT_TRUE(p.connect(5)) << p.c.text << p.a.text;
	auto rc = Pair::rates(p.c), ra = Pair::rates(p.a);
	EXPECT_EQ(rc.first, rc.second);
	EXPECT_EQ(rc.first, ra.first);
	EXPECT_LT(rc.first, 62000u);
	EXPECT_GE(rc.first, 56000u);
	EXPECT_TRUE(p.exchange(50000, 30));
}

/* the answerer set up 8 s after the caller has started: the caller's
   INFO goes on, with no escape to V.34, until the answerer's comes */
TEST(FourWireV91Line, LateAnswerer)
{
	dmodem_init();
	debug();
	Digital line(DMODEM_LAW_ULAW);
	End c("caller", "ATE0X4&L2+MS=V91\r", 8000, DMODEM_LAW_ULAW);
	ASSERT_EQ(dmodem_dial(c.m, ""), 0);
	End idle("idle", "ATE0X4\r", 8000, DMODEM_LAW_ULAW);
	run91(line, c, idle, 8, [] { return false; });
	EXPECT_FALSE(c.online);
	End a("answerer", "ATE0X4&L2S0=1+MS=V91\r", 8000, DMODEM_LAW_ULAW);
	run91(line, c, a, 10, [&] { return c.online && a.online; });
	ASSERT_TRUE(c.online && a.online) << c.text << a.text;
	EXPECT_GE(Pair::rates(c).first, 62000u);
	std::string msg = "late but here\r\n";
	c.dte.write_some(msg, 0);
	run91(line, c, a, 5, [&] {
		return a.data.find(msg) != std::string::npos;
	});
	EXPECT_NE(a.data.find(msg), std::string::npos);
}

/* a short cut (0.3 s): a retrain (INFO again) and every byte through
   LAPM, no NO CARRIER; a long one (15 s): NO CARRIER at both ends,
   each starts the call again by itself, CONNECT again once the
   circuit is back */
TEST(FourWireV91Line, Cuts)
{
	dmodem_init();
	debug();
	Pair91 p(DMODEM_LAW_ALAW);
	ASSERT_TRUE(p.connect());
	p.line.drop_at = static_cast<double>(p.line.t) / 8000 + 1;
	p.line.drop_s = 0.3;
	EXPECT_TRUE(p.exchange(100000, 40, 5)) << p.a.data.size() << " "
					       << p.c.data.size();
	EXPECT_EQ(p.c.no_carriers + p.a.no_carriers, 0)
		<< p.c.text << p.a.text;
	p.line.drop_at = static_cast<double>(p.line.t) / 8000 + 1;
	p.line.drop_s = 15;
	run91(p.line, p.c, p.a, 60, [&] {
		return p.c.connects >= 2 && p.a.connects >= 2 &&
			p.c.online && p.a.online;
	});
	EXPECT_GE(p.c.no_carriers, 1) << p.c.text;
	EXPECT_GE(p.a.no_carriers, 1) << p.a.text;
	ASSERT_GE(p.c.connects, 2) << p.c.text;
	ASSERT_GE(p.a.connects, 2) << p.a.text;
	EXPECT_TRUE(p.exchange(20000, 20, 13));
}
