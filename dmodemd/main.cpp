/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    main.cpp - dmodemd: the modem core on a SIP line, its DTE a pty
 *
 *    Started as root, it makes its pty and the link /dev/ttyDM<n> to it,
 *    opens its data file and leaves the link to a keeper process
 *    (pty.cpp) before it goes on as nobody.  Each call's audio comes
 *    from the SIP user agent (sip.cpp) on a socket pair, 20 ms frames
 *    that pace the modem; a fax call that goes over to T.38 has the
 *    modem's fax engine on the call's UDPTL instead, on our own clock.
 *    With -a the line is a softmodem's codec through ALSA instead
 *    (alsa_line.cpp): a telephone line, its samples pacing the modem.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>
#include <string>

#include <dcore.h>
#include <dsplibs.h>
#include <log.h>
#include <t38.h>

#ifdef DMODEM_ALSA
#include "alsa_line.h"
#endif
#include "datafile.h"
#include "options.h"
#include "pty.h"
#include "run_loop.h"
#include "sip.h"

#define ERR(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)
#define DBG(fmt, ...) dcore::log_printf(1, "dmodemd: " fmt, ##__VA_ARGS__)

namespace {

/* the line: 8000 samples a second, in frames of 20 ms */
const int RATE = 8000;
const int FRAME = RATE / 50;
/* the most samples one read of the call's audio takes */
const int AUDIO_MAX = 2048;

/* the SIP user agent's control lines: the longest, with its newline */
const size_t CONTROL_MAX = 255;

/* T.38: the far end's datagrams if it says nothing of them, and the
   least we take; the octets of a datagram an IFP packet's own headers
   need; the IFP packets each one repeats (redundancy); the longest
   datagram we read; the loop's longest wait while on T.38 (ms), and
   the most of its clock made up after a pause (samples) */
const int T38_DATAGRAM = 400;
const int T38_DATAGRAM_MIN = 32;
const int T38_IFP_HEADERS = 16;
const int T38_DEPTH = 3;
const int T38_RX_MAX = 2048;
const long T38_WAIT_MS = 20;
const unsigned long T38_CATCH_UP = RATE;

/* the user dmodemd goes on as, started as root, and the locked memory
   it then needs with realtime scheduling */
const char RUN_AS[] = "nobody";
const rlim_t MEMLOCK_MIN = 8UL << 20;

/* where the modems' data files are */
const char DATA_PREFIX[] = "/var/lib/dmodem/data.";

/* the modem's Host: its line a call of the SIP user agent (the call's
   audio on our end of a socket pair, or its fax on the call's UDPTL),
   its DTE the pty, its time the monotonic clock */
class SipLine final : public dcore::Host {
public:
	explicit SipLine(dmd::Pty &pty) : pty_(pty) {}
	~SipLine();
	SipLine(const SipLine &) = delete;
	SipLine &operator=(const SipLine &) = delete;

	/* Host */
	int start(dcore::Modem &m) override;
	int stop(dcore::Modem &m) override;
	const char *driver_name() override { return "SIP"; }
	/* SIP says when a call we dial is answered, and why it failed */
	bool signals_answer(dcore::Modem &) override { return true; }
	int dte_write(dcore::Modem &, const char *buf, int n) override {
		return pty_.write(buf, n);
	}
	void t38_send(dcore::Modem &m, const void *ifp) override;
	/* a hook flash: an RFC 4733 event on the call, if it has them */
	bool can_flash(dcore::Modem &) override {
		return audio_ >= 0 && sip::can_flash();
	}
	void hook_flash(dcore::Modem &, unsigned ms) override {
		sip::flash(ms);
	}
	unsigned long now_ms() override { return dcore::monotonic_ms(); }

	/* the call's audio socket, -1 if none */
	int audio_fd() const { return audio_; }
	/* it is readable: its samples through the modem and back; 0, or
	   -1 if the answer can't be written */
	int audio(dcore::Modem &m);

