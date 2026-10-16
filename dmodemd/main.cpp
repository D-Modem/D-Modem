/*
 * SPDX-License-Identifier: AGPL-3.0-only AND BSD-3-Clause
 * Copyright (C) 2026 Neo-Desktop
 * Parts from Smart Link's slmodemd: Copyright (c) Smart Link Ltd.,
 * under its BSD licence (LICENSES/BSD-3-Clause.txt)
 *
 *    main.cpp - dmodemd: a modem behind a pseudo terminal, on SIP calls
 *
 *    The modem core (dcore::Modem) with this program as its Host: the
 *    line is a call of the SIP user agent (sip.cpp), whose audio comes and
 *    goes through a socket, 16 bit linear at 8000 Hz in frames of 20 ms;
 *    the DTE is a pty (/dev/ttyDM<n> links to it, n the modem's number:
 *    /dev/ttyDM0 for dmodem0).
 *
 *    A fax call that goes over to T.38 has its fax engine's IFP packets
 *    in UDPTL on the call's socket for it (dsp/t38.h), and its time by
 *    the clock: silence through the modem every 20 ms.
 *
 *    Started as root, it goes on as nobody (drop_privileges()) once what
 *    needs root is set up: the data file is opened for the save at the
 *    end, and the pty's group and link in /dev are left to a process
 *    that keeps root's rights for them alone (dmd::Pty::fork_keeper()).
 *
 *    The behaviour is that of Smart Link's slmodemd's modem_main.c with
 *    its SIP driver.
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
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
#include "run_loop.h"
#include "sip.h"
#include "t38.h"

#define INFO(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "main: " fmt, ##__VA_ARGS__)

#define DMODEMD_USER "nobody"
#define LOCKED_MEM_MIN_KB (8UL * 1024)
#define LOCKED_MEM_MIN    (LOCKED_MEM_MIN_KB * 1024)

#define RATE 8000
#define FRAME (RATE / 50)       /* 20 ms */

/* T.38: the far end's datagrams if it doesn't say (octets), and the
   headers of an IFP packet in one (its type and count, and each field's
   type and length: a data field and the end's) */
#define T38_DATAGRAM 400
#define T38_IFP_HEADERS 16

namespace {

/* the line: a SIP call's audio socket, while the modem is off hook */
struct Dmodemd final : public dcore::Host {
	dmd::Pty pty;
	dcore::Modem *m = nullptr;
	int fd = -1;            /* the call's audio */

	/* T.38: the call's UDPTL socket, the far end's, the packets'
	   version and longest datagram, the fax engine's time (samples
	   since it went over, by the clock) */
	int t38_fd = -1;
	struct sockaddr_storage t38_peer = {};
	socklen_t t38_peer_len = 0;
	int t38_version = 0;
	int t38_datagram = T38_DATAGRAM;
	dsp::UdptlTx utx;
	dsp::UdptlRx urx;
	unsigned long t38_t0 = 0;
	long long t38_samples = 0;

	/* off hook: a socketpair for the call's audio, one end to the SIP user
	   agent (dial, or answer the call that rings), ours primed with 20 ms
	   of silence; < 0 on failure */
	int start(dcore::Modem &modem) override {
		int sockets[2], ret;
		DBG("off hook: the call's audio socket\n");
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
		short silence[RATE / 50] = {};
		ret = ::write(fd, silence, sizeof(silence));
		if (ret < 0) {
			::close(fd);
			fd = -1;
			return ret;
		}
		return 0;
	}
	/* on hook: our end of the socket closed, which ends the call */
	int stop(dcore::Modem &) override {
		DBG("on hook: the call's audio socket closed\n");
		if (fd >= 0)
			::close(fd);
		fd = -1;
		t38_close();
		return 0;
	}
	/* for ATI3: the line */
	const char *driver_name() override { return "SIP"; }
	/* to the pty */
	int dte_write(dcore::Modem &, const char *buf, int n) override {
		return pty.write(buf, n);
	}
	/* the monotonic clock */
	unsigned long now_ms() override { return dcore::monotonic_ms(); }
	/* the fax engine's IFP packet: in UDPTL to the far end */
	void t38_send(dcore::Modem &, const void *ifp) override;

