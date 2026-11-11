/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dmodem_bridge_test.cpp - the PBX modules' call (frontend/pbx/
 *    dmodem_bridge.c) answering several callers at once, every call
 *    connected to one server on a unix socket (as modemmenu -serve or the
 *    BBS listen there): each call a connection of its own, its own
 *    header first, its bytes never another call's; and a server that
 *    takes no more connections costs a call its server, not the line
 *    for ever.  The same with the handoff (the modem's DTE passed to the
 *    server, SCM_RIGHTS): the server on the modem's bytes themselves
 *
 *    DMODEM_DEBUG=<level>   libdmodem's log, and the bridge's
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "dmodem_bridge.h"

#define FRAME 160               /* 20 ms */

/* V.22: online in under 6 s of line time, which runs here far faster
   than the wall's */
static const char line_ms[] = "+MS=22";

static std::mutex log_mu;
static std::vector<std::string> log_lines;

/* the bridge's log: kept for the tests, printed with DMODEM_DEBUG */
static void host_log(void *call, enum dmbridge_level level, const char *line)
{
	std::lock_guard<std::mutex> l(log_mu);
	log_lines.push_back(line);
	if (getenv("DMODEM_DEBUG"))
		printf("[%s] %d %s\n", call ? (const char *)call : "-", level,
		       line);
}

/* yes, on or 1 */
static int host_is_true(const char *v)
{
	return !strcasecmp(v, "yes") || !strcasecmp(v, "on") ||
	       !strcmp(v, "1");
}

static const struct dmbridge_host host = { host_log, host_is_true, nullptr,
					   nullptr, nullptr };

/* whether a line of the log has s */
static bool logged(const char *s)
{
	std::lock_guard<std::mutex> l(log_mu);
	for (auto &line : log_lines)
		if (line.find(s) != std::string::npos)
			return true;
	return false;
}

/* a socket path of the test's own */
static std::string socket_path(const char *name)
{
	const char *dir = getenv("TMPDIR");
	return std::string(dir && *dir ? dir : "/tmp") + "/dmbridge-" +
	       std::to_string(getpid()) + "-" + name;
}

/* a unix socket listening at path (backlog n); -1 on failure */
static int listen_at(const std::string &path, int n)
{
	struct sockaddr_un a = {};
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path.c_str());
	unlink(path.c_str());
	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) ||
	    listen(fd, n)) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}

/* what came on fd into buf (as read()), and in *got a descriptor that
   came with it (SCM_RIGHTS), if one did */
static int recv_fd(int fd, char *buf, size_t size, int *got)
{
	struct msghdr m = {};
	struct iovec v = { buf, size };
	union {
		struct cmsghdr h;
		char b[CMSG_SPACE(sizeof(int))];
	} u;
	int n;

	m.msg_iov = &v;
	m.msg_iovlen = 1;
	m.msg_control = u.b;
	m.msg_controllen = sizeof(u.b);
	n = (int)recvmsg(fd, &m, 0);
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); n >= 0 && c;
	     c = CMSG_NXTHDR(&m, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
			memcpy(got, CMSG_DATA(c), sizeof(int));
	return n;
}

/* the server: as modemmenu -serve, a connection for each call, served
   at once; its header read, then a greeting naming the call its header
   names, then each line back with that name in front, until a line
   "quit": "bye" then, and the session ends.  With handoff the modem's
   DTE comes with the header, and the caller's bytes are on it */
struct Server {
	bool handoff = false;
	std::atomic<int> fds{ 0 };      /* descriptors handed over */
	int ln = -1;
	std::atomic<bool> stop{ false };
	std::atomic<int> active{ 0 }, most{ 0 }, served{ 0 };
	std::mutex mu;
	std::set<std::string> callers;  /* the headers' DMODEM_CALLERID */
	std::vector<std::string> failures;
	std::vector<std::thread> conns;
	std::thread accepter;