	/* the call's UDPTL socket while on T.38, -1 if none */
	int t38_fd() const { return t38_; }
	/* the call went over to T.38: the modem's fax engine on it */
	void t38_begin(dcore::Modem &m);
	/* off T.38: its socket closed */
	void t38_end();
	/* the UDPTL socket is readable: its IFP packets to the modem */
	void t38_receive(dcore::Modem &m);
	/* the fax engine's time on T.38: the samples due by the clock */
	void t38_clock(dcore::Modem &m);

private:
	/* an IFP packet out of UDPTL (UdptlRx::packet()'s callback) */
	static void ifp_in(void *ctx, const unsigned char *ifp, int len,
			   unsigned seq);

	dmd::Pty &pty_;
	int audio_ = -1;

	/* T.38: the socket, the far end, the version agreed, the largest
	   datagram it takes; UDPTL each way; the fax engine's clock (when
	   it started, the samples it has had since) */
	int t38_ = -1;
	struct sockaddr_storage peer_ = {};
	socklen_t peer_len_ = 0;
	int version_ = 0;
	int datagram_ = T38_DATAGRAM;
	dsp::UdptlTx tx_ = {};
	dsp::UdptlRx rx_ = {};
	unsigned long t38_start_ = 0;
	unsigned long t38_samples_ = 0;
	/* the modem t38_receive() gives the packets to */
	dcore::Modem *rx_modem_ = nullptr;
};

/* the line's sockets, if they are still open */
SipLine::~SipLine()
{
	if (audio_ >= 0)
		close(audio_);
	t38_end();
}

/* off hook: a socket pair for the call's audio, ours with a frame of
   silence in it ahead (the user agent reads our answer to each frame it
   writes, and finds this one for its first), the other end to the user
   agent to dial with or to answer the ringing call */
int SipLine::start(dcore::Modem &m)
{
	static const int16_t silence[FRAME] = {};
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		ERR("socketpair: %s\n", strerror(errno));
		return -1;
	}
	if (write(sv[0], silence, sizeof(silence)) !=
	    (ssize_t)sizeof(silence)) {
		ERR("dev write: %s\n", strerror(errno));
		close(sv[0]);
		close(sv[1]);
		return -1;
	}
	audio_ = sv[0];
	if (m.caller()) {
		DBG("off hook: dialling %s\n", m.dial_string());
		sip::dial(m.dial_string(), sv[1]);
	} else {
		DBG("off hook: answering\n");
		sip::answer(sv[1]);
	}
	return 0;
}

/* on hook: our end of the audio closed (the call ends with it), and
   T.38's */
int SipLine::stop(dcore::Modem &)
{
	DBG("on hook\n");
	if (audio_ >= 0) {
		close(audio_);
		audio_ = -1;
	}
	t38_end();
	return 0;
}

/* the call's samples into the modem, its answer back: as many samples
   as came.  On T.38 the modem's time is the clock's (t38_clock()), and
   the answer silence */
int SipLine::audio(dcore::Modem &m)
{
	int16_t in[AUDIO_MAX], out[AUDIO_MAX];
	ssize_t n = read(audio_, in, sizeof(in)), done;
	int count;

	if (n < 0 && (errno == EINTR || errno == EAGAIN))
		return 0;
	if (n <= 0) {
		/* the call is over: NO CARRIER */
		if (n < 0 && errno == ECONNRESET)
			DBG("dev read: connection reset\n");
		else if (n < 0)
			ERR("dev read: %s\n", strerror(errno));
		m.line_dropped();
		return 0;
	}
	/* (the user agent writes whole frames) */
	count = (int)n / 2;
	if (t38_ >= 0) {
		memset(out, 0, 2 * count);
	} else {
		int law = sip::law();
		if (law)
			m.set_line_law(law);
		m.process(in, out, count);
	}
	if (audio_ < 0) {
		DBG("hung up: no answer to the line's samples\n");
		return 0;
	}
	done = write(audio_, out, 2 * count);
	if (done == 2 * count)
		return 0;
	if (done < 0 && (errno == EPIPE || errno == ECONNRESET)) {
		/* the user agent ended the call as we answered */
		DBG("dev write: %s\n", strerror(errno));
		m.line_dropped();
		return 0;
	}
	ERR("dev write: %s\n", done < 0 ? strerror(errno) : "short write");
	return -1;
}

