/*
 *
 *    core_test - the new modem core by itself: its Host interface, the
 *                timers, the options, a line that fails, and two modems
 *                back to back with data both ways, byte for byte
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <dcore.h>
#include <log.h>

int dp_dummy_init(void);
int dp_sinus_init(void);
extern "C" int prop_dp_init(void);

using dcore::LinkState;
using dcore::Modem;

static int failures;

#define CHECK(cond) do { if (!(cond)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	failures++; } } while (0)

static unsigned long now;

struct Line final : public dcore::Host {
	std::string out;
	int fail_start = 0, fail_hook = 0;
	int starts = 0, stops = 0, last_hook = -1, volume = -1;
	int start(Modem &) override {
		starts++;
		return fail_start;
	}
	int stop(Modem &) override {
		stops++;
		return 0;
	}
	int set_hook(Modem &, int state) override {
		if (fail_hook)
			return -1;
		last_hook = state;
		return 0;
	}
	void speaker(Modem &, int v) override { volume = v; }
	int dte_write(Modem &, const char *buf, int n) override {
		out.append(buf, n);
		return n;
	}
	unsigned long now_ms() override { return now; }
};

static Modem *make(Line &l, const char *name, const char *country = nullptr)
{
	dcore::Options o;
	o.name = name;
	o.rate = 8000;
	o.country = country;
	return new Modem(l, o);
}

static std::string cmd(Modem *m, Line &l, const char *s)
{
	l.out.clear();
	m->dte_input(s, strlen(s));
	if (getenv("SHOW")) {
		printf("[");
		for (char c : l.out)
			printf(c == '\r' ? "\\r" : c == '\n' ? "\\n" : "%c", c);
		printf("]\n");
	}
	return l.out;
}

static void test_options()
{
	Line l;
	Modem *m = make(l, "uk", "UK");
	CHECK(m->ok());
	CHECK(cmd(m, l, "ATI7\r") == "ATI7\r\r\nCountry: UK\r\nOK\r\n");
	delete m;
	m = make(l, "xx", "NOWHERE");
	CHECK(cmd(m, l, "ATE0I7\r") == "ATE0I7\r\r\nCountry: USA\r\nOK\r\n");
	CHECK(m->sreg(SREG_ECHO) == 0);
	CHECK(m->set_sreg(300, 1) == -1);
	CHECK(m->sreg(300) == -1);
	CHECK(m->next_timer_ms() == -1);
	CHECK(m->link_state() == LinkState::Idle);
	CHECK(!m->started());
	delete m;
}

static void test_failing_line()
{
	Line l;
	Modem *m = make(l, "fail");
	l.fail_start = -1;
	/* the call ends at once (NO CARRIER), the command fails */
	CHECK(cmd(m, l, "ATE0X3D\r") ==
	      "ATE0X3D\r\r\nNO CARRIER\r\nERROR\r\n");
	CHECK(!m->started());
	CHECK(m->link_state() == LinkState::Idle);
	l.fail_start = 0;
	l.fail_hook = 1;
	CHECK(cmd(m, l, "ATA\r") == "NO CARRIER\r\nERROR\r\n");
	CHECK(!m->started());
	CHECK(cmd(m, l, "ATH1\r") == "ERROR\r\n");
	l.fail_hook = 0;
	CHECK(cmd(m, l, "ATH1\r") == "OK\r\n");
	CHECK(l.last_hook == MODEM_HOOK_OFF);
	CHECK(cmd(m, l, "ATH\r") == "OK\r\n");
	CHECK(l.last_hook == MODEM_HOOK_ON);
	CHECK(cmd(m, l, "ATM2L2\r") == "OK\r\n");
	CHECK(l.volume == 2);
	delete m;
}

/* two modems back to back */
struct Pair {
	Line la, lb;
	Modem *a, *b;
	std::vector<int16_t> ia, ib, oa, ob;
	std::string got_a, got_b;       /* what each DTE received */
	static const int BLOCK = 160;
	Pair(const char *ms) {
		a = make(la, "A");
		b = make(lb, "B");
		ia.assign(BLOCK, 0);
		ib = oa = ob = ia;
		std::string s = std::string("ATE0X3") + ms + "\r";
		a->dte_input(s.data(), s.size());
		b->dte_input(s.data(), s.size());
		la.out.clear();
		lb.out.clear();
	}
	~Pair() {
		delete a;
		delete b;
	}
	void step() {
		if (a->started())
			a->process(ia.data(), oa.data(), BLOCK);
		else
			std::fill(oa.begin(), oa.end(), 0);
		if (b->started())
			b->process(ib.data(), ob.data(), BLOCK);
		else
			std::fill(ob.begin(), ob.end(), 0);
		ia = ob;
		ib = oa;
		now += BLOCK / 8;
		a->poll();
		b->poll();
		got_a += la.out;
		got_b += lb.out;
		la.out.clear();
		lb.out.clear();
	}
	bool run_until(bool (*cond)(Pair &), int ms) {
		for (int t = 0; t < ms; t += 20) {
			if (cond(*this))
				return true;
			step();
		}
		return cond(*this);
	}
};