	/* one connection, until the call hangs up */
	void serve(int fd)
	{
		std::string in, id;
		char buf[512];
		bool header = true, quit = false;
		int n, data = fd, passed = -1;

		served++;
		int now = ++active;
		for (int m = most; now > m && !most.compare_exchange_weak(m, now);)
			;
		while (!quit && (n = header && handoff ?
				 recv_fd(fd, buf, sizeof(buf), &passed) :
				 (int)read(data, buf, sizeof(buf))) > 0) {
			in.append(buf, n);
			if (header) {
				size_t end = in.find("\n\n");
				if (end == std::string::npos)
					continue;
				std::string h = in.substr(0, end + 1);
				in.erase(0, end + 2);
				size_t at = h.find("DMODEM_CALLERID=");
				if (at == std::string::npos) {
					fail("no DMODEM_CALLERID in [" + h + "]");
					break;
				}
				id = h.substr(at + 16, h.find('\n', at) - at - 16);
				{
					std::lock_guard<std::mutex> l(mu);
					if (!callers.insert(id).second)
						failures.push_back("caller " + id +
							" connected twice");
				}
				header = false;
				if (handoff) {
					if (passed < 0) {
						fail("caller " + id + ": no descriptor");
						break;
					}
					fds++;
					data = passed;
					/* the line's terminal: a session,
					   pppd, run on it */
					if (!isatty(data))
						fail("caller " + id + ": not a terminal");
					fcntl(data, F_SETFL,
					      fcntl(data, F_GETFL) & ~O_NONBLOCK);
					/* (what came after the header, on
					   the connection: nothing should) */
					if (!in.empty())
						fail("caller " + id + ": bytes on the connection");
				}
				std::string hello = "hello " + id + "\r\n";
				if (write(data, hello.data(), hello.size()) < 0)
					break;
			}
			size_t eol;
			while ((eol = in.find('\n')) != std::string::npos) {
				std::string line = id + ":" + in.substr(0, eol + 1);
				in.erase(0, eol + 1);
				if (line == id + ":quit\r\n") {
					line = "bye\r\n";
					quit = true;
				}
				if (write(data, line.data(), line.size()) < 0 || quit)
					break;
			}
		}
		active--;
		if (data != fd)
			close(data);
		close(fd);
	}

	/* a failure, for the test */
	void fail(const std::string &s)
	{
		std::lock_guard<std::mutex> l(mu);
		failures.push_back(s);
	}

	/* listening at path, accepting until stopped */
	bool start(const std::string &path)
	{
		ln = listen_at(path, 16);
		if (ln < 0)
			return false;
		accepter = std::thread([this] {
			while (!stop) {
				struct pollfd p = { ln, POLLIN, 0 };
				if (poll(&p, 1, 50) <= 0)
					continue;
				int fd = accept(ln, nullptr, nullptr);
				if (fd >= 0)
					conns.emplace_back(&Server::serve, this, fd);
			}
		});
		return true;
	}

	/* stopped, every connection ended */
	void end()
	{
		stop = true;
		if (accepter.joinable())
			accepter.join();
		for (auto &t : conns)
			t.join();
		if (ln >= 0)
			close(ln);
	}
};

/* one call: the caller's modem dials the bridge's, which answers and,
   once online, connects the call to the server */
struct Call {
	int id;
	std::string name;
	const struct dmbridge_profile *p;
	std::atomic<int> *ready;        /* the calls through to the server */
	int calls;                      /* how many there are */
	bool quit = false;              /* the caller ends it by "quit" */
	const char *status = "";
	std::string got;                /* what the caller's DTE received */
	std::vector<std::string> failures;
};