/* the call is on T.38 now (sip::t38()): its socket and far end kept,
   UDPTL set up as agreed, the fax engine started on it with its clock
   from now */
void SipLine::t38_begin(dcore::Modem &m)
{
	sip::T38 t;

	if (!sip::t38(&t)) {
		DBG("T38: the call is not on T.38\n");
		return;
	}
	t38_end();
	t38_ = t.fd;
	peer_ = t.peer;
	peer_len_ = t.peer_len;
	version_ = t.version;
	datagram_ = t.max_datagram > 0 ? t.max_datagram : T38_DATAGRAM;
	if (datagram_ < T38_DATAGRAM_MIN)
		datagram_ = T38_DATAGRAM_MIN;
	tx_.init(t.redundancy ? dsp::T38_EC_REDUNDANCY : dsp::T38_EC_NONE,
		 T38_DEPTH);
	rx_.init();
	t38_start_ = dcore::monotonic_ms();
	t38_samples_ = 0;
	if (m.t38_start(datagram_ - T38_IFP_HEADERS) < 0) {
		ERR("T.38: the modem's fax can't go over to it\n");
		t38_end();
		return;
	}
	DBG("T.38 on: version %d, datagrams of %d octets, %s\n", version_,
	    datagram_, t.redundancy ? "redundancy" : "no redundancy");
}

/* off T.38: the UDPTL socket closed */
void SipLine::t38_end()
{
	if (t38_ >= 0) {
		close(t38_);
		t38_ = -1;
	}
}

/* an IFP packet of the fax engine's: encoded for the version agreed,
   in a UDPTL datagram to the far end (one that can't be made is
   dropped) */
void SipLine::t38_send(dcore::Modem &, const void *ifp)
{
	unsigned char pkt[UDPTL_IFP_MAX], dgram[2 * UDPTL_IFP_MAX];
	int n, len, max;

	if (t38_ < 0)
		return;
	n = dsp::t38_ifp_encode(static_cast<const dsp::t38_ifp *>(ifp),
				version_, pkt, sizeof(pkt));
	if (n < 0)
		return;
	max = datagram_ < (int)sizeof(dgram) ? datagram_ : (int)sizeof(dgram);
	len = tx_.packet(pkt, n, dgram, max);
	if (len <= 0)
		return;
	if (sendto(t38_, dgram, len, 0, (const struct sockaddr *)&peer_,
		   peer_len_) < 0)
		DBG("T.38 send: %s\n", strerror(errno));
}

/* an IFP packet UDPTL gave (ctx: the SipLine), decoded for the version
   agreed and, if it is one, to the modem */
void SipLine::ifp_in(void *ctx, const unsigned char *ifp, int len,
		     unsigned)
{
	SipLine *line = static_cast<SipLine *>(ctx);
	dsp::t38_ifp p;

	/* (the modem may have hung up on the packet before) */
	if (line->t38_ < 0)
		return;
	if (!dsp::t38_ifp_decode(&p, line->version_, ifp, len))
		line->rx_modem_->t38_rx(&p);
}

/* every datagram waiting, until the socket is closed meanwhile */
void SipLine::t38_receive(dcore::Modem &m)
{
	unsigned char buf[T38_RX_MAX];

	rx_modem_ = &m;
	while (t38_ >= 0) {
		ssize_t n = recv(t38_, buf, sizeof(buf), MSG_DONTWAIT);
		if (n < 0)
			break;
		rx_.packet(buf, (int)n, ifp_in, this);
	}
}

/* the samples the clock says are due since T.38 began (a second of
   them at most made up), through the modem 160 at a time on silence:
   its fax engine's timers and packets */
