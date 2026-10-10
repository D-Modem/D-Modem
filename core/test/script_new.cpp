/*
 *
 *    script_new.cpp - core_script's modem: the new core (dcore.h)
 *
 */

#include <stdlib.h>

#include <dcore.h>
#include <log.h>

#include "script.h"

int dp_dummy_init(void);
int dp_sinus_init(void);
extern "C" int prop_dp_init(void);

const char *core_kind = "new";

namespace {

struct NewCore final : public Core, public dcore::Host {
	dcore::Modem *m;
	const unsigned long *clock;
	std::string out;

	NewCore(const char *name, const unsigned long *ms) : clock(ms) {
		dcore::Options o;
		o.name = name;
		o.dev_name = name;
		o.rate = 8000;
		m = new dcore::Modem(*this, o);
	}
	~NewCore() override { delete m; }

	/* Host */
	int start(dcore::Modem &) override { return 0; }
	int stop(dcore::Modem &) override { return 0; }
	int dte_write(dcore::Modem &, const char *buf, int n) override {
		out.append(buf, n);
		return n;
	}
	unsigned long now_ms() override { return *clock; }
	const char *driver_name() override { return "core_script"; }

	/* Core */
	int dte_in(const char *buf, int n) override {
		return m->dte_input(buf, n);
	}
	int dte_space() override { return m->dte_space(); }
	std::string dte_out() override {
		std::string s;
		s.swap(out);
		return s;
	}
	bool started() override { return m->started(); }
	void process(const int16_t *in, int16_t *o, int n) override {
		m->process(in, o, n);
	}
	void poll() override { m->poll(); }
	void ring() override { m->ring(); }
	void ring_signal(bool on) override { m->ring_signal(on); }
	void hangup() override { m->hangup(); }
	int link_state() override { return (int)m->link_state(); }
	unsigned rates() override {
		return m->tx_rate() / 100 << 16 | m->rx_rate() / 100;
	}
};

} // namespace

void core_init()
{
	if (getenv("CORE_DEBUG"))
		dcore::log_level = atoi(getenv("CORE_DEBUG"));
	dp_dummy_init();
	dp_sinus_init();
	prop_dp_init();
}

Core *core_create(const char *name, const unsigned long *ms)
{
	return new NewCore(name, ms);
}
