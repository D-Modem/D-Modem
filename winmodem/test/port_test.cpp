/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    port_test.cpp - the virtual modem driver's COM port (port.h) and its
 *                    frames to dmodemd (wire.h): reads by each kind of
 *                    serial timeout, writes held and timed, the wait
 *                    mask's events, the modem's lines and the app's DTR
 *                    and RTS, LSRMST_INSERT, purges, the receive buffer's
 *                    room, the settings and properties; the frames
 *                    through a reader a byte at a time.
 *
 */

#include <string.h>

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "port.h"
#include "wire.h"

using namespace vmodem;

namespace {

uint64_t g_now = 1000;

/* the tests' clock */
uint64_t clk()
{
	return g_now;
}

/* a request of n bytes (a write's: s), its completion kept */
class Req : public Request {
public:
	/* n bytes of room */
	explicit Req(size_t n) : buf(n) {}
	/* s to write */
	explicit Req(const std::string &s) : buf(s.begin(), s.end()) {}
	/* (Request's) */
	uint8_t *data() override { return buf.data(); }
	/* (Request's) */
	size_t size() const override { return buf.size(); }
	/* (Request's: kept, counted) */
	void complete(uint32_t st, size_t n) override
	{
		completed++;
		status = st;
		info = n;
	}
	/* what was read */
	std::string text() const
	{
		return std::string(buf.begin(), buf.begin() + info);
	}
	/* a wait's events */
	uint32_t events() const
	{
		uint32_t e;
		memcpy(&e, buf.data(), 4);
		return e;
	}

	std::vector<uint8_t> buf;
	int completed = 0;
	uint32_t status = 0xffffffff;
	size_t info = 0;
};

/* the port's callbacks counted */
class Host : public PortHost {
public:
	/* (PortHost's, each counted or kept) */
	void tx_ready() override { tx++; }
	/* (see tx_ready) */
	void rx_room() override { room++; }
	/* (see tx_ready) */
	void state_changed() override { state++; }
	/* (see tx_ready) */
	void timer(uint64_t d) override { due = d; }
	int tx = 0, room = 0, state = 0;
	uint64_t due = 0;
};

/* a port opened, dmodemd there with DSR and CTS on */
class PortTest : public ::testing::Test {
protected:
	/* (see the class) */
	PortTest() : port(&host, clk)
	{
		g_now = 1000;
		port.open();
		port.connect(true);
		port.set_lines(LINE_DSR | LINE_CTS);
	}
	/* an IOCTL with input in (n bytes) and no output */
	uint32_t set(uint32_t code, const void *in, size_t n)
	{
		size_t info;
		return port.ioctl(code, in, n, nullptr, 0, &info, nullptr);
	}
	/* an IOCTL's 4 bytes of output */
	uint32_t get32(uint32_t code)
	{
		uint32_t v = 0xdeadbeef;
		size_t info = 0;
		EXPECT_EQ(port.ioctl(code, nullptr, 0, &v, 4, &info, nullptr),
			  ST_SUCCESS);
		EXPECT_EQ(info, 4u);
		return v;
	}
	/* the read timeouts (interval, multiplier, constant) and the write
	   ones */
	void timeouts(uint32_t ri, uint32_t rm, uint32_t rc, uint32_t wm = 0,
		      uint32_t wc = 0)
	{
		SerialTimeouts t = { ri, rm, rc, wm, wc };
		ASSERT_EQ(set(IO_SET_TIMEOUTS, &t, sizeof(t)), ST_SUCCESS);
	}
	/* s from the modem */
	size_t rx(const std::string &s)
	{
		return port.rx_put((const uint8_t *)s.data(), s.size());
	}
	/* what the modem would take */
	std::string tx()
	{
		uint8_t b[Port::TX_SIZE];
		size_t n = port.tx_take(b, sizeof(b));
		return std::string((const char *)b, n);
	}