/* fd non-blocking */
static void nonblock(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* a call's thread: up to 90 s; the caller waits for the
   server's greeting, sends a line and waits for it back, then holds the
   line until every call has had its greeting (so that they all are on
   the server at once) and hangs up */
static void run_call(Call *k)
{
	struct dmbridge_call c;
	struct dmodem_config cfg = {};
	struct dmodem *a;
	int sa[2];
	short ab[FRAME] = { 0 }, ba[FRAME] = { 0 }, oa[FRAME], ob[FRAME];
	char init[64], buf[512];
	std::string cid = std::to_string(k->id);
	std::string hello = "hello " + cid + "\r\n";
	std::string ping = "ping " + cid + "\r\n";
	std::string pong = cid + ":" + ping;
	const char *env[] = { "DMODEM_CALLERID", cid.c_str(), "DMODEM_RATE",
			      "", nullptr };
	bool greeted = false, pinged = false, ponged = false;
	bool hung_up = false;
	enum dmbridge_event ev = DMBRIDGE_RUN;
	int n;

	socketpair(AF_UNIX, SOCK_STREAM, 0, sa);
	nonblock(sa[0]);
	nonblock(sa[1]);
	snprintf(init, sizeof(init), "ATE0Q1X3%s\r", line_ms);
	cfg.init = init;
	cfg.law = DMODEM_LAW_ULAW;
	cfg.name = "caller";
	cfg.dte_fd = sa[0];
	a = dmodem_create(&cfg);

	dmbridge_call_init(&c);
	if (dmbridge_call_start(&c, &host, (void *)k->name.c_str(), k->p,
				k->name.c_str(), DMODEM_LAW_ULAW, 0)) {
		k->failures.push_back("dmbridge_call_start failed");
		ev = DMBRIDGE_END;
	} else
		dmodem_dial(a, "");

	/* (a time limit on the wall: a call through waits for the others,
	   eight modems on fewer cores) */
	auto until = std::chrono::steady_clock::now() + std::chrono::seconds(90);
	while (ev != DMBRIDGE_END && std::chrono::steady_clock::now() < until) {
		dmodem_process(a, ba, oa, FRAME);
		dmodem_process(c.d, ab, ob, FRAME);
		memcpy(ab, oa, sizeof(ab));
		memcpy(ba, ob, sizeof(ba));
		dmodem_poll_dte(a);
		ev = dmbridge_call_step(&c, FRAME);
		if (ev == DMBRIDGE_CONNECTED) {
			snprintf(buf, sizeof(buf), "%s", c.rate);
			env[3] = buf;
			ev = dmbridge_call_serve(&c, env);
		}
		while ((n = read(sa[1], buf, sizeof(buf))) > 0)
			k->got.append(buf, n);
		if (!greeted && k->got.find(hello) != std::string::npos) {
			greeted = true;
			(*k->ready)++;
		}
		if (greeted && !pinged) {
			pinged = write(sa[1], ping.data(), ping.size()) > 0;
		}
		if (pinged && k->got.find(pong) != std::string::npos)
			ponged = true;
		/* the server is real time, the line here faster: a little
		   wall time for each frame while the server has work */
		if (c.connecting >= 0 || (greeted && *k->ready < k->calls))
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		if (ponged && *k->ready >= k->calls && !hung_up) {
			if (k->quit)
				write(sa[1], "quit\r\n", 6);
			else
				dmodem_hangup(a);
			hung_up = true;
		}
	}
	if (ev != DMBRIDGE_END) {
		dmbridge_call_hangup(&c);
		k->status = "(still up)";
	} else
		k->status = c.status;
	dmbridge_call_end(&c);
	dmodem_destroy(a);
	close(sa[0]);
	close(sa[1]);
	if (!k->p->header)
		return;
	/* nothing of another call's: the greeting and the echo are this
	   call's alone */
	for (size_t at = 0; (at = k->got.find("hello ", at)) != std::string::npos; at++)
		if (k->got.compare(at, hello.size(), hello))
			k->failures.push_back("another call's greeting: [" +
					      k->got + "]");
	if (!ponged)
		k->failures.push_back("no echo; got [" + k->got + "]");
	if (k->quit && k->got.find("bye\r\n") == std::string::npos)
		k->failures.push_back("no bye; got [" + k->got + "]");
}

/* the bridge's profile for a server at path: mode unix, the header,
   V.22, timeout s, and the handoff if handoff */
static void unix_profile(struct dmbridge_profile *p, const std::string &path,
			 int timeout, bool handoff = false)
{
	dmbridge_profile_init(p);
	ASSERT_TRUE(dmbridge_profile_option(&host, p, "test", "mode", "unix"));
	ASSERT_TRUE(dmbridge_profile_option(&host, p, "test", "path", path.c_str()));
	ASSERT_TRUE(dmbridge_profile_option(&host, p, "test", "header", "yes"));
	ASSERT_TRUE(dmbridge_profile_option(&host, p, "test", "init", line_ms));
	if (handoff)
		ASSERT_TRUE(dmbridge_profile_option(&host, p, "test", "handoff", "yes"));
	p->timeout = timeout;
	ASSERT_EQ(dmbridge_profile_check(&host, p, "test"), 0);
}

class Bridge : public testing::Test {
protected:
	/* libdmodem, its log as DMODEM_DEBUG says; a write to a gone
	   server an error, not the end of the test (as the PBXs have it) */
	void SetUp() override
	{
		dmodem_init();
		if (getenv("DMODEM_DEBUG"))
			dmodem_set_debug(atoi(getenv("DMODEM_DEBUG")));
		signal(SIGPIPE, SIG_IGN);
		std::lock_guard<std::mutex> l(log_mu);
		log_lines.clear();
	}
};

/* eight calls at once to one server, with the handoff or not; every
   failure of each, and of the server */
static void many_callers(bool handoff)
{
	const int calls = 8;
	std::string path = socket_path("many");
	struct dmbridge_profile p;
	Server srv;
	std::atomic<int> ready{ 0 };
	std::vector<Call> k(calls);
	std::vector<std::thread> th;

	srv.handoff = handoff;
	ASSERT_TRUE(srv.start(path)) << path << ": " << strerror(errno);
	unix_profile(&p, path, 20, handoff);
	for (int i = 0; i < calls; i++) {
		k[i].id = 100 + i;
		k[i].name = "call" + std::to_string(i);
		k[i].p = &p;
		k[i].ready = &ready;
		k[i].calls = calls;
		k[i].quit = handoff && i % 2;   /* half end it on the server */
		th.emplace_back(run_call, &k[i]);
	}
	for (auto &t : th)
		t.join();
	srv.end();
	unlink(path.c_str());

	for (auto &c : k) {
		for (auto &f : c.failures)
			ADD_FAILURE() << c.name << ": " << f;
		EXPECT_STREQ(c.status, c.quit ? "LOCAL" : "REMOTE") << c.name;
	}
	for (auto &f : srv.failures)
		ADD_FAILURE() << "server: " << f;
	EXPECT_EQ(srv.served, calls) << "connections to the server";
	EXPECT_EQ((int)srv.callers.size(), calls) << "distinct headers";
	EXPECT_EQ(srv.most, calls) << "calls on the server at once";
	EXPECT_EQ(srv.fds, handoff ? calls : 0) << "modems' DTEs handed over";
	if (handoff)
		EXPECT_FALSE(logged("not sent at the hangup"));
}

/* eight callers at once on one server's unix socket: each a connection of
   its own with its own header, all of them on the server together, every
   call's bytes its own; each call ends when its caller hangs up */
TEST_F(Bridge, ManyCallersOneUnixServer)
{
	many_callers(false);
}

/* the same with the handoff: each call's modem DTE passed to the server
   with its header, the bytes between the caller and the server through
   no one; half the calls end by the caller hanging up (REMOTE), half by
   the server ending the session (LOCAL, its last line delivered) */
TEST_F(Bridge, ManyCallersHandedOver)
{
	many_callers(true);
}

/* handoff with what cannot have it: mode tcp, or no header */
TEST_F(Bridge, HandoffNeedsUnixAndHeader)
{
	struct dmbridge_profile p;

	dmbridge_profile_init(&p);
	dmbridge_profile_option(&host, &p, "t", "host", "localhost");
	dmbridge_profile_option(&host, &p, "t", "port", "23");
	dmbridge_profile_option(&host, &p, "t", "header", "yes");
	dmbridge_profile_option(&host, &p, "t", "handoff", "yes");
	EXPECT_EQ(dmbridge_profile_check(&host, &p, "t"), -1);
	dmbridge_profile_option(&host, &p, "t", "mode", "unix");
	dmbridge_profile_option(&host, &p, "t", "path", "/x");
	EXPECT_EQ(dmbridge_profile_check(&host, &p, "t"), 0);
	dmbridge_profile_option(&host, &p, "t", "header", "no");
	EXPECT_EQ(dmbridge_profile_check(&host, &p, "t"), -1);
}

/* a server that has stopped taking connections (its listen queue full):
   the call ends (ERROR) at the profile's timeout, the line not held */
TEST_F(Bridge, ServerThatTakesNoMore)
{
	std::string path = socket_path("full");
	struct dmbridge_profile p;
	std::vector<int> queued;
	std::atomic<int> ready{ 0 };
	Call k;
	int ln = listen_at(path, 0);

	ASSERT_GE(ln, 0) << path << ": " << strerror(errno);
	bool full = false;

	/* connections it never accepts until its queue takes no more */
	for (int i = 0; i < 64; i++) {
		struct sockaddr_un a = {};
		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		a.sun_family = AF_UNIX;
		snprintf(a.sun_path, sizeof(a.sun_path), "%s", path.c_str());
		nonblock(fd);
		if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
			close(fd);
			full = true;
			break;
		}
		queued.push_back(fd);
	}
	if (!full) {
		/* (macOS: a unix socket's queue takes every connection, and
		   the call's connect with them) */
		for (int fd : queued)
			close(fd);
		close(ln);
		unlink(path.c_str());
		GTEST_SKIP() << "the listen queue never filled";
	}
	unix_profile(&p, path, 10);
	k.id = 1;
	k.name = "full";
	k.p = &p;
	k.ready = &ready;
	k.calls = 1;
	auto t0 = std::chrono::steady_clock::now();
	run_call(&k);
	double s = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - t0).count();
	for (int fd : queued)
		close(fd);
	close(ln);
	unlink(path.c_str());

	EXPECT_STREQ(k.status, "ERROR");
	EXPECT_TRUE(logged("cannot connect to") ||
		    logged("did not take the call"));
	EXPECT_LT(s, 30.0) << "seconds for the call to end";
}