void SipLine::t38_clock(dcore::Modem &m)
{
	static const int16_t silence[FRAME] = {};
	int16_t out[FRAME];
	unsigned long due;

	if (t38_ < 0)
		return;
	due = (dcore::monotonic_ms() - t38_start_) * (RATE / 1000);
	if (due - t38_samples_ > T38_CATCH_UP)
		t38_samples_ = due - T38_CATCH_UP;
	while (due - t38_samples_ >= (unsigned long)FRAME && t38_ >= 0 &&
	       m.started()) {
		m.process(silence, out, FRAME);
		t38_samples_ += FRAME;
	}
}

/* the result for a call we dialled that failed with SIP status code: busy
   (486 Busy Here, 600 Busy Everywhere, 603 Decline), no answer (408
   Request Timeout, 480 Temporarily Unavailable), else no carrier */
dcore::Result failure(int code)
{
	switch (code) {
	case 486:
	case 600:
	case 603:
		return dcore::Result::Busy;
	case 408:
	case 480:
		return dcore::Result::NoAnswer;
	default:
		return dcore::Result::NoCarrier;
	}
}

/* the SIP user agent's control descriptor: its lines (RING, CANCEL,
   T38, AUDIO, ANSWERED, FAILED), each acted on as it is complete */
class Control {
public:
	explicit Control(int fd) : fd_(fd) {}

	int fd() const { return fd_; }
	/* it is readable: what came; -1 if the user agent has gone */
	int serve(dcore::Modem &m, SipLine &line);

private:
	void act(const char *s, dcore::Modem &m, SipLine &line);

	int fd_;
	char buf_[CONTROL_MAX];
	size_t len_ = 0;
};

/* what came, onto the line kept from before: each complete line acted
   on (a line that fills the buffer without its end dropped) */
int Control::serve(dcore::Modem &m, SipLine &line)
{
	char chunk[CONTROL_MAX];
	ssize_t n = read(fd_, chunk, sizeof(chunk));

	if (n < 0 && errno == EINTR)
		return 0;
	if (n <= 0) {
		ERR("the SIP user agent has gone%s%s\n", n < 0 ? ": " : "",
		    n < 0 ? strerror(errno) : "");
		return -1;
	}
	for (ssize_t i = 0; i < n; i++) {
		if (chunk[i] == '\n') {
			buf_[len_] = '\0';
			act(buf_, m, line);
			len_ = 0;
			continue;
		}
		buf_[len_++] = chunk[i];
		if (len_ == sizeof(buf_)) {
			DBG("SIP: a control line too long, dropped\n");
			len_ = 0;
		}
	}
	return 0;
}

/* a control line */
void Control::act(const char *s, dcore::Modem &m, SipLine &line)
{
	DBG("SIP: %s\n", s);
	if (!strcmp(s, "RING")) {
		m.ring_signal(true);
	} else if (!strcmp(s, "CANCEL")) {
		m.ring_signal(false);
	} else if (!strcmp(s, "T38")) {
		if (m.started())
			line.t38_begin(m);
	} else if (!strcmp(s, "AUDIO")) {
		m.t38_stop();
		line.t38_end();
	} else if (!strcmp(s, "ANSWERED")) {
		m.line_answered();
	} else if (!strncmp(s, "FAILED ", 7)) {
		m.line_failed(failure(atoi(s + 7)));
	}
}

/* until SIGINT or SIGTERM (0) or an error (-1), each turn: the modem's
   timers, then select() on the call's audio, the control lines, the pty
   (if the modem takes its characters now) and T.38 */