	int run(int ctl_fd);
	int audio();
	int ctl_read(int ctl_fd);
	void t38_on();
	void t38_close();
	void t38_read();
	void t38_time();
};

/* see t38_send() */
void Dmodemd::t38_send(dcore::Modem &, const void *ifp)
{
	unsigned char buf[UDPTL_IFP_MAX], pkt[UDPTL_IFP_MAX * 2];
	int max = t38_datagram < (int)sizeof(pkt) ? t38_datagram :
		(int)sizeof(pkt);
	int n, len;

	if (t38_fd < 0)
		return;
	n = dsp::t38_ifp_encode(static_cast<const struct dsp::t38_ifp *>(ifp),
				t38_version, buf, sizeof(buf));
	if (n < 0)
		return;
	len = utx.packet(buf, n, pkt, max);
	if (len > 0 && sendto(t38_fd, pkt, len, 0,
			      reinterpret_cast<struct sockaddr *>(&t38_peer),
			      t38_peer_len) < 0)
		DBG("T.38: sendto: %s\n", strerror(errno));
}

/* the call's fax over to T.38 (a T38 line): its UDPTL from the SIP user
   agent, the fax engine on it, its time from now by the clock */
void Dmodemd::t38_on()
{
	sip::T38 t;

	if (!sip::t38(&t))
		return;
	t38_close();
	t38_fd = t.fd;
	t38_peer = t.peer;
	t38_peer_len = t.peer_len;
	t38_version = t.version;
	t38_datagram = t.max_datagram > 0 ? t.max_datagram : T38_DATAGRAM;
	if (t38_datagram < 2 * T38_IFP_HEADERS)
		t38_datagram = 2 * T38_IFP_HEADERS;
	utx.init(t.redundancy ? dsp::T38_EC_REDUNDANCY : dsp::T38_EC_NONE, 3);
	urx.init();
	t38_t0 = dcore::monotonic_ms();
	t38_samples = 0;
	if (m->t38_start(t38_datagram - T38_IFP_HEADERS) < 0) {
		ERR("T.38: the modem is not in fax mode\n");
		t38_close();
		return;
	}
	DBG("T.38: version %d, datagrams of %d octets%s\n", t38_version,
	    t38_datagram, t.redundancy ? ", redundancy" : "");
}

/* the UDPTL socket closed (the call is over, or back on the audio) */
void Dmodemd::t38_close()
{
	if (t38_fd >= 0)
		::close(t38_fd);
	t38_fd = -1;
}

/* an IFP packet out of UDPTL, to the fax engine */
static void t38_ifp_in(void *ctx, const unsigned char *ifp, int len,
		       unsigned seq)
{
	Dmodemd *s = static_cast<Dmodemd *>(ctx);
	struct dsp::t38_ifp p;
	(void)seq;
	if (!dsp::t38_ifp_decode(&p, s->t38_version, ifp, len))
		s->m->t38_rx(&p);
}

/* the UDPTL packets that came */
void Dmodemd::t38_read()
{
	unsigned char pkt[2048];
	ssize_t n;

	while (t38_fd >= 0 &&
	       (n = recv(t38_fd, pkt, sizeof(pkt), MSG_DONTWAIT)) > 0)
		urx.packet(pkt, (int)n, t38_ifp_in, this);
}

/* the fax engine's time on T.38: as many frames of silence through the
   modem as the clock has had since (a second's at most) */
void Dmodemd::t38_time()
{
	static const short silence[FRAME] = {};
	short out[FRAME];
	long long due;

	if (t38_fd < 0)
		return;
	due = (long long)(dcore::monotonic_ms() - t38_t0) * RATE / 1000;
	if (due - t38_samples > RATE)
		t38_samples = due - RATE;
	while (t38_fd >= 0 && m->started() && due - t38_samples >= FRAME) {
		m->process(silence, out, FRAME);
		t38_samples += FRAME;
	}
}

/* ctl_read()'s partial line */
char ctl_buf[256];
int ctl_len;

/* RING and CANCEL lines from the SIP user agent, and T38 and AUDIO */
int Dmodemd::ctl_read(int ctl_fd)
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
		else if (!strcmp(ctl_buf, "T38") && m->started())
			t38_on();
		else if (!strcmp(ctl_buf, "AUDIO")) {
			m->t38_stop();
			t38_close();
		}
		ctl_len -= nl + 1 - ctl_buf;
		memmove(ctl_buf, nl + 1, ctl_len + 1);
	}
	if (ctl_len == (int)sizeof(ctl_buf) - 1)
		ctl_len = 0;
	return 0;
}

/* a frame of the call's audio in, one out (on T.38 silence, the modem
   on the clock) */