/*
 *    voice: a person to the profile's extension, back to data, DSVD
 */

/* a voice profile: a server at path as unix_profile(), voice to 200 at
   default, listening listen s; DSVD and ASVD's voice there too if dsvd */
static void voice_profile(struct dmbridge_profile *p, const std::string &path,
			  int listen, bool dsvd)
{
	unix_profile(p, path, 30);
	ASSERT_TRUE(dmbridge_profile_option(&host, p, "t", "voice", "200@default"));
	ASSERT_TRUE(dmbridge_profile_option(&host, p, "t", "voice-listen",
					    std::to_string(listen).c_str()));
	ASSERT_TRUE(dmbridge_profile_option(&host, p, "t", "voice_dsvd",
					    dsvd ? "yes" : "no"));
}

/* a frame of a voice (a pitch moving 100 to 250 Hz, its harmonics),
   at sample i of the call */
static void speech(short *x, long i)
{
	static double ph;
	for (int k = 0; k < FRAME; k++, i++) {
		double t = (double)i / DMODEM_RATE, v = 0;
		double pitch = 175 + 75 * sin(2 * M_PI * t / 0.8);
		ph += 2 * M_PI * pitch / DMODEM_RATE;
		for (int h = 1; h * pitch < 3500; h++)
			v += 3000.0 / h * sin(h * ph);
		x[k] = (short)v;
	}
}

