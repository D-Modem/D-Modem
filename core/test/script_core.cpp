/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    script_core.cpp - core_script's modem: the modem core (dcore.h)
 *
 */

#include <stdlib.h>

#include <dcore.h>
#include <log.h>

#include "script.h"

extern "C" int prop_dp_init(void);

namespace {

/* the core; its Host keeps the DTE's output and has the script's
   clock */
struct ScriptCore final : public Core, public dcore::Host {
	dcore::Modem *m;
	const unsigned long *clock;
	std::string out;

	/* a modem at 8000 Hz */
	ScriptCore(const char *name, const unsigned long *ms) : clock(ms) {
		dcore::Options o;
		o.name = name;
		o.dev_name = name;
		o.rate = 8000;
		m = new dcore::Modem(*this, o);
	}
	/* the modem freed */
	~ScriptCore() override { delete m; }

	/* Host */
	s16 start(dcore::Modem &) override { return 0; }
	/* (see start()) */
	s16 stop(dcore::Modem &) override { return 0; }
	/* the DTE's output, kept */
	s32 dte_write(dcore::Modem &, const char *buf, s32 n) override {
		out.append(buf, n);
		return n;
	}
	/* the script's clock (ms) */
	uint64_t now_ms() override { return *clock; }
	/* for ATI3 */
	const char *driver_name() override { return "core_script"; }

	/* Core */
	int dte_in(const char *buf, int n) override {
		return m->dte_input(buf, n);
	}
	/* what the modem's DTE input takes now */
	int dte_space() override { return m->dte_space(); }
	/* what was kept, emptied */
	std::string dte_out() override {
		std::string s;
		s.swap(out);
		return s;
	}
	/* the modem's: whether its line is started */
	bool started() override { return m->started(); }
	/* n samples through the modem */
	void process(const int16_t *in, int16_t *o, int n) override {
		m->process(in, o, n);
	}
	/* the modem's timers */
	void poll() override { m->poll(); }
	/* a ring signal to the modem */
	void ring() override { m->ring(); }
	/* a call that rings by itself (on = false: the caller gave up) */
	void ring_signal(bool on) override { m->ring_signal(on); }
	/* the modem's call hung up */
	void hangup() override { m->hangup(); }
	/* the modem's link state, as an int */
	int link_state() override { return (int)m->link_state(); }
	/* tx_rate / 100 << 16 | rx_rate / 100 */
	unsigned rates() override {
		return m->tx_rate() / 100 << 16 | m->rx_rate() / 100;
	}
};

} // namespace

/* CORE_DEBUG: the log level; the pumps registered */
void core_init()
{
	if (getenv("CORE_DEBUG"))
		dcore::log_level = atoi(getenv("CORE_DEBUG"));
	prop_dp_init();
}

/* a modem of the core */
Core *core_create(const char *name, const unsigned long *ms)
{
	return new ScriptCore(name, ms);
}
