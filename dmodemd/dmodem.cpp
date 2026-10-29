/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dmodem.cpp - the modem as a library (see dmodem.h)
 *
 *    The modem core (../core, dcore::Modem) with this program as its
 *    Host: the audio comes and goes through dmodem_process(), the DTE
 *    through a file descriptor of the caller's, and the time is the
 *    audio's.
 *
 */

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <dcore.h>
#include <log.h>
#include "dm_types.h"
#include "dmodem.h"
#include "dmodem_g711.h"
#include "dsp.h"
#include "dsplibs.h"
#include "portable.h"

/* one modem: the core with the library as its Host */
struct dmodem final : public dcore::Host {
	dcore::Modem *m = nullptr;
	int dte_fd = -1;
	enum dmodem_law law = DMODEM_LAW_UNKNOWN;
	u64 samples = 0; /* the modem's clock: its audio */
	s32 rate = DMODEM_RATE;         /* its samples a second */
	char name[32] = {};
	/* a calling fax machine's CNG (on the line before the modem's
	   own detector, which runs as it answers) */
	dsp::CngDetect cng;

	/* the line is the caller's: nothing to start */
	s16 start(dcore::Modem &) override { return 0; }
	/* nor to stop */
	s16 stop(dcore::Modem &) override { return 0; }
	/* for ATI3 */
	const char *driver_name() override { return "dmodem"; }
	/* the program's hook flash, if it gave one */
	void (*flash)(void *, unsigned) = nullptr;
	void *flash_arg = nullptr;
	/* a hook flash if the program gave one */
	bool can_flash(dcore::Modem &) override { return flash != nullptr; }
	/* the program's hook flash, ms long */
	void hook_flash(dcore::Modem &, u16 ms) override {
		if (flash)
			flash(flash_arg, ms);
	}
	/* to the DTE's fd (non-blocking): what it took, 0 if nothing */
	s32 dte_write(dcore::Modem &, const char *buf, s32 n) override {
		int ret = dmd::fd_write(dte_fd, buf, n);
		return ret < 0 ? 0 : ret;
	}
	/* the clock: the samples processed so far, in ms */
	u64 now_ms() override {
		return samples * 1000 / static_cast<u64>(rate);
	}
};

/*
 *    the library
 */

static std::once_flag init_once;

/* the data pumps registered (once: std::call_once) */
static void init_all(void)
{
	prop_dp_init();
}

/* the pumps, once whatever the threads; 0 */
int dmodem_init(void)
{
	std::call_once(init_once, init_all);
	return 0;
}

/* the core's log hook: one for all the modems of the process */
void dmodem_set_log(void (*fn)(const char *line))
{
	dcore::log_set_hook(fn);
}

/* the core's and the pumps' log levels (globals of the process) */
void dmodem_set_debug(int level)
{
	dcore::log_level = level;
	dsplibs_debug_level = level;
}

/* the core with d as its Host at DMODEM_RATE */
struct dmodem *dmodem_create(const struct dmodem_config *cfg)
{
	return dmodem_create_rate(cfg, DMODEM_RATE);
}

/* the core with d as its Host at rate, the name cut to 31 characters;
   cfg->init goes in as the DTE's input */
struct dmodem *dmodem_create_rate(const struct dmodem_config *cfg,
				  int32_t rate)
{
	struct dmodem *d;
	dcore::Options o;

	if (rate != 8000 && rate != 9600 && rate != 48000 && rate != 96000 &&
	    rate != 192000)
		return nullptr;
	dmodem_init();
	d = new dmodem;
	d->rate = rate;
	strncpy(d->name, cfg->name ? cfg->name : "dmodem",
		sizeof(d->name) - 1);
	d->dte_fd = cfg->dte_fd;
	d->law = cfg->law;
	d->flash = cfg->hook_flash;
	d->flash_arg = cfg->hook_flash_arg;
	o.name = d->name;
	o.dev_name = d->name;
	o.rate = rate;
	/* a fax caller is the program's (dmodem_status()'s fax_caller):
	   the core finds out a V.8 fax call too */
	o.fax_detect = true;
	d->cng.init(rate);
	d->m = new dcore::Modem(*d, o);
	if (!d->m->ok()) {
		delete d->m;
		delete d;
		return nullptr;
	}
	/* (enum dmodem_law is the pumps' code: 1 mu-law, 2 A-law) */
	d->m->set_line_law(d->law);
	if (cfg->init) {
		d->m->dte_input(cfg->init, strlen(cfg->init));
		d->m->poll();
	}
	return d;
}