/* a frame of V.25's calling tone (1300 Hz) */
static void calling_tone(short *x, long i)
{
	for (int k = 0; k < FRAME; k++, i++)
		x[k] = (short)(6000 * sin(2 * M_PI * 1300 * i / DMODEM_RATE));
}

/* a person calls: speaking into the silent line, the call goes to the
   voice leg (VOICE) well before the modem would answer; there the
   caller's speech is nothing, its modem's calling tone is DATA; after
   dmbridge_call_resume() the modem answers on the same call and the
   caller's modem (dialling now) connects to the server */
TEST_F(Bridge, PersonToExtensionThenData)
{
	std::string path = socket_path("voice");
	struct dmbridge_profile p;
	struct dmbridge_call c;
	Server srv;
	short in[FRAME], out[FRAME], oa[FRAME];
	long i = 0;
	enum dmbridge_event ev = DMBRIDGE_RUN;

	ASSERT_TRUE(srv.start(path));
	voice_profile(&p, path, 3, false);
	dmbridge_call_init(&c);
	ASSERT_EQ(dmbridge_call_start(&c, &host, (void *)"v", &p, "v",
				      DMODEM_LAW_ULAW, 0), 0);
	/* listening: the modem silent */
	for (; i < 3L * DMODEM_RATE && ev == DMBRIDGE_RUN; i += FRAME) {
		speech(in, i);
		dmodem_process(c.d, in, out, FRAME);
		for (short s : out)
			ASSERT_EQ(s, 0) << "the modem spoke while listening";
		ev = dmbridge_call_step(&c, FRAME);
	}
	ASSERT_EQ(ev, DMBRIDGE_VOICE);
	EXPECT_LT(i, DMODEM_RATE) << "samples before the voice leg";
	EXPECT_STREQ(c.status, "VOICE");

	/* the voice leg: speech goes on, then a calling tone */
	for (long end = i + 2L * DMODEM_RATE; i < end; i += FRAME) {
		speech(in, i);
		dmodem_process(c.d, in, out, FRAME);
		ASSERT_EQ(dmbridge_voice_step(&c, FRAME), DMBRIDGE_RUN);
	}
	ev = DMBRIDGE_RUN;
	for (long end = i + DMODEM_RATE; i < end && ev == DMBRIDGE_RUN;
	     i += FRAME) {
		calling_tone(in, i);
		dmodem_process(c.d, in, out, FRAME);
		ev = dmbridge_voice_step(&c, FRAME);
	}
	ASSERT_EQ(ev, DMBRIDGE_DATA);

	/* back to data: the modem answers, the caller's modem dials */
	ASSERT_EQ(dmbridge_call_resume(&c), 0);
	struct dmodem_config cfg = {};
	int sa[2];
	char init[64];
	socketpair(AF_UNIX, SOCK_STREAM, 0, sa);
	nonblock(sa[0]);
	snprintf(init, sizeof(init), "ATE0Q1X3%s\r", line_ms);
	cfg.init = init;
	cfg.law = DMODEM_LAW_ULAW;
	cfg.name = "caller";
	cfg.dte_fd = sa[0];
	struct dmodem *a = dmodem_create(&cfg);
	dmodem_dial(a, "");
	memset(in, 0, sizeof(in));
	const char *env[] = { "DMODEM_CALLERID", "1", nullptr };
	ev = DMBRIDGE_RUN;
	for (int f = 0; f < 30 * 50 && ev != DMBRIDGE_END &&
	     ev != DMBRIDGE_CONNECTED; f++) {
		dmodem_process(a, out, oa, FRAME);
		dmodem_process(c.d, oa, out, FRAME);
		ev = dmbridge_call_step(&c, FRAME);
	}
	EXPECT_EQ(ev, DMBRIDGE_CONNECTED);
	if (ev == DMBRIDGE_CONNECTED)
		dmbridge_call_serve(&c, env);
	dmbridge_call_hangup(&c);
	dmbridge_call_end(&c);
	dmodem_destroy(a);
	close(sa[0]);
	close(sa[1]);
	srv.end();
	unlink(path.c_str());
}