int Dmodemd::audio()
{
	short in[2048], out[2048];
	int count = ::read(fd, in, sizeof(in));
	if (count <= 0) {
		if (count < 0 && errno == ECONNRESET)
			DBG("the call's audio: connection reset\n");
		else if (count < 0)
			ERR("dev read: %s\n", strerror(errno));
		/* the call has gone: hang up now */
		m->line_dropped();
		return 0;
	}
	count /= 2;
	if (t38_fd >= 0) {
		memset(out, 0, count * 2);
	} else {
		/* the call's G.711 law, once SIP has it */
		if (sip::law())
			m->set_line_law(sip::law());
		m->process(in, out, count);
	}
	if (fd < 0) {
		DBG("the call's audio: socket closed\n");
		return 0;
	}
	if (::write(fd, out, count * 2) < 0) {
		ERR("dev write: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

/* the main loop until SIGINT or SIGTERM: the timers, the SIP user
   agent's control lines, the call's audio, the pty; 0, or -1 on an
   error */
int Dmodemd::run(int ctl_fd)
{
	dmd::RunLoop loop;

	while (dmd::RunLoop::running()) {
		long next = dmd::RunLoop::poll(&m, 1);
		int ret;

		/* its calls may go over to T.38 while in fax mode */
		sip::set_fax(m->mode() == MODEM_MODE_FAX);
		loop.clear();
		if (m->started() && fd >= 0)
			loop.watch(fd);
		if (ctl_fd >= 0)
			loop.watch(ctl_fd);
		if (pty.want_read(*m))
			loop.watch(pty.fd());
		if (t38_fd >= 0) {
			/* the fax engine's next 20 ms */
			loop.watch(t38_fd);
			if (next < 0 || next > 20)
				next = 20;
		}
		ret = loop.wait(next);
		if (ret < 0) {
			ERR("select: %s\n", strerror(errno));
			return ret;
		}
		if (loop.ready(t38_fd))
			t38_read();
		t38_time();
		if (ret == 0)
			continue;
		if (loop.ready(ctl_fd) && ctl_read(ctl_fd) < 0)
			return -1;
		if (loop.ready(fd) && audio() < 0)
			return -1;
		/* the DTE closing the slave with HUPCL hangs up, and a new
		   pty simulates it */
		if (loop.ready(pty.fd()) && pty.serve(*m, true) < 0)
			return -1;
	}
	return 0;
}

/* the modem's number: the digits its name ends with (dmodem1: 1), 0 if
   none */
int dev_number(const char *name)
{
	size_t n = strlen(name);
	while (n && isdigit((unsigned char)name[n - 1]))
		n--;
	return name[n] ? atoi(name + n) : 0;
}

/* started as root: on as DMODEMD_USER, after checking (realtime) that
   the locked memory limit is enough; exits on any failure */
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
	pwd = getpwnam(DMODEMD_USER);
	if (!pwd) {
		ERR("getpwnam " DMODEMD_USER ": %s\n", strerror(errno));
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

/* dmodemd [option...] [name]: the modem on its pty, the line the SIP
   user agent; the pumps' line data kept in /var/lib/dmodem/data.<name> */
int main(int argc, char *argv[])
{
	dmd::Options opt;
	Dmodemd s;
	char path_name[PATH_MAX], link_name[PATH_MAX];
	char *dev = nullptr;
	const char *name;
	int ctl_fd, ret;

	dmd::parse_options(argc, argv, opt);
	dev = strdup(opt.name);
	name = basename(dev);
	dcore::log_open(name);
	dmd::print_version();

	prop_dp_init();

	/* the modem's number names the link: /dev/ttyDM0 for dmodem0 */
	snprintf(link_name, sizeof(link_name), "/dev/ttyDM%d",
		 dev_number(opt.name));

	dcore::Options mo;
	mo.name = name;
	mo.dev_name = opt.name;
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

	/* the data file opened now, while still root: saved at the end
	   through this descriptor */
	snprintf(path_name, sizeof(path_name), "/var/lib/dmodem/data.%s",
		 name);
	dmd::DataFile data(path_name);
	data.load(&s.m->dsp_info());

	/* started as root: the pty's group and its link in /dev are the
	   keeper's to set for each new pty (after the DTE hangs up) and the
	   link its to remove, root's rights gone here (drop_privileges
	   below) */
	if (geteuid() == 0)
		s.pty.fork_keeper();

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

	dmd::RunLoop::catch_signals();
	signal(SIGPIPE, SIG_IGN);

	/* started as root: on as DMODEMD_USER; otherwise there is nothing
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

	data.save(&s.m->dsp_info());
	delete s.m;
	usleep(100000);
	s.pty.close();

	prop_dp_exit();
	dcore::log_close();
	free(dev);
	return ret;
}
