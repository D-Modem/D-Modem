/*
 *
 *    script_old.cpp - core_script's modem: slmodemd's old core (modem.h),
 *                     on the timers of libdmodem (dmodem_timer.cpp)
 *
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <modem.h>
#include <modem_debug.h>
#include <dsplibs.h>

#include "script.h"

int dp_dummy_init(void);
int dp_sinus_init(void);
extern void dmodem_run_timers(void *owner);
extern void dmodem_set_time(unsigned long ticks);
extern int modem_answer(struct modem *m);

const char *core_kind = "old";

#ifndef ENOIOCTLCMD
#define ENOIOCTLCMD 515
#endif

namespace {

int drv_start(struct modem *) { return 0; }
int drv_stop(struct modem *) { return 0; }
int drv_ioctl(struct modem *, unsigned int cmd, unsigned long)
{
	switch (cmd) {
	case MDMCTL_CAPABILITIES:
		return -EINVAL;
	case MDMCTL_CODECTYPE:
	case MDMCTL_IODELAY:
	case MDMCTL_HOOKSTATE:
	case MDMCTL_SPEED:
	case MDMCTL_GETFMTS:
	case MDMCTL_SETFMT:
	case MDMCTL_SETFRAGMENT:
	case MDMCTL_START:
	case MDMCTL_STOP:
	case MDMCTL_GETSTAT:
	case MDMCTL_SPEAKERVOL:
		return 0;
	default:
		return -ENOIOCTLCMD;
	}
}

struct modem_driver driver = {
	"core_script", drv_start, drv_stop, drv_ioctl,
};

struct OldCore final : public Core {
	struct modem *m;
	const unsigned long *clock;
	int fds[2];
	int16_t buf[4096];

	OldCore(const char *name, const unsigned long *ms) : clock(ms) {
		enter();
		m = modem_create(&driver, name);
		m->dev_name = name;
		/* the DTE: a pipe the harness reads */
		if (pipe(fds))
			abort();
		fcntl(fds[0], F_SETFL, O_NONBLOCK);
		fcntl(fds[1], F_SETFL, O_NONBLOCK);
		fcntl(fds[1], F_SETPIPE_SZ, 1 << 20);
		m->pty = fds[1];
	}
	~OldCore() override {
		enter();
		modem_delete(m);
		close(fds[0]);
		close(fds[1]);
	}
	void enter() { dmodem_set_time(*clock / 10); }

	int dte_in(const char *b, int n) override {
		enter();
		return modem_write(m, b, n);
	}
	int dte_space() override { return m->xmit.size - m->xmit.count; }
	std::string dte_out() override {
		std::string s;
		char b[4096];
		int n;
		while ((n = read(fds[0], b, sizeof(b))) > 0)
			s.append(b, n);
		return s;
	}
	bool started() override { return m->started; }
	void process(const int16_t *in, int16_t *out, int n) override {
		enter();
		memcpy(buf, in, n * sizeof(*in));
		modem_process(m, buf, out, n);
	}
	void poll() override {
		enter();
		dmodem_run_timers(m);
		if (m->event)
			modem_event(m);
	}
	void ring() override {
		enter();
		modem_ring(m);
	}
	void ring_signal(bool on) override {
		enter();
		modem_ring_signal(m, on);
	}
	void hangup() override {
		enter();
		modem_hangup(m);
	}
	int link_state() override { return modem_link_state(m); }
	unsigned rates() override {
		return m->tx_rate / 100 << 16 | m->rx_rate / 100;
	}
};

} // namespace

static void log_line(const char *line)
{
	fputs(line, stderr);
}

void core_init()
{
	/* the version banner and the log to stderr */
	modem_debug_hook = log_line;
	if (getenv("CORE_DEBUG"))
		modem_debug_level = dsplibs_debug_level =
			atoi(getenv("CORE_DEBUG"));
	dp_dummy_init();
	dp_sinus_init();
	prop_dp_init();
	modem_timer_init();
}

Core *core_create(const char *name, const unsigned long *ms)
{
	return new OldCore(name, ms);
}