int run(dcore::Modem &m, SipLine &line, dmd::Pty &pty, Control &control)
{
	dcore::Modem *const modems[] = { &m };
	dmd::RunLoop loop;

	while (dmd::RunLoop::running()) {
		long wait = dmd::RunLoop::poll(modems, 1);
		int audio = m.started() ? line.audio_fd() : -1;
		int ret;

		sip::set_fax(m.mode() == MODEM_MODE_FAX);
		loop.clear();
		if (audio >= 0)
			loop.watch(audio);
		loop.watch(control.fd());
		if (pty.want_read(m))
			loop.watch(pty.fd());
		if (line.t38_fd() >= 0) {
			loop.watch(line.t38_fd());
			if (wait > T38_WAIT_MS)
				wait = T38_WAIT_MS;
		}
		ret = loop.wait(wait);
		if (ret < 0) {
			ERR("select: %s\n", strerror(errno));
			return -1;
		}
		if (loop.ready(line.t38_fd()))
			line.t38_receive(m);
		line.t38_clock(m);
		if (ret == 0)
			continue;
		if (loop.ready(control.fd()) && control.serve(m, line) < 0)
			return -1;
		/* (the call it watched, not one started meanwhile) */
		if (audio >= 0 && audio == line.audio_fd() &&
		    loop.ready(audio) && line.audio(m) < 0)
			return -1;
		if (loop.ready(pty.fd()) && pty.serve(m, true) < 0)
			return -1;
	}
	return 0;
}

#ifdef DMODEM_ALSA
/* the loop on an ALSA line, until SIGINT or SIGTERM (0) or an error
   (-1), each turn: the modem's timers, then select() on the codec's
   capture and the pty (if the modem takes its characters now), then the
   samples that came */
int run_alsa(dcore::Modem &m, dmd::AlsaLine &line, dmd::Pty &pty)
{
	dcore::Modem *const modems[] = { &m };
	dmd::RunLoop loop;

	while (dmd::RunLoop::running()) {
		long wait = dmd::RunLoop::poll(modems, 1);
		int ret;

		loop.clear();
		line.watch(loop);
		if (pty.want_read(m))
			loop.watch(pty.fd());
		ret = loop.wait(wait);
		if (ret < 0) {
			ERR("select: %s\n", strerror(errno));
			return -1;
		}
		if (line.pump(m) < 0)
			return -1;
		if (ret > 0 && loop.ready(pty.fd()) && pty.serve(m, true) < 0)
			return -1;
	}
	return 0;
}
#endif

/* the pty's link: /dev/ttyDM<n>, n the number name ends with (0 if it
   ends with no digit) */
std::string link_path(const char *name)
{
	const char *end = name + strlen(name), *p = end;
	unsigned long n = 0;

	while (p > name && isdigit((unsigned char)p[-1]))
		p--;
	if (p < end)
		n = strtoul(p, nullptr, 10);
	return "/dev/ttyDM" + std::to_string(n);
}

/* all the memory we have now locked, and SCHED_FIFO at its highest
   priority (the threads made later have it too); what fails is only
   reported */
void go_realtime()
{
	struct sched_param sp;

	if (mlockall(MCL_CURRENT) < 0)
		ERR("cannot lock the memory: %s\n", strerror(errno));
	memset(&sp, 0, sizeof(sp));
	sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		ERR("cannot have realtime scheduling: %s\n", strerror(errno));
	else
		DBG("realtime scheduling: SCHED_FIFO, priority %d\n",
		    sp.sched_priority);
}

/* root's rights given up for RUN_AS's, for good (exits if they can't
   be); with realtime scheduling the locked memory it needs must be
   allowed first */
void drop_privileges(bool realtime)
{
	struct passwd *pw;
	gid_t gid;
	uid_t uid;

	if (realtime) {
		struct rlimit rl;
		if (getrlimit(RLIMIT_MEMLOCK, &rl) < 0) {
			ERR("getrlimit: %s\n", strerror(errno));
			exit(-1);
		}
		if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < MEMLOCK_MIN) {
			ERR("the locked memory limit is too low for "
			    "realtime scheduling\n");
			ERR("%lu octets needed, %lu allowed: ulimit -l %lu "
			    "(or -n)\n", (unsigned long)MEMLOCK_MIN,
			    (unsigned long)rl.rlim_cur,
			    (unsigned long)(MEMLOCK_MIN >> 10));
			exit(-1);
		}
	}
	pw = getpwnam(RUN_AS);
	if (!pw) {
		ERR("cannot find the user '%s'\n", RUN_AS);
		exit(-1);
	}
	gid = pw->pw_gid;
	uid = pw->pw_uid;
	if (setgroups(1, &gid) < 0 || setgid(gid) < 0 || setuid(uid) < 0) {
		ERR("cannot go on as gid %u, uid %u: %s\n", (unsigned)gid,
		    (unsigned)uid, strerror(errno));
		exit(-1);
	}
	if (setuid(0) == 0) {
		ERR("root's rights are still there after setuid(%u)\n",
		    (unsigned)uid);
		exit(-1);
	}
	DBG("running as gid %u, uid %u\n", (unsigned)gid, (unsigned)uid);
}

} // namespace

