/*
 *
 *    main.cpp - slmodemd: a modem behind a pseudo terminal, on SIP calls
 *
 *    The modem core (dcore::Modem) with this program as its Host: the
 *    line is a call of the SIP user agent (sip.cpp), whose audio comes and
 *    goes through a socket, 16 bit linear at 8000 Hz in frames of 20 ms;
 *    the DTE is a pty (/dev/ttySL0 links to it).
 *
 *    The behaviour is that of slmodemd's modem_main.c with its SIP driver.
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <libgen.h>
#include <pwd.h>
#include <grp.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <dcore.h>
#include <log.h>
#include <dsplibs.h>

#include "datafile.h"
#include "options.h"
#include "pty.h"
#include "sip.h"

#define INFO(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "main: " fmt, ##__VA_ARGS__)

#define SLMODEMD_USER "nobody"
#define LOCKED_MEM_MIN_KB (8UL * 1024)
#define LOCKED_MEM_MIN    (LOCKED_MEM_MIN_KB * 1024)

#define RATE 8000

extern int  dp_dummy_init(void);
extern void dp_dummy_exit(void);
extern int  dp_sinus_init(void);
extern void dp_sinus_exit(void);

namespace {

volatile sig_atomic_t keep_running = 1;

void mark_termination(int signum)
{
	(void)signum;
	keep_running = 0;
}

unsigned long monotonic_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000UL + ts.tv_nsec / 1000000;
}

/* the line: a SIP call's audio socket, while the modem is off hook */
struct Slmodemd final : public dcore::Host {
	slm::Pty pty;
	dcore::Modem *m = nullptr;
	int fd = -1;            /* the call's audio */
	int delay = 0;          /* samples on their way */
	int pending_delay = 0;  /* the data pumps' delay change, left */
	int law_set = 0;

	int start(dcore::Modem &modem) override {
		int sockets[2], ret;
		DBG("socket_start...\n");
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == -1) {
			ERR("socketpair: %s\n", strerror(errno));
			return -1;
		}
		if (modem.caller())
			sip::dial(modem.dial_string(), sockets[0]);
		else
			sip::answer(sockets[0]);
		/* prime the socket with a frame (20 ms) */
		fd = sockets[1];
		delay = 0;
		pending_delay = 0;
		law_set = 0;
		short silence[RATE / 50] = {};
		ret = ::write(fd, silence, sizeof(silence));
		if (ret < 0) {
			::close(fd);
			fd = -1;
			return ret;
		}
		delay = ret / 2;
		return 0;
	}
	int stop(dcore::Modem &) override {
		DBG("socket_stop...\n");
		if (fd >= 0)
			::close(fd);
		fd = -1;
		return 0;
	}
	/* the old driver's codec type (CODEC_STLC7550) */
	int codec_type(dcore::Modem &) override { return CODEC_STLC7550; }
	const char *driver_name() override { return "socket driver"; }
	int dte_write(dcore::Modem &, const char *buf, int n) override {
		return pty.write(buf, n);
	}
	unsigned long now_ms() override { return monotonic_ms(); }

	int run(int ctl_fd);
	int audio();
	int ctl_read(int ctl_fd);
};

char ctl_buf[256];
int ctl_len;

/* RING and CANCEL lines from the SIP user agent */
int Slmodemd::ctl_read(int ctl_fd)
{
	char *nl;
	int n = ::read(ctl_fd, ctl_buf + ctl_len, sizeof(ctl_buf) - 1 - ctl_len);
	if (n <= 0) {
		ERR("the SIP user agent has gone\n");
		return -1;
	}
	ctl_len += n;
	ctl_buf[ctl_len] = 0;
	while ((nl = strchr(ctl_buf, '\n'))) {
		*nl = 0;
		DBG("listen: %s\n", ctl_buf);
		if (!strcmp(ctl_buf, "RING"))
			m->ring_signal(true);
		else if (!strcmp(ctl_buf, "CANCEL"))
			m->ring_signal(false);
		ctl_len -= nl + 1 - ctl_buf;
		memmove(ctl_buf, nl + 1, ctl_len + 1);
	}
	if (ctl_len == (int)sizeof(ctl_buf) - 1)
		ctl_len = 0;
	return 0;
}