static std::string data(char who, int n)
{
	std::string d;
	unsigned x = who;
	while ((int)d.size() < n) {
		x = x * 1103515245 + 12345;
		/* text that compresses, and every byte value */
		d += (char)((x >> 16) & 1 ? "hello modem "[d.size() % 12] :
			    (x >> 8) & 0xff);
	}
	return d;
}

static void test_call(const char *ms, unsigned min_rate, int bytes)
{
	Pair p(ms);
	std::string da = data('a', bytes), db = data('b', bytes * 3 / 4);
	size_t sa = 0, sb = 0;

	CHECK(p.b->answer() == 0);
	CHECK(p.a->dial("") == 0);
	CHECK(p.a->link_state() == LinkState::Connecting);
	CHECK(p.a->caller() && !p.b->caller());
	CHECK(p.run_until([](Pair &q) {
		return q.a->link_state() == LinkState::Online &&
			q.b->link_state() == LinkState::Online; }, 40000));
	printf("%s: online at %u/%u bit/s\n", ms, p.a->tx_rate(),
	       p.a->rx_rate());
	CHECK(p.a->tx_rate() >= min_rate && p.b->rx_rate() >= min_rate);
	CHECK(p.a->carrier() && p.b->carrier());
	CHECK(p.got_a.find("CONNECT") != std::string::npos);
	p.got_a.clear();
	p.got_b.clear();
	/* the data, both ways, as fast as the modems take it */
	for (int t = 0; t < 120000; t += 20) {
		int n = p.a->dte_space();
		if (n > (int)(da.size() - sa))
			n = da.size() - sa;
		sa += p.a->dte_input(da.data() + sa, n);
		n = p.b->dte_space();
		if (n > (int)(db.size() - sb))
			n = db.size() - sb;
		sb += p.b->dte_input(db.data() + sb, n);
		p.step();
		if (p.got_b.size() >= da.size() && p.got_a.size() >= db.size())
			break;
	}
	CHECK(p.got_b == da);
	CHECK(p.got_a == db);
	if (p.got_b != da || p.got_a != db)
		printf("%s: got %zu/%zu and %zu/%zu bytes\n", ms,
		       p.got_b.size(), da.size(), p.got_a.size(), db.size());
	p.run_until([](Pair &q) {
		return !q.a->tx_pending() && !q.b->tx_pending(); }, 5000);
	CHECK(!p.a->tx_pending() && !p.b->tx_pending());

	/* the escape: guard time, +++, guard time; the timer says when */
	p.run_until([](Pair &) { return false; }, 1100);
	p.a->dte_input("+++", 3);
	CHECK(p.a->next_timer_ms() > 400 && p.a->next_timer_ms() <= 500);
	CHECK(p.run_until([](Pair &q) {
		return q.got_a.find("OK") != std::string::npos; }, 1000));
	CHECK(p.a->link_state() == LinkState::Online);
	p.got_a.clear();
	/* the remote hangs up; we hear NO CARRIER */
	p.b->hangup_now();
	CHECK(!p.b->started());
	CHECK(p.b->link_state() == LinkState::Idle);
	CHECK(p.run_until([](Pair &q) {
		return !q.a->started(); }, 15000));
	CHECK(p.a->link_state() == LinkState::Idle);
	CHECK(!p.a->carrier());
	CHECK(p.la.starts == 1 && p.la.stops == 1);
	CHECK(p.got_a.find("NO CARRIER") != std::string::npos);
	if (getenv("SHOW"))
		printf("A after the hangup: [%s]\n", p.got_a.c_str());
}

static int log_lines;

static void test_log()
{
	dcore::log_set_hook([](const char *) { log_lines++; });
	dcore::log_level = 1;
	Line l;
	Modem *m = make(l, "log");
	cmd(m, l, "ATI0\r");
	delete m;
	dcore::log_level = 0;
	dcore::log_set_hook(nullptr);
	CHECK(log_lines > 0);
}

int main()
{
	dp_dummy_init();
	dp_sinus_init();
	prop_dp_init();
	test_options();
	test_failing_line();
	test_log();
	test_call("+MS=132", 14400, 20000);
	test_call("+MS=34", 28800, 40000);
	test_call("+MS=22", 1200, 4000);
	if (failures)
		printf("%d FAILED\n", failures);
	else
		printf("PASS\n");
	return failures != 0;
}