	Host host;
	Port port;
};

const uint32_t MAXT = 0xffffffffu;

/* interval MAXULONG, the rest 0: a read at once, with what there is */
TEST_F(PortTest, ReadAtOnce)
{
	timeouts(MAXT, 0, 0);
	Req r(8);
	port.read(&r);
	EXPECT_EQ(r.completed, 1);
	EXPECT_EQ(r.status, ST_SUCCESS);
	EXPECT_EQ(r.info, 0u);
	rx("OK\r\n");
	Req r2(8);
	port.read(&r2);
	EXPECT_EQ(r2.text(), "OK\r\n");
}

/* MAXULONG, MAXULONG, c: the first characters, or nothing after c ms */
TEST_F(PortTest, ReadFirstCharacter)
{
	/* Unimodem's and PuTTY's kind: the first character, or c ms */
	timeouts(MAXT, MAXT, 500);
	Req r(16);
	port.read(&r);
	EXPECT_EQ(r.completed, 0);
	EXPECT_EQ(host.due, 1500u);
	g_now += 100;
	rx("AT");
	EXPECT_EQ(r.completed, 1);
	EXPECT_EQ(r.status, ST_SUCCESS);
	EXPECT_EQ(r.text(), "AT");
	EXPECT_EQ(host.due, 0u);

	Req r2(16);
	port.read(&r2);
	g_now += 499;
	port.tick();
	EXPECT_EQ(r2.completed, 0);
	g_now += 1;
	port.tick();
	EXPECT_EQ(r2.completed, 1);
	EXPECT_EQ(r2.status, ST_TIMEOUT);
	EXPECT_EQ(r2.info, 0u);
}

/* multiplier and constant: the total time, with what came */
TEST_F(PortTest, ReadTotalTimeout)
{
	timeouts(0, 10, 100);
	Req r(4);
	port.read(&r);
	EXPECT_EQ(host.due, 1140u);
	rx("ab");
	EXPECT_EQ(r.completed, 0);
	g_now = 1139;
	port.tick();
	EXPECT_EQ(r.completed, 0);
	g_now = 1140;
	port.tick();
	EXPECT_EQ(r.completed, 1);
	EXPECT_EQ(r.status, ST_TIMEOUT);
	EXPECT_EQ(r.text(), "ab");
}

/* an interval alone: timed from each character */
TEST_F(PortTest, ReadIntervalTimeout)
{
	timeouts(50, 0, 0);
	Req r(10);
	port.read(&r);
	port.tick();
	EXPECT_EQ(r.completed, 0);
	EXPECT_EQ(host.due, 0u);       /* (no interval before a character) */
	rx("x");
	EXPECT_EQ(host.due, 1050u);
	g_now = 1040;
	rx("y");
	EXPECT_EQ(host.due, 1090u);
	g_now = 1089;
	port.tick();
	EXPECT_EQ(r.completed, 0);
	g_now = 1090;
	port.tick();
	EXPECT_EQ(r.status, ST_TIMEOUT);
	EXPECT_EQ(r.text(), "xy");
}

/* no timeouts: each read until full, in turn */
TEST_F(PortTest, ReadUntilFull)
{
	timeouts(0, 0, 0);
	Req a(3), b(2);
	port.read(&a);
	port.read(&b);
	rx("12");
	EXPECT_EQ(a.completed, 0);
	rx("345");
	EXPECT_EQ(a.text(), "123");
	EXPECT_EQ(b.text(), "45");
	EXPECT_EQ(a.status, ST_SUCCESS);
}

/* no dmodemd: a write dropped at once, EV_TXEMPTY */
TEST_F(PortTest, WriteWithoutDmodemd)
{
	port.connect(false);
	uint32_t m = PEV_TXEMPTY;
	ASSERT_EQ(set(IO_SET_WAIT_MASK, &m, 4), ST_SUCCESS);
	Req w(std::string("ATZ\r"));
	port.write(&w);
	EXPECT_EQ(w.completed, 1);
	EXPECT_EQ(w.info, 4u);
	EXPECT_EQ(tx(), "");
	Req ev(4);
	size_t info;
	EXPECT_EQ(port.ioctl(IO_WAIT_ON_MASK, nullptr, 0, ev.data(), 4, &info,
			     &ev), ST_PENDING);
	EXPECT_EQ(ev.completed, 1);
	EXPECT_EQ(ev.events(), PEV_TXEMPTY);
}

/* a write bigger than the buffer held until dmodemd takes it */
TEST_F(PortTest, WriteHeldUntilTaken)
{
	std::string big(Port::TX_SIZE + 10, 'x');
	Req w(big);
	port.write(&w);
	EXPECT_EQ(w.completed, 0);
	EXPECT_GE(host.tx, 1);
	EXPECT_EQ(tx().size(), (size_t)Port::TX_SIZE);
	EXPECT_EQ(w.completed, 1);
	EXPECT_EQ(w.info, big.size());
	EXPECT_EQ(tx().size(), 10u);
}

/* a held write's total time */
TEST_F(PortTest, WriteTimeout)
{
	timeouts(0, 0, 0, 1, 100);
	std::string big(Port::TX_SIZE + 10, 'x');
	Req w(big);
	port.write(&w);
	EXPECT_EQ(host.due, 1000u + 100 + big.size());
	g_now = host.due;
	port.tick();
	EXPECT_EQ(w.status, ST_TIMEOUT);
	EXPECT_EQ(w.info, (size_t)Port::TX_SIZE);
}

/* IOCTL_SERIAL_WAIT_ON_MASK: an event, one before the wait, a new mask, none */
TEST_F(PortTest, WaitOnMask)
{
	uint32_t m = PEV_RXCHAR | PEV_RLSD | PEV_RING;
	ASSERT_EQ(set(IO_SET_WAIT_MASK, &m, 4), ST_SUCCESS);
	EXPECT_EQ(get32(IO_GET_WAIT_MASK), m);
	size_t info;
	Req ev(4);
	ASSERT_EQ(port.ioctl(IO_WAIT_ON_MASK, nullptr, 0, ev.data(), 4, &info,
			     &ev), ST_PENDING);
	EXPECT_EQ(ev.completed, 0);
	Req ev2(4);
	EXPECT_EQ(port.ioctl(IO_WAIT_ON_MASK, nullptr, 0, ev2.data(), 4,
			     &info, &ev2), ST_INVALID_PARAMETER);
	port.set_lines(LINE_DSR | LINE_CTS | LINE_DCD);
	EXPECT_EQ(ev.completed, 1);
	EXPECT_EQ(ev.events(), PEV_RLSD);

	/* (an event before the wait: the wait at once) */
	rx("x");
	port.set_lines(LINE_DSR | LINE_CTS | LINE_DCD | LINE_RI);
	Req ev3(4);
	port.ioctl(IO_WAIT_ON_MASK, nullptr, 0, ev3.data(), 4, &info, &ev3);
	EXPECT_EQ(ev3.completed, 1);
	EXPECT_EQ(ev3.events(), PEV_RXCHAR | PEV_RING);

	/* a new mask ends a waiting wait with none */
	Req ev4(4);
	port.ioctl(IO_WAIT_ON_MASK, nullptr, 0, ev4.data(), 4, &info, &ev4);
	EXPECT_EQ(ev4.completed, 0);
	m = PEV_DSR;
	set(IO_SET_WAIT_MASK, &m, 4);
	EXPECT_EQ(ev4.completed, 1);
	EXPECT_EQ(ev4.events(), 0u);

	/* no mask: no wait */
	m = 0;
	set(IO_SET_WAIT_MASK, &m, 4);
	Req ev5(4);
	EXPECT_EQ(port.ioctl(IO_WAIT_ON_MASK, nullptr, 0, ev5.data(), 4,
			     &info, &ev5), ST_INVALID_PARAMETER);
}

/* the event character (EV_RXFLAG) and the buffer at 80% (EV_RX80FULL) */
TEST_F(PortTest, RxFlagAndFull)
{
	SerialChars c = { 0, 0, 0, '\n', 0x11, 0x13 };
	ASSERT_EQ(set(IO_SET_CHARS, &c, sizeof(c)), ST_SUCCESS);
	uint32_t m = PEV_RXFLAG | PEV_RX80FULL;
	set(IO_SET_WAIT_MASK, &m, 4);
	size_t info;
	Req ev(4);
	port.ioctl(IO_WAIT_ON_MASK, nullptr, 0, ev.data(), 4, &info, &ev);
	rx("abc");
	EXPECT_EQ(ev.completed, 0);
	rx("\r\n");
	EXPECT_EQ(ev.events(), PEV_RXFLAG);
	Req ev2(4);
	port.ioctl(IO_WAIT_ON_MASK, nullptr, 0, ev2.data(), 4, &info, &ev2);
	rx(std::string(Port::RX_SIZE * 8 / 10, 'z'));
	EXPECT_EQ(ev2.events(), PEV_RX80FULL);
}

/* the modem's lines and their deltas, dmodemd going and coming */
TEST_F(PortTest, ModemStatus)
{
	uint32_t s = get32(IO_GET_MODEMSTATUS);
	EXPECT_EQ(s, (uint32_t)(PMSR_DSR | PMSR_CTS | PMSR_DDSR | PMSR_DCTS));
	EXPECT_EQ(get32(IO_GET_MODEMSTATUS), (uint32_t)(PMSR_DSR | PMSR_CTS));
	port.set_lines(LINE_DSR | LINE_CTS | LINE_DCD | LINE_RI);
	port.set_lines(LINE_DSR | LINE_CTS | LINE_DCD);
	EXPECT_EQ(get32(IO_GET_MODEMSTATUS),
		  (uint32_t)(PMSR_DSR | PMSR_CTS | PMSR_DCD | PMSR_DDCD |
			     PMSR_TERI));
	/* dmodemd gone: every line off */
	port.connect(false);
	EXPECT_EQ(get32(IO_GET_MODEMSTATUS),
		  (uint32_t)(PMSR_DDSR | PMSR_DCTS | PMSR_DDCD));
	/* (its lines are its own again when it comes back) */
	port.set_lines(LINE_DCD);
	EXPECT_EQ(get32(IO_GET_MODEMSTATUS), 0u);
	port.connect(true);
	port.set_lines(LINE_DSR);
	EXPECT_EQ(get32(IO_GET_MODEMSTATUS),
		  (uint32_t)(PMSR_DSR | PMSR_DDSR));
}

/* the app's DTR and RTS: set, by the handflow, refused in handshake modes */
TEST_F(PortTest, DtrAndRts)
{
	/* open: the default handflow's DTR and RTS on */
	EXPECT_EQ(port.app_state(), STATE_OPEN | STATE_DTR | STATE_RTS);
	EXPECT_EQ(get32(IO_GET_DTRRTS), DTRRTS_DTR | DTRRTS_RTS);
	int st = host.state;
	ASSERT_EQ(set(IO_CLR_DTR, nullptr, 0), ST_SUCCESS);
	EXPECT_EQ(port.app_state(), STATE_OPEN | STATE_RTS);
	EXPECT_EQ(host.state, st + 1);
	set(IO_SET_DTR, nullptr, 0);
	EXPECT_EQ(port.app_state(), STATE_OPEN | STATE_DTR | STATE_RTS);

	/* SetCommState's DTR_CONTROL_DISABLE and RTS_CONTROL_HANDSHAKE */
	SerialHandflow h = { 0, HF_RTS_HANDSHAKE, 0, 0 };
	ASSERT_EQ(set(IO_SET_HANDFLOW, &h, sizeof(h)), ST_SUCCESS);
	EXPECT_EQ(port.app_state(), STATE_OPEN | STATE_RTS);
	EXPECT_EQ(set(IO_CLR_RTS, nullptr, 0), ST_INVALID_PARAMETER);
	SerialHandflow g;
	size_t info;
	ASSERT_EQ(port.ioctl(IO_GET_HANDFLOW, nullptr, 0, &g, sizeof(g),
			     &info, nullptr), ST_SUCCESS);
	EXPECT_EQ(g.flow_replace, (uint32_t)HF_RTS_HANDSHAKE);
	h.control_handshake = HF_DTR_HANDSHAKE;
	set(IO_SET_HANDFLOW, &h, sizeof(h));
	EXPECT_EQ(set(IO_SET_DTR, nullptr, 0), ST_INVALID_PARAMETER);

	uint32_t mcr = 0;
	h = { HF_DTR_CONTROL, HF_RTS_CONTROL, 0, 0 };
	set(IO_SET_HANDFLOW, &h, sizeof(h));
	set(IO_SET_MODEM_CONTROL, &mcr, 4);
	EXPECT_EQ(port.app_state(), STATE_OPEN);

	port.close();
	EXPECT_EQ(port.app_state(), 0);
}

/* closing cancels what waits and empties the buffers */
TEST_F(PortTest, CloseCancels)
{
	timeouts(0, 0, 0);
	Req r(4), w(std::string(Port::TX_SIZE + 1, 'q')), ev(4);
	uint32_t m = PEV_RXCHAR;
	set(IO_SET_WAIT_MASK, &m, 4);
	size_t info;
	port.read(&r);
	port.write(&w);
	EXPECT_EQ(port.ioctl(IO_WAIT_ON_MASK, nullptr, 0, ev.data(), 4, &info,
			     &ev), ST_PENDING);
	port.close();
	EXPECT_EQ(r.status, ST_CANCELLED);
	EXPECT_EQ(w.status, ST_CANCELLED);
	EXPECT_EQ(w.info, (size_t)Port::TX_SIZE);
	EXPECT_EQ(ev.status, ST_CANCELLED);
	EXPECT_EQ(tx(), "");
	/* the modem's characters for a closed port go nowhere */
	EXPECT_EQ(rx("RING\r\n"), 6u);
	port.open();
	Req r2(8);
	timeouts(MAXT, 0, 0);
	port.read(&r2);
	EXPECT_EQ(r2.info, 0u);
}

/* a read cancelled with what it had; the next one the head */
TEST_F(PortTest, Cancel)
{
	timeouts(0, 0, 0);
	Req a(4), b(4);
	port.read(&a);
	port.read(&b);
	rx("ab");
	EXPECT_TRUE(port.cancel(&a));
	EXPECT_EQ(Port::done(&a), 2u);
	EXPECT_EQ(a.completed, 0);      /* (the caller completes it) */
	EXPECT_FALSE(port.cancel(&a));
	rx("cdef");
	EXPECT_EQ(b.text(), "cdef");
}

/* IOCTL_SERIAL_PURGE: a read aborted, the buffer cleared */
TEST_F(PortTest, Purge)
{
	timeouts(0, 0, 0);
	Req r(4);
	port.read(&r);
	uint32_t p = PRG_RXABORT;
	ASSERT_EQ(set(IO_PURGE, &p, 4), ST_SUCCESS);
	EXPECT_EQ(r.status, ST_CANCELLED);
	rx("junk");
	p = PRG_RXCLEAR;
	set(IO_PURGE, &p, 4);
	SerialStatus s;
	size_t info;
	ASSERT_EQ(port.ioctl(IO_GET_COMMSTATUS, nullptr, 0, &s, sizeof(s),
			     &info, nullptr), ST_SUCCESS);
	EXPECT_EQ(s.in_queue, 0u);
	p = 0;
	EXPECT_EQ(set(IO_PURGE, &p, 4), ST_INVALID_PARAMETER);
}

/* the buffer full: the rest turned away, PortHost::rx_room when there is room */
TEST_F(PortTest, ReceiveRoom)
{
	std::string big(Port::RX_SIZE + 100, 'r');
	EXPECT_EQ(rx(big), (size_t)Port::RX_SIZE);
	EXPECT_EQ(host.room, 0);
	timeouts(MAXT, 0, 0);
	Req r(Port::RX_SIZE / 2);
	port.read(&r);
	EXPECT_EQ(r.info, (size_t)Port::RX_SIZE / 2);
	EXPECT_EQ(host.room, 1);
}

/* LSRMST_INSERT: the escape doubled, the modem status in-band */
TEST_F(PortTest, LsrMstInsert)
{
	uint8_t esc = 0xff;
	ASSERT_EQ(set(IO_LSRMST_INSERT, &esc, 1), ST_SUCCESS);
	rx(std::string("a\xff" "b", 3));
	port.set_lines(LINE_DSR | LINE_CTS | LINE_DCD);
	timeouts(MAXT, 0, 0);
	Req r(16);
	port.read(&r);
	EXPECT_EQ(r.text(), std::string("a\xff\x00" "b\xff\x03\xbb", 7));
}

/* the handflow's NUL stripping */
TEST_F(PortTest, NullStripping)
{
	SerialHandflow h = { HF_DTR_CONTROL, HF_RTS_CONTROL | HF_NULL_STRIPPING,
			     0, 0 };
	set(IO_SET_HANDFLOW, &h, sizeof(h));
	rx(std::string("a\0b", 3));
	timeouts(MAXT, 0, 0);
	Req r(8);
	port.read(&r);
	EXPECT_EQ(r.text(), "ab");
}

/* the settings kept and refused, the properties, the counters */
TEST_F(PortTest, Settings)
{
	uint32_t baud = 57600;
	int st = host.state;
	ASSERT_EQ(set(IO_SET_BAUD_RATE, &baud, 4), ST_SUCCESS);
	EXPECT_EQ(get32(IO_GET_BAUD_RATE), 57600u);
	EXPECT_EQ(port.baud(), 57600u);
	EXPECT_EQ(host.state, st + 1);
	baud = 0;
	EXPECT_EQ(set(IO_SET_BAUD_RATE, &baud, 4), ST_INVALID_PARAMETER);

	SerialLineControl lc = { 0, 2, 7 }, got;
	ASSERT_EQ(set(IO_SET_LINE_CONTROL, &lc, 3), ST_SUCCESS);
	size_t info;
	port.ioctl(IO_GET_LINE_CONTROL, nullptr, 0, &got, 3, &info, nullptr);
	EXPECT_EQ(got.word_length, 7);
	EXPECT_EQ(got.parity, 2);
	lc = { 1, 0, 8 };              /* (1.5 stop bits: 5 data bits only) */
	EXPECT_EQ(set(IO_SET_LINE_CONTROL, &lc, 3), ST_INVALID_PARAMETER);

	SerialTimeouts bad = { MAXT, MAXT, MAXT, 0, 0 };
	EXPECT_EQ(set(IO_SET_TIMEOUTS, &bad, sizeof(bad)),
		  ST_INVALID_PARAMETER);
	EXPECT_EQ(set(IO_SET_TIMEOUTS, &bad, 4), ST_BUFFER_TOO_SMALL);

	SerialCommProp p;
	ASSERT_EQ(port.ioctl(IO_GET_PROPERTIES, nullptr, 0, &p, sizeof(p),
			     &info, nullptr), ST_SUCCESS);
	EXPECT_EQ(info, sizeof(p));
	EXPECT_EQ(p.max_rx_queue, (uint32_t)Port::RX_SIZE);
	EXPECT_EQ(port.ioctl(IO_GET_PROPERTIES, nullptr, 0, &p, 62, &info,
			     nullptr), ST_SUCCESS);
	EXPECT_EQ(info, 62u);
	EXPECT_EQ(get32(IO_CONFIG_SIZE), 0u);
	EXPECT_EQ(port.ioctl(0x1b0000u | 99 << 2, nullptr, 0, nullptr, 0,
			     &info, nullptr), ST_INVALID_PARAMETER);

	Req w(std::string("ATH\r"));
	port.write(&w);
	rx("OK");
	SerialStats s;
	port.ioctl(IO_GET_STATS, nullptr, 0, &s, sizeof(s), &info, nullptr);
	EXPECT_EQ(s.transmitted, 4u);
	EXPECT_EQ(s.received, 2u);
}

/* every frame, a byte at a time */
TEST(Wire, Frames)
{
	std::string out;
	wire_hello(out, "COM7");
	wire_lines(out, LINE_DSR | LINE_CTS);
	wire_state(out, STATE_OPEN | STATE_DTR, 115200);
	std::string big(WIRE_MAX_DATA + 5, 'd');
	wire_data(out, big.data(), big.size());

	WireReader rd;
	std::vector<WireReader::Frame> got;
	std::string data;
	uint16_t ver = 0;
	std::string port;
	uint8_t lines = 0, state = 0;
	uint32_t baud = 0;
	/* a byte at a time, as a pipe may give it */
	for (char c : out) {
		rd.feed(&c, 1);
		WireReader::Frame f;
		while (rd.next(f)) {
			got.push_back(f);
			if (f.type == FRAME_HELLO) {
				ASSERT_TRUE(WireReader::hello(f, &ver, &port));
			}
			if (f.type == FRAME_LINES) {
				ASSERT_TRUE(WireReader::lines(f, &lines));
			}
			if (f.type == FRAME_STATE) {
				ASSERT_TRUE(WireReader::state(f, &state,
							      &baud));
			}
			if (f.type == FRAME_DATA)
				data.append((const char *)f.p, f.n);
		}
	}
	ASSERT_EQ(got.size(), 5u);
	EXPECT_EQ(ver, WIRE_VERSION);
	EXPECT_EQ(port, "COM7");
	EXPECT_EQ(lines, LINE_DSR | LINE_CTS);
	EXPECT_EQ(state, STATE_OPEN | STATE_DTR);
	EXPECT_EQ(baud, 115200u);
	EXPECT_EQ(data, big);
	EXPECT_EQ(rd.pending(), 0u);
	EXPECT_FALSE(rd.bad());
}

/* frames that can't be: an unknown type, DATA too long, a short LINES */
TEST(Wire, Bad)
{
	WireReader rd;
	WireReader::Frame f;
	rd.feed("Z\0\0", 3);
	EXPECT_FALSE(rd.next(f));
	EXPECT_TRUE(rd.bad());

	WireReader rd2;
	uint8_t h[3] = { FRAME_DATA, 0xff, 0xff };
	rd2.feed(h, 3);
	EXPECT_FALSE(rd2.next(f));
	EXPECT_TRUE(rd2.bad());

	WireReader rd3;
	uint8_t s[3] = { FRAME_LINES, 0, 0 };
	rd3.feed(s, 3);
	ASSERT_TRUE(rd3.next(f));
	uint8_t l;
	EXPECT_FALSE(WireReader::lines(f, &l));
}

} // namespace