/* the modem on a pty and a SIP line (or with -a an ALSA one) until
   SIGINT or SIGTERM (0), or an error (255) */
int main(int argc, char *argv[])
{
	dmd::Options opt;
	dmd::Pty pty;
	SipLine line(pty);
#ifdef DMODEM_ALSA
	dmd::AlsaLine alsa(pty);
#endif
	dcore::Host *host = &line;
	dcore::Options mo;
	sip::Config cfg;
	const char *name, *slash;
	int ctl, ret;

	dmd::parse_options(argc, argv, opt);
	slash = strrchr(opt.name, '/');
	name = slash ? slash + 1 : opt.name;
	dcore::log_open(name);
	dmd::print_version();
	prop_dp_init();
	std::string link = link_path(opt.name);

	mo.name = name;
	mo.dev_name = opt.name;
	mo.rate = RATE;
	mo.country = opt.country;
	if (opt.alsa) {
#ifdef DMODEM_ALSA
		host = &alsa;
		mo.rate = (int)opt.rate;
#else
		ERR("-a: dmodemd was built without ALSA (libasound2-dev)\n");
		exit(-1);
#endif
	}
	std::unique_ptr<dcore::Modem> m(new dcore::Modem(*host, mo));
	if (!m->ok())
		exit(-1);

	if (pty.open(opt.group, opt.perm, link.c_str()) < 0) {
		ERR("cannot create PTY.\n");
		exit(-1);
	}
	fprintf(stderr, "modem `%s' created. TTY is `%s'\n", name, pty.name());

	/* opened as root: saved at the end as nobody */
	dmd::DataFile data((DATA_PREFIX + std::string(name)).c_str());
	data.load(&m->dsp_info());

#ifdef DMODEM_ALSA
	/* the codec opened as root (its device's group may be audio) */
	if (opt.alsa && !alsa.open(opt.alsa, opt.alsa_ctl, opt.rate))
		exit(-1);
#endif
	/* the keeper an ordinary process, forked before any thread */
	if (geteuid() == 0)
		pty.fork_keeper();
	if (opt.realtime)
		go_realtime();
	dmd::RunLoop::catch_signals();
	signal(SIGPIPE, SIG_IGN);
	if (geteuid() == 0)
		drop_privileges(opt.realtime);
	fprintf(stderr, "Use `%s' as modem device, Ctrl+C for termination.\n",
		*pty.link() ? pty.link() : pty.name());

#ifdef DMODEM_ALSA
	if (opt.alsa) {
		ret = run_alsa(*m, alsa, pty);
		data.save(&m->dsp_info());
		m.reset();
		usleep(100000);
		pty.close();
		prop_dp_exit();
		dcore::log_close();
		return ret < 0 ? 255 : 0;
	}
#endif
	if (!sip::config_from_env(cfg, opt.listen))
		exit(-1);
	ctl = sip::start(cfg);
	if (ctl < 0) {
		ERR("cannot start the SIP user agent\n");
		exit(-1);
	}
	DBG("SIP: %s@%s%s\n", cfg.user, cfg.domain,
	    cfg.listen ? ", taking calls" : "");

	Control control(ctl);
	ret = run(*m, line, pty, control);

	sip::stop();
	close(ctl);
	data.save(&m->dsp_info());
	m.reset();
	/* what the modem said last still there for the DTE to read */
	usleep(100000);
	pty.close();
	prop_dp_exit();
	dcore::log_close();
	return ret < 0 ? 255 : 0;
}