/* a frame of the call's audio in, one out */
int Slmodemd::audio()
{
	short in[2048], out[2048];
	int count = ::read(fd, in, sizeof(in));
	if (count <= 0) {
		if (count < 0 && errno == ECONNRESET)
			DBG("lost connection to child socket process\n");
		else if (count < 0)
			ERR("dev read: %s\n", strerror(errno));
		/* the call has gone: hang up now */
		m->hangup_now();
		return 0;
	}
	count /= 2;
	short *p = in;
	pending_delay += m->take_update_delay();
	if (pending_delay < 0) {
		if (-pending_delay >= count) {
			DBG("change delay -%d...\n", count);
			delay -= count;
			pending_delay += count;
			return 0;
		}
		DBG("change delay %d...\n", pending_delay);
		p -= pending_delay;
		count += pending_delay;
		delay += pending_delay;
		pending_delay = 0;
	}
	if (!law_set && m->dp_runtime() && sip::law()) {
		dp_runtime_set_law(m->dp_runtime(), sip::law());
		law_set = 1;
	}
	m->process(p, out, count);
	if (fd < 0) {
		DBG("closed connection to child socket process\n");
		return 0;
	}
	if (::write(fd, out, count * 2) < 0) {
		ERR("dev write: %s\n", strerror(errno));
		return -1;
	}
	pending_delay += m->take_update_delay();
	if (pending_delay > 0) {
		short z[2048] = {};
		int n = pending_delay > 2048 ? 2048 : pending_delay;
		DBG("change delay +%d...\n", n);
		if (::write(fd, z, n * 2) != n * 2) {
			ERR("cannot update delay: %d.\n", n);
			return -1;
		}
		delay += n;
		pending_delay -= n;
	}
	return 0;
}

int Slmodemd::run(int ctl_fd)
{
	while (keep_running) {
		struct timeval tmo;
		fd_set rset;
		int max_fd = -1, ret;
		long next;

		m->poll();
		next = m->next_timer_ms();
		if (next < 0 || next > 1000)
			next = 1000;
		tmo.tv_sec = next / 1000;
		tmo.tv_usec = next % 1000 * 1000;
		FD_ZERO(&rset);
		if (m->started() && fd >= 0) {
			FD_SET(fd, &rset);
			max_fd = fd;
		}
		if (ctl_fd >= 0) {
			FD_SET(ctl_fd, &rset);
			if (ctl_fd > max_fd)
				max_fd = ctl_fd;
		}
		if (pty.want_read(*m)) {
			FD_SET(pty.fd(), &rset);
			if (pty.fd() > max_fd)
				max_fd = pty.fd();
		}
		ret = select(max_fd + 1, &rset, NULL, NULL, &tmo);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			ERR("select: %s\n", strerror(errno));
			return ret;
		}
		if (ret == 0)
			continue;
		if (ctl_fd >= 0 && FD_ISSET(ctl_fd, &rset) &&
		    ctl_read(ctl_fd) < 0)
			return -1;
		if (fd >= 0 && FD_ISSET(fd, &rset) && audio() < 0)
			return -1;
		if (FD_ISSET(pty.fd(), &rset)) {
			switch (pty.read(*m)) {
			case slm::Pty::CLOSED:
				/* the DTE closed the slave: with HUPCL, that
				   hangs up, and a new pty simulates it */
				if (pty.hupcl()) {
					m->hangup();
					if (pty.reopen() < 0) {
						ERR("cannot re-create PTY.\n");
						return -1;
					}
				}
				break;
			case slm::Pty::ERROR:
				return -1;
			default:
				break;
			}
		}
	}
	return 0;
}