/* a modem calls a voice profile: it sends nothing before an answer tone,
   so the line is quiet until the listening ends; then the modem answers
   and they connect, with no voice leg */
TEST_F(Bridge, ModemCallerToVoiceProfile)
{
	std::string path = socket_path("vmodem");
	struct dmbridge_profile p;
	std::atomic<int> ready{ 0 };
	Server srv;
	Call k;

	ASSERT_TRUE(srv.start(path));
	voice_profile(&p, path, 2, false);
	k.id = 7;
	k.name = "vmodem";
	k.p = &p;
	k.ready = &ready;
	k.calls = 1;
	run_call(&k);
	srv.end();
	unlink(path.c_str());
	for (auto &f : k.failures)
		ADD_FAILURE() << f;
	EXPECT_STREQ(k.status, "REMOTE");
	EXPECT_FALSE(logged("a person is calling"));
	EXPECT_TRUE(logged("the modem answers"));
}

/* the caller's side of a DSVD voice channel: its state, a tone it
   speaks, what it hears */
struct Phone {
	int on = 0, opened = 0;
	long said = 0;
	double heard = 0;
};

/* a DSVD caller (V.8 bis, DSVD offered, a voice path of its own) and the
   bridge with DSVD to the extension: online, the caller's voice channel
   opening is DSVD_OPEN; its speech comes out of dmbridge_dsvd_ear(), the
   extension's (dmbridge_dsvd_mic()) reaches it; dmbridge_dsvd_close()
   closes the channel at both ends and the data goes on */