/* on hook first if a call is on; d->dte_fd is left open */
void dmodem_destroy(struct dmodem *d)
{
	if (!d)
		return;
	if (d->m->started())
		d->m->hangup();
	delete d->m;
	delete d;
}

/* ATA: the core's answer() */
int dmodem_answer(struct dmodem *d)
{
	return d->m->answer();
}

/* line and \r as the DTE's command line, its events run; 0 (what it
   did is the link's state), -1 for a line too long for the modem */
int dmodem_command(struct dmodem *d, const char *line)
{
	char cmd[128];
	int n = snprintf(cmd, sizeof(cmd), "%s\r", line);
	if (n < 0 || n >= static_cast<int>(sizeof(cmd)))
		return -1;
	d->m->dte_input(cmd, n);
	d->m->poll();
	return 0;
}

/* ATD<number>\r as the DTE's command line; 0 (how it goes is the
   link's state), -1 for a number too long for a command line */
int dmodem_dial(struct dmodem *d, const char *number)
{
	char cmd[128];
	int n = snprintf(cmd, sizeof(cmd), "ATD%s", number);
	if (n < 0 || n >= static_cast<int>(sizeof(cmd)) - 1)
		return -1;
	return dmodem_command(d, cmd);
}

/* ATH: the core's hangup(), its events run */
void dmodem_hangup(struct dmodem *d)
{
	d->m->hangup();
	d->m->poll();
}

/* n samples of the line: the CNG detector while not online, the core
   (silence out while on hook); the clock moves by n samples and the
   timers run */
void dmodem_process(struct dmodem *d, const s16 *in, s16 *out,
		    s32 n)
{
	dcore::Modem *m = d->m;

	/* a fax machine calling, while there is no connection */
	if (m->link_state() != dcore::LinkState::Online) {
		s32 i;
		for (i = 0; i < n; i++)
			d->cng.sample(in[i]);
	}
	if (m->started())
		m->process(in, out, n);
	else
		std::fill_n(out, n, 0);
	d->samples += n;
	m->poll();
}

/* G.711 octets through dmodem_process(), 160 at a time */
void dmodem_process_g711(struct dmodem *d, const unsigned char *in,
			 unsigned char *out, s32 n)
{
	auto process = [d](const s16 *li, s16 *lo, s16 k) {
		dmodem_process(d, li, lo, k);
	};
	dmodem_g711_blocks(d->law == DMODEM_LAW_ALAW, in, out, n, process);
}

/* what the DTE wrote, up to the room the core has (1024 at most): the
   bytes taken, 0 if none (or no room), -1 at end of file or on an
   error */
int dmodem_poll_dte(struct dmodem *d)
{
	dcore::Modem *m = d->m;
	char buf[1024];
	int n = m->dte_space();

	m->poll();
	if (n <= 0)
		return 0;
	if (n > static_cast<int>(sizeof(buf)))
		n = sizeof(buf);
	n = dmd::fd_read(d->dte_fd, buf, n);
	if (n < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	if (n == 0)
		return -1;
	m->dte_input(buf, n);
	m->poll();
	return n;
}

/* the core's state into *s: tx_pending one more while V.42 has frames
   unacknowledged; fax_caller latched by a CNG burst (or 0.4 s of one),
   or the core's finding (its CNG detector, a V.8 CM of a fax call) */
void dmodem_status(struct dmodem *d, struct dmodem_status *s)
{
	dcore::Modem *m = d->m;
	s->link = static_cast<enum dmodem_link>(m->link_state());
	s->offhook = m->started();
	s->tx_rate = m->tx_rate();
	s->rx_rate = m->rx_rate();
	s->tx_pending = m->xmit_count();
	/* V.42: frames not sent yet or not acknowledged */
	if (m->link_tx_pending())
		s->tx_pending++;
	s->fax_caller = d->cng.bursts > 0 || d->cng.run_ms() >= 400 ||
		m->fax_caller();
}