void drop_privileges(bool realtime)
{
	struct passwd *pwd;
	if (realtime) {
		struct rlimit limit;
		if (getrlimit(RLIMIT_MEMLOCK, &limit)) {
			ERR("getrlimit failed to read RLIMIT_MEMLOCK\n");
			exit(-1);
		}
		if (limit.rlim_cur != RLIM_INFINITY &&
		    limit.rlim_cur < LOCKED_MEM_MIN) {
			ERR("locked memory limit too low:\n");
			ERR("need %lu bytes, have %lu bytes\n", LOCKED_MEM_MIN,
			    (unsigned long)limit.rlim_cur);
			ERR("try 'ulimit -l %lu'\n", LOCKED_MEM_MIN_KB);
			exit(-1);
		}
	}
	pwd = getpwnam(SLMODEMD_USER);
	if (!pwd) {
		ERR("getpwnam " SLMODEMD_USER ": %s\n", strerror(errno));
		exit(-1);
	}
	if (setgroups(1, &pwd->pw_gid) || setgid(pwd->pw_gid) ||
	    setuid(pwd->pw_uid)) {
		ERR("setgroups or setgid %ld or setuid %ld failed: %s\n",
		    (long)pwd->pw_gid, (long)pwd->pw_uid, strerror(errno));
		exit(-1);
	}
	if (setuid(0) != -1) {
		ERR("setuid 0 succeeded after dropping privileges!\n");
		exit(-1);
	}
	DBG("dropped privileges to %ld.%ld\n", (long)pwd->pw_gid,
	    (long)pwd->pw_uid);
}

} // namespace

int main(int argc, char *argv[])
{
	slm::Options opt;
	Slmodemd s;
	char path_name[PATH_MAX], link_name[PATH_MAX];
	char *dev = nullptr;
	const char *name;
	int ctl_fd, ret;

	slm::parse_options(argc, argv, opt);
	dev = strdup(opt.dev_name);
	name = basename(dev);
	dcore::log_open(name);
	slm::print_version();

	dp_dummy_init();
	dp_sinus_init();
	prop_dp_init();

	/* the device's number names the link: /dev/ttySL0 */
	snprintf(link_name, sizeof(link_name), "/dev/ttySL%d", 0);

	dcore::Options mo;
	mo.name = name;
	mo.dev_name = opt.dev_name;
	mo.rate = RATE;
	mo.country = opt.country;
	s.m = new dcore::Modem(s, mo);
	if (!s.m->ok())
		exit(-1);
	if (s.pty.open(opt.group, opt.perm, link_name) < 0) {
		ERR("cannot create PTY.\n");
		exit(-1);
	}
	INFO("modem `%s' created. TTY is `%s'\n", name, s.pty.name());

	snprintf(path_name, sizeof(path_name), "/var/lib/slmodem/data.%s",
		 name);
	slm::datafile_load(path_name, &s.m->dsp_info());

	if (opt.ring_detector)
		INFO("the ring detector needs a telephone line: not on SIP "
		     "calls, ignored.\n");

	if (opt.realtime) {
		struct sched_param prm;
		/* not MCL_FUTURE: the SIP user agent's pools grow while it
		   runs, beyond what an unprivileged process may lock */
		if (mlockall(MCL_CURRENT))
			ERR("mlockall: %s\n", strerror(errno));
		prm.sched_priority = sched_get_priority_max(SCHED_FIFO);
		if (sched_setscheduler(0, SCHED_FIFO, &prm))
			ERR("sched_setscheduler: %s\n", strerror(errno));
		DBG("rt applyed: SCHED_FIFO, pri %d\n", prm.sched_priority);
	}

	signal(SIGINT, mark_termination);
	signal(SIGTERM, mark_termination);
	signal(SIGPIPE, SIG_IGN);

	/* started as root: on as SLMODEMD_USER; otherwise there is nothing
	   to give up */
	if (geteuid() == 0)
		drop_privileges(opt.realtime);

	INFO("Use `%s' as modem device, Ctrl+C for termination.\n",
	     *s.pty.link() ? s.pty.link() : s.pty.name());

	{
		sip::Config cfg;
		if (!sip::config_from_env(cfg, opt.listen))
			exit(-1);
		ctl_fd = sip::start(cfg);
		if (ctl_fd < 0) {
			ERR("cannot start the SIP user agent\n");
			exit(-1);
		}
		DBG("SIP: %s@%s%s\n", cfg.user, cfg.domain,
		    cfg.listen ? ", listening" : "");
	}

	ret = s.run(ctl_fd);
	sip::stop();

	slm::datafile_save(path_name, &s.m->dsp_info());
	delete s.m;
	usleep(100000);
	s.pty.close();

	dp_dummy_exit();
	dp_sinus_exit();
	prop_dp_exit();
	dcore::log_close();
	free(dev);
	return ret;
}