TEST_F(Bridge, DsvdVoiceToExtension)
{
	std::string path = socket_path("dsvd");
	struct dmbridge_profile p;
	struct dmbridge_call c;
	Server srv;
	Phone ph;
	struct dmodem_voice v = {};
	struct dmodem_config cfg = {};
	short ab[FRAME] = {}, ba[FRAME] = {}, oa[FRAME], ob[FRAME];
	int sa[2];
	enum dmbridge_event ev = DMBRIDGE_RUN;
	const char *env[] = { "DMODEM_CALLERID", "9", nullptr };

	ASSERT_TRUE(srv.start(path));
	voice_profile(&p, path, 3, true);
	/* (the modulation V.8 picks: two D-Modems on G.711, V.91) */
	p.init[0] = '\0';
	socketpair(AF_UNIX, SOCK_STREAM, 0, sa);
	nonblock(sa[0]);
	nonblock(sa[1]);
	cfg.init = "ATE0Q1X3+A8E=1,1,,1+DSVD=1\r";
	cfg.law = DMODEM_LAW_ULAW;
	cfg.name = "dsvd-caller";
	cfg.dte_fd = sa[0];
	struct dmodem *a = dmodem_create(&cfg);
	v.state = [](void *arg, int on) {
		Phone *q = (Phone *)arg;
		q->on = on;
		q->opened += on;
	};
	v.mic = [](void *arg, int16_t *pcm, int n) {
		Phone *q = (Phone *)arg;
		for (int k = 0; k < n; k++, q->said++)
			pcm[k] = (int16_t)(8000 * sin(2 * M_PI * 440 *
						    q->said / DMODEM_RATE));
		return n;
	};
	v.ear = [](void *arg, const int16_t *pcm, int n) {
		Phone *q = (Phone *)arg;
		for (int k = 0; k < n; k++)
			q->heard += (double)pcm[k] * pcm[k];
	};
	v.arg = &ph;
	dmodem_set_voice(a, &v);

	dmbridge_call_init(&c);
	ASSERT_EQ(dmbridge_call_start(&c, &host, (void *)"d", &p, "d",
				      DMODEM_LAW_ULAW, 0), 0);
	dmodem_dial(a, "");
	int16_t pcm[FRAME];
	double ext_heard = 0;
	bool opened = false, closed = false;
	long frames_open = 0;
	for (int f = 0; f < 90 * 50 && ev != DMBRIDGE_END; f++) {
		dmodem_process(a, ba, oa, FRAME);
		dmodem_process(c.d, ab, ob, FRAME);
		memcpy(ab, oa, sizeof(ab));
		memcpy(ba, ob, sizeof(ba));
		dmodem_poll_dte(a);
		ev = dmbridge_call_step(&c, FRAME);
		if (ev == DMBRIDGE_CONNECTED)
			ev = dmbridge_call_serve(&c, env);
		ASSERT_NE(ev, DMBRIDGE_VOICE) << "a modem taken for a person";
		if (ev == DMBRIDGE_DSVD_OPEN)
			opened = true;
		if (ev == DMBRIDGE_DSVD_CLOSED)
			ADD_FAILURE() << "closed before we closed it";
		if (!opened || closed)
			continue;
		/* the extension: hears the caller, speaks a tone */
		int n = dmbridge_dsvd_ear(&c, pcm, FRAME);
		for (int k = 0; k < n; k++)
			ext_heard += (double)pcm[k] * pcm[k];
		for (int k = 0; k < FRAME; k++)
			pcm[k] = (int16_t)(8000 * sin(2 * M_PI * 1000 *
				(frames_open * FRAME + k) / DMODEM_RATE));
		dmbridge_dsvd_mic(&c, pcm, FRAME);
		/* 3 s of talk, then the extension hangs up */
		if (++frames_open == 150) {
			dmbridge_dsvd_close(&c);
			closed = true;
		}
		if (closed)
			break;
	}
	ASSERT_TRUE(opened) << "no voice channel";
	EXPECT_TRUE(closed);
	EXPECT_EQ(ph.opened, 1);
	EXPECT_GT(ext_heard, 1e9) << "the extension heard the caller";
	EXPECT_GT(ph.heard, 1e9) << "the caller heard the extension";
	/* the close reaches the caller; the data goes on */
	for (int f = 0; f < 100; f++) {
		dmodem_process(a, ba, oa, FRAME);
		dmodem_process(c.d, ab, ob, FRAME);
		memcpy(ab, oa, sizeof(ab));
		memcpy(ba, ob, sizeof(ba));
		ev = dmbridge_call_step(&c, FRAME);
		EXPECT_NE(ev, DMBRIDGE_END);
		EXPECT_NE(ev, DMBRIDGE_DSVD_CLOSED);
	}
	EXPECT_EQ(ph.on, 0) << "the caller's voice channel still open";
	struct dmodem_status st;
	dmodem_status(c.d, &st);
	EXPECT_EQ(st.link, DMODEM_ONLINE);
	dmbridge_call_hangup(&c);
	dmbridge_call_end(&c);
	dmodem_destroy(a);
	close(sa[0]);
	close(sa[1]);
	srv.end();
	unlink(path.c_str());
}
