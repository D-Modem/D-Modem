/*
 *
 *    interop_call.cpp - calls between two modem processes in real time
 *                       (see interop_call.h)
 *
 */

#include "interop_call.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace interop {

namespace {

using Clock = std::chrono::steady_clock;

double now()
{
	return std::chrono::duration<double>(Clock::now().time_since_epoch())
		.count();
}

void sleep_s(double s)
{
	if (s > 0)
		std::this_thread::sleep_for(std::chrono::duration<double>(s));
}

std::string env(const char *name, const std::string &def = "")
{
	const char *v = getenv(name);
	return v ? v : def;
}

std::vector<std::string> split(const std::string &s, char sep)
{
	std::vector<std::string> out;
	size_t i = 0;
	for (;;) {
		size_t j = s.find(sep, i);
		out.push_back(s.substr(i, j - i));
		if (j == std::string::npos)
			return out;
		i = j + 1;
	}
}

std::vector<std::string> commands(const std::string &s)
{
	std::vector<std::string> out;
	for (auto &c : split(s, ';'))
		if (!c.empty())
			out.push_back(c);
	return out;
}

/* printable, as Python prints bytes */
std::string show(const std::string &s)
{
	std::string out = "b'";
	for (unsigned char c : s) {
		char t[8];
		if (c == '\r')
			out += "\\r";
		else if (c == '\n')
			out += "\\n";
		else if (c == '\\' || c == '\'')
			out += std::string("\\") + (char)c;
		else if (c < 32 || c > 126) {
			snprintf(t, sizeof(t), "\\x%02x", c);
			out += t;
		} else
			out += (char)c;
	}
	return out + "'";
}

void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

/* the end of a call: done(ok) in the drivers */
struct Done {
	bool ok;
};

void make_dirs(const std::string &dir)
{
	std::string p;
	for (auto &part : split(dir, '/')) {
		p += part + "/";
		mkdir(p.c_str(), 0777);
	}
}

void cloexec(int fd, bool on)
{
	int f = fcntl(fd, F_GETFD);
	fcntl(fd, F_SETFD, on ? f | FD_CLOEXEC : f & ~FD_CLOEXEC);
}

struct Pipe {
	int r = -1, w = -1;
	Pipe()
	{
		int p[2];
		if (pipe2(p, O_CLOEXEC) == 0) {
			r = p[0];
			w = p[1];
		}
	}
};

/* a modem process: line audio on fds 3 and 4, its pty, its output in a
   log file */
struct Modem {
	pid_t pid = -1;
	int out = -1;           /* its stdout and stderr */
	std::string pty;
	std::thread logger;

	~Modem() { stop(true); }

	bool start(const std::string &exe, const std::string &name, int rfd,
		   int wfd, const std::string &debug,
		   const std::vector<std::string> &extra_env,
		   const std::string &log_path)
	{
		/* ours first: getenv takes the first */
		std::vector<std::string> envs = { "MODEM_NAME=" + name };
		for (auto &e : extra_env)
			envs.push_back(e);
		for (char **e = environ; *e; e++)
			envs.push_back(*e);
		std::vector<char *> envp, argv;
		for (auto &e : envs)
			envp.push_back((char *)e.c_str());
		envp.push_back(nullptr);
		std::string dbg = "--debug=" + debug;
		argv.push_back((char *)exe.c_str());
		argv.push_back((char *)dbg.c_str());
		argv.push_back(nullptr);
		Pipe o;
		pid = fork();
		if (pid == 0) {
			int a = fcntl(rfd, F_DUPFD, 10);
			int b = fcntl(wfd, F_DUPFD, 10);
			dup2(o.w, 1);
			dup2(o.w, 2);
			dup2(a, 3);
			dup2(b, 4);
			execve(exe.c_str(), argv.data(), envp.data());
			_exit(127);
		}
		close(o.w);
		out = o.r;
		if (pid < 0)
			return false;
		/* "created <name>: <pty>", within 10 s */
		std::string line;
		double end = now() + 10;
		static const std::regex created("created (\\S+): (\\S+)");
		while (now() < end) {
			struct pollfd p = { out, POLLIN, 0 };
			if (poll(&p, 1, 100) <= 0)
				continue;
			char c;
			if (read(out, &c, 1) != 1)
				break;
			if (c != '\n') {
				line += c;
				continue;
			}
			std::smatch m;
			if (std::regex_search(line, m, created)) {
				pty = m[2];
				break;
			}
			line.clear();
		}
		FILE *lf = fopen(log_path.c_str(), "wbe");
		int fd = out;
		logger = std::thread([fd, lf] {
			char b[4096];
			ssize_t n;
			while ((n = read(fd, b, sizeof(b))) > 0)
				if (lf) {
					fwrite(b, 1, n, lf);
					fflush(lf);
				}
			if (lf)
				fclose(lf);
		});
		return !pty.empty();
	}

	/* kill it (SIGKILL), or terminate it and give it 2 s */
	void stop(bool hard)
	{
		if (pid > 0) {
			if (hard)
				kill(pid, SIGKILL);
			else {
				kill(pid, SIGTERM);
				double end = now() + 2;
				while (now() < end &&
				       waitpid(pid, nullptr, WNOHANG) == 0)
					sleep_s(0.02);
				kill(pid, SIGKILL);
			}
			waitpid(pid, nullptr, 0);
			pid = -1;
		}
		if (logger.joinable())
			logger.join();
		if (out >= 0)
			close(out);
		out = -1;
	}
};

int open_pty(const std::string &path)
{
	int fd = open(path.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct termios t;
	if (tcgetattr(fd, &t) == 0) {
		cfmakeraw(&t);
		tcsetattr(fd, TCSAFLUSH, &t);
	}
	return fd;
}

void write_all(int fd, const std::string &s)
{
	size_t i = 0;
	while (i < s.size()) {
		ssize_t n = write(fd, s.data() + i, s.size() - i);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return;
		i += n;
	}
}

/* the DTE side of the two modems: what each has said so far */
struct Dte {
	int fd[2] = { -1, -1 };
	std::string buf[2];
	double slice;           /* the longest poll */

	explicit Dte(double slice) : slice(slice) {}
	~Dte()
	{
		for (int f : fd)
			if (f >= 0)
				close(f);
	}

	void pump_once(double t)
	{
		struct pollfd p[2] = { { fd[0], POLLIN, 0 },
				       { fd[1], POLLIN, 0 } };
		if (poll(p, 2, (int)(t * 1000)) <= 0)
			return;
		for (int k = 0; k < 2; k++)
			if (p[k].revents & POLLIN) {
				char b[4096];
				ssize_t n = read(fd[k], b, sizeof(b));
				if (n > 0)
					buf[k].append(b, n);
			} else if (p[k].revents & (POLLHUP | POLLERR))
				/* the modem has gone: do not spin */
				sleep_s(0.01);
	}

	/* reads for t seconds (one poll at least) */
	void pump(double t)
	{
		double end = now() + t;
		do
			pump_once(std::min(slice, std::max(0.0, end - now())));
		while (now() < end);
	}

	void cmd(int k, const std::string &s) { write_all(fd[k], s + "\r"); }

	bool wait_from(int k, const std::regex &pat, size_t mark, double t)
	{
		double end = now() + t;
		while (now() < end) {
			pump(slice);
			if (mark < buf[k].size() &&
			    std::regex_search(buf[k].begin() + mark,
					      buf[k].end(), pat))
				return true;
		}
		return false;
	}

	bool wait(int k, const std::regex &pat, double t)
	{
		return wait_from(k, pat, 0, t);
	}
};

const char *const NAME[2] = { "modemA", "modemB" };

/* EXPECT_B: POSIX extended, in which '.' takes newlines ((?s) is
   Python's way of saying so) */
bool log_has(const std::string &path, std::string pat)
{
	if (pat.compare(0, 4, "(?s)") == 0)
		pat.erase(0, 4);
	FILE *f = fopen(path.c_str(), "rbe");
	if (!f)
		return false;
	std::string text;
	char b[65536];
	size_t n;
	while ((n = fread(b, 1, sizeof(b), f)) > 0)
		text.append(b, n);
	fclose(f);
	std::replace(text.begin(), text.end(), '\0', ' ');
	regex_t re;
	if (regcomp(&re, pat.c_str(), REG_EXTENDED | REG_NOSUB) != 0) {
		say("EXPECT_B: bad regex %s\n", pat.c_str());
		return false;
	}
	bool ok = regexec(&re, text.c_str(), 0, nullptr, 0) == 0;
	regfree(&re);
	return ok;
}

/* BULK: text, binary octets, long repeats and noise in turn (what a
   compressor has to get right: its dictionary filling up, 8 bit
   characters, string extensions, and the Agere modem's switch to
   transparent mode on what does not compress); no XON/XOFF */
std::string bulk(size_t n, unsigned seed)
{
	static const char *const words[] = { "modem", "line", "the", "of",
		"compression", "V.44", "dictionary", "history", "string",
		"codeword", "data" };
	unsigned long long x = seed * 0x9E3779B97F4A7C15ull + 1;
	auto rnd = [&x](unsigned m) {
		x = x * 6364136223846793005ull + 1442695040888963407ull;
		return (unsigned)((x >> 33) % m);
	};
	std::string out;
	while (out.size() < n) {
		switch (out.size() / 2048 % 4) {
		case 0:
			for (int i = 0; i < 300; i++) {
				if (i)
					out += ' ';
				out += words[rnd(11)];
			}
			out += "\r\n";
			break;
		case 1:
			for (int i = 0; i < 1024; i++)
				out += (char)(128 + rnd(128));
			break;
		case 2: {
			std::string s;
			for (int i = 0; i < 16; i++)
				s += "abc"[rnd(3)];
			for (int i = 0; i < 64; i++)
				out += s;
			break;
		}
		default:
			for (int i = 0; i < 2048; i++)
				out += (char)rnd(256);
		}
	}
	out.erase(std::remove_if(out.begin(), out.end(), [](char c) {
		return c == 0x11 || c == 0x13; }), out.end());
	out.resize(std::min(out.size(), n));
	return out;
}

} // namespace

bool data_call(const std::string &exe_a, const std::string &exe_b,
	       const std::string &proto)
{
	signal(SIGPIPE, SIG_IGN);
	/* the modems take the DTE's commands at fixed samples of the line
	   (0: as they come), so that a call meets the other modem at the
	   same alignment every run */
	setenv("PIPE_SYNC", "4800", 0);
	for (const char *v : { "AGERE_SYNC", "SM56_SYNC", "SPANDSP_SYNC",
			       "PCTEL_SYNC", "HSF_SYNC" })
		setenv(v, getenv("PIPE_SYNC"), 0);
	std::string log_dir = env("LOG_DIR", ".");
	make_dirs(log_dir);
	std::string dbg = env("DBG", "3");

	Pipe ab, ba;    /* A -> B, B -> A */
	int a_in = ba.r, a_out = ab.w, b_in = ab.r, b_out = ba.w;
	std::vector<int> fds = { ab.r, ab.w, ba.r, ba.w };
	pid_t pn = -1;
	std::string net = env("NET");
	if (!net.empty()) {
		auto r = split(net, ':');
		if (r.size() != 3) {
			say("NET=%s: <rate A>:<rate B>:<law>\n", net.c_str());
			return false;
		}
		Pipe an, na, bn, nb;
		for (int f : { an.r, an.w, na.r, na.w, bn.r, bn.w, nb.r,
			       nb.w })
			fds.push_back(f);
		if (!env("WAV").empty()) {
			char *p = realpath(env("WAV").c_str(), nullptr);
			std::string w = env("WAV");
			if (!p && w[0] != '/') {
				char cwd[4096];
				if (getcwd(cwd, sizeof(cwd)))
					w = std::string(cwd) + "/" + w;
			}
			setenv("NET_WAV", p ? p : w.c_str(), 1);
			free(p);
		}
		std::string netsim = env("NETSIM", "netsim");
		std::vector<std::string> args = { netsim, r[0], r[1], r[2],
			std::to_string(an.r), std::to_string(na.w),
			std::to_string(bn.r), std::to_string(nb.w) };
		std::vector<char *> argv;
		for (auto &s : args)
			argv.push_back((char *)s.c_str());
		argv.push_back(nullptr);
		pn = fork();
		if (pn == 0) {
			for (int f : { an.r, na.w, bn.r, nb.w })
				cloexec(f, false);
			if (netsim.find('/') != std::string::npos)
				execv(netsim.c_str(), argv.data());
			else
				execvp(netsim.c_str(), argv.data());
			_exit(127);
		}
		a_in = na.r;
		a_out = an.w;
		b_in = nb.r;
		b_out = bn.w;
	}

	Dte dte(0.1);
	std::unique_ptr<Modem> m[2] = { std::make_unique<Modem>(),
					std::make_unique<Modem>() };
	int wav_marks = 0;
	bool wav = pn > 0 && !env("WAV").empty();
	auto wav_mark = [&] {
		if (wav && wav_marks < 2) {
			kill(pn, SIGUSR2);
			wav_marks++;
		}
	};
	auto cleanup = [&] {
		for (auto &x : m)
			x->stop(true);
		for (int &f : dte.fd)
			if (f >= 0) {
				close(f);
				f = -1;
			}
		for (int f : fds)
			close(f);
		fds.clear();
		if (pn > 0) {
			/* netsim ends on its inputs' end (and writes its
			   WAV then) */
			double end = now() + 5;
			while (now() < end && waitpid(pn, nullptr, WNOHANG) == 0)
				sleep_s(0.02);
			kill(pn, SIGKILL);
			waitpid(pn, nullptr, 0);
			pn = -1;
		}
	};
	auto done = [&](bool ok) {
		if (wav_marks == 1)
			wav_mark();     /* a call that failed: up to here */
		for (auto &x : m)
			x->stop(true);
		/* EXPECT_B: modemB's log must have it as well (e.g. that
		   the call took a certain path) */
		if (ok && !env("EXPECT_B").empty()) {
			sleep_s(0.5);
			if (!log_has(log_dir + "/modemB.log", env("EXPECT_B"))) {
				say("modemB's log has no %s\n",
				    env("EXPECT_B").c_str());
				ok = false;
			}
		}
		say("%s\n", ok ? "PASS" : "FAIL");
		throw Done{ ok };
	};

	bool result;
	try {
		if (!m[0]->start(exe_a, "modemA", a_in, a_out, dbg, {},
				 log_dir + "/modemA.log") ||
		    !m[1]->start(exe_b, "modemB", b_in, b_out, dbg, {},
				 log_dir + "/modemB.log")) {
			say("a modem did not start\n");
			done(false);
		}
		for (int k = 0; k < 2; k++)
			dte.fd[k] = open_pty(m[k]->pty);
		static const std::regex ok_re("OK"), ok_err("OK|ERROR"),
			connect_a("(CONNECT[^\r]*|NO CARRIER|NO ANSWER|BUSY)\r"),
			connect_b("(CONNECT[^\r]*|NO CARRIER)\r"),
			ok_cr("OK\r"),
			result_re("(OK|ERROR|CONNECT[^\r]*|NO CARRIER)\r");
		std::string proto_b = getenv("PROTO_B") ? env("PROTO_B") : proto;

		auto setup = [&](int k) {
			for (auto &c : commands(k ? proto_b : proto)) {
				dte.cmd(k, c);
				if (!dte.wait(k, ok_err, 5) ||
				    dte.buf[k].find("ERROR") != std::string::npos) {
					say("%s %s %s\n", NAME[k], c.c_str(),
					    show(dte.buf[k]).c_str());
					done(false);
				}
				dte.buf[k].clear();
			}
		};
		for (int k = 0; k < 2; k++)
			dte.cmd(k, "ATZ");
		for (int k = 0; k < 2; k++) {
			if (!dte.wait(k, ok_re, 5)) {
				say("%s %s\n", NAME[k], show(dte.buf[k]).c_str());
				done(false);
			}
			dte.buf[k].clear();
		}
		for (int k = 0; k < 2; k++)
			setup(k);

		int calls = atoi(env("CALLS", "1").c_str());
		for (int call = 0; call < calls; call++) {
			if (call) {
				/* hang up: escape both (their OK, after the
				   guard time), ATH and its OK, then wait for
				   the line to clear */
				sleep_s(1.5);
				for (int k = 0; k < 2; k++) {
					dte.buf[k].clear();
					write_all(dte.fd[k], "+++");
				}
				for (int k = 0; k < 2; k++)
					dte.wait(k, ok_re, 5);
				for (int k = 0; k < 2; k++) {
					dte.buf[k].clear();
					dte.cmd(k, "ATH");
					if (!dte.wait(k, ok_re, 10)) {
						say("%s ATH %s\n", NAME[k],
						    show(dte.buf[k]).c_str());
						done(false);
					}
				}
				sleep_s(2);
				if (!env("RESTART_B").empty()) {
					m[1]->stop(true);
					close(dte.fd[1]);
					dte.fd[1] = -1;
					m[1] = std::make_unique<Modem>();
					if (!m[1]->start(exe_b, "modemB2", b_in,
							 b_out, dbg, {},
							 log_dir + "/modemB2.log")) {
						say("modemB2 did not start\n");
						done(false);
					}
					dte.fd[1] = open_pty(m[1]->pty);
					dte.buf[1].clear();
					dte.cmd(1, "ATZ");
					if (!dte.wait(1, ok_re, 5)) {
						say("modemB ATZ %s\n",
						    show(dte.buf[1]).c_str());
						done(false);
					}
					dte.buf[1].clear();
					setup(1);
				}
				dte.buf[0].clear();
				dte.buf[1].clear();
			}
			if (!call)
				wav_mark();
			dte.cmd(1, "ATA");
			dte.cmd(0, "ATD" + env("DIAL"));
			double t_call = now();
			if (dte.wait(0, connect_a, 90))
				dte.wait(1, connect_b, 15);
			say("A: %s\n", show(dte.buf[0]).c_str());
			say("B: %s\n", show(dte.buf[1]).c_str());
			say("call %d: %.1f s to CONNECT\n", call + 1,
			    now() - t_call);
			if (dte.buf[0].find("CONNECT") == std::string::npos ||
			    dte.buf[1].find("CONNECT") == std::string::npos)
				done(false);
		}
		/* NET_SYNC: netsim's NET_DROP, NET_STEP and NET_NOISE times
		   count from here */
		if (pn > 0 && !env("NET_SYNC").empty())
			kill(pn, SIGUSR1);
		/* the slowest direction sets how long the data takes (V.23:
		   75 bit/s) */
		long rate = 0;
		static const std::regex rate_re("CONNECT (\\d+)");
		for (int k = 0; k < 2; k++)
			for (std::sregex_iterator i(dte.buf[k].begin(),
						    dte.buf[k].end(), rate_re), e;
			     i != e; ++i) {
				long r = atol((*i)[1].str().c_str());
				rate = rate ? std::min(rate, r) : r;
			}
		if (!rate)
			rate = 1200;
		/* DATA_RATE: when the CONNECTs say less (a modem that
		   gives the DTE's rate; V.23's 75 bit/s back channel) */
		if (atol(env("DATA_RATE", "0").c_str()))
			rate = atol(env("DATA_RATE").c_str());
		dte.buf[0].clear();
		dte.buf[1].clear();

		std::string msg[2];
		for (int c = 32; c < 127; c++)
			msg[0] += (char)c;
		msg[0] = msg[0] + msg[0] + msg[0] + msg[0] + "\r\n";
		for (int i = 0; i < 8; i++)
			msg[1] += "The quick brown fox jumps over the lazy dog. ";
		size_t bulk_n = atol(env("BULK", "0").c_str());
		double stream = atof(env("STREAM", "0").c_str());
		if (stream) {
			std::string sent[2];
			double t_end = now() + stream;
			struct Mid {
				double at;
				std::string cmds;
			};
			std::map<int, Mid> mids;
			for (int k = 0; k < 2; k++) {
				std::string v = env(k ? "MIDCALL_B" : "MIDCALL_A");
				size_t c = v.find(':');
				if (c != std::string::npos)
					mids[k] = { now() + atof(v.substr(0, c)
								 .c_str()),
						    v.substr(c + 1) };
			}
			for (int n = 0; now() < t_end; n++) {
				for (auto &[k, mid] : mids) {
					if (!mid.at || now() < mid.at)
						continue;
					mid.at = 0;
					char name[16];
					snprintf(name, sizeof(name), "midcall %c",
						 "AB"[k]);
					dte.pump(1.2);
					size_t kept = dte.buf[k].size();
					write_all(dte.fd[k], "+++");
					/* the remote gets it too */
					sent[k] += "+++";
					dte.wait_from(k, ok_cr, kept, 5);
					for (auto &c : split(mid.cmds, ';')) {
						if (c.compare(0, 5, "WAIT ") == 0) {
							dte.pump(atof(c.c_str() + 5));
							continue;
						}
						size_t mark = dte.buf[k].size();
						dte.cmd(k, c);
						dte.wait_from(k, result_re, mark, 30);
						say("%s: %s %s\n", name, c.c_str(),
						    show(dte.buf[k].substr(mark))
						    .c_str());
					}
					size_t mark = dte.buf[k].size();
					dte.cmd(k, "ATO");
					dte.wait_from(k, connect_b, mark, 60);
					say("%s: ATO %s\n", name,
					    show(dte.buf[k].substr(mark)).c_str());
					/* what the commands put in its buffer is
					   not data */
					dte.buf[k].resize(kept);
				}
				char line[16];
				snprintf(line, sizeof(line), "%06d ", n);
				for (int k = 0; k < 2; k++) {
					write_all(dte.fd[k], line + msg[k]);
					sent[k] += line + msg[k];
				}
				dte.pump(0.5);
			}
			msg[0] = sent[0];
			msg[1] = sent[1];
		} else if (bulk_n) {
			msg[0] = bulk(bulk_n, 1);
			msg[1] = bulk(bulk_n, 2);
			size_t at[2] = { 0, 0 };
			for (int f : dte.fd)
				fcntl(f, F_SETFL, fcntl(f, F_GETFL) | O_NONBLOCK);
			while (at[0] < msg[0].size() || at[1] < msg[1].size()) {
				for (int k = 0; k < 2; k++)
					if (at[k] < msg[k].size()) {
						ssize_t w = write(dte.fd[k],
							msg[k].data() + at[k],
							std::min<size_t>(1024,
							msg[k].size() - at[k]));
						if (w > 0)
							at[k] += w;
					}
				dte.pump(0.05);
			}
		} else
			for (int k = 0; k < 2; k++)
				write_all(dte.fd[k], msg[k]);
		/* EXPECT_DROP: the call is to end on the way (EXPECT_B says
		   how), the data is not checked */
		if (!env("EXPECT_DROP").empty()) {
			dte.pump(1);
			done(true);
		}
		/* 10 bits a character, and half as much again for V.42
		   framing */
		double end = now() + 40 + stream + 15.0 *
			std::max(msg[0].size(), msg[1].size()) / rate;
		while (now() < end && (dte.buf[1].size() < msg[0].size() ||
				       dte.buf[0].size() < msg[1].size()))
			dte.pump(0.2);
		wav_mark();
		if (wav)
			say("wav: %s\n", env("NET_WAV").c_str());
		bool ok_a = dte.buf[0] == msg[1], ok_b = dte.buf[1] == msg[0];
		say("A received %zu/%zu ok=%s\n", dte.buf[0].size(),
		    msg[1].size(), ok_a ? "True" : "False");
		say("B received %zu/%zu ok=%s\n", dte.buf[1].size(),
		    msg[0].size(), ok_b ? "True" : "False");
		done(ok_a && ok_b);
		result = false;
	} catch (const Done &d) {
		result = d.ok;
	}
	cleanup();
	return result;
}

/* --- fax ---------------------------------------------------------- */

namespace {

struct Fail {
	std::string msg;
};

[[noreturn]] void fail(const std::string &msg) { throw Fail{ msg }; }

const char *const FAX_NAME[2] = { "A", "B" };

std::string shield(const std::string &data)
{
	std::string out;
	for (char c : data) {
		out += c;
		if (c == 0x10)
			out += c;
	}
	return out + "\x10\x03";
}

/* the data before DLE ETX, whether it came, and where it ends */
std::string unshield(const std::string &data, bool *ended, size_t *end)
{
	std::string out;
	size_t i = 0;
	*ended = false;
	while (i < data.size()) {
		unsigned char c = data[i];
		if (c == 0x10 && i + 1 < data.size()) {
			unsigned char n = data[i + 1];
			i += 2;
			if (n == 0x10)
				out += (char)0x10;
			else if (n == 0x03) {
				*ended = true;
				break;
			}
			continue;
		}
		out += (char)c;
		i++;
	}
	*end = i;
	return out;
}

struct FaxDte : Dte {
	FaxDte() : Dte(0.02) {}

	/* a result code: the text before it and the code ("" if none came),
	   both taken off the buffer */
	std::string wait_code(int k, const std::regex &pat, double t,
			      std::string *before = nullptr)
	{
		double end = now() + t;
		while (now() < end) {
			pump(0.02);
			std::smatch m;
			if (std::regex_search(buf[k], m, pat)) {
				if (before)
					*before = m.prefix();
				std::string r = m[1];
				buf[k] = m.suffix();
				return r;
			}
		}
		if (before)
			*before = buf[k];
		return "";
	}
};

const std::regex &fax_result()
{
	static const std::regex r(
		"(?:\r\n)?(OK|CONNECT|NO CARRIER|ERROR|NO ANSWER|BUSY)\r\n");
	return r;
}

struct Fax {
	FaxDte dte;

	std::string cmd(int k, const std::string &s,
			const std::regex *expect = nullptr, double t = 5)
	{
		static const std::regex ok_err("(OK|ERROR)\r\n");
		dte.cmd(k, s);
		std::string r = dte.wait_code(k, expect ? *expect : ok_err, t);
		say("%s: %-12s -> %s\n", FAX_NAME[k], s.c_str(),
		    r.empty() ? "None" : r.c_str());
		return r;
	}

	/* after CONNECT: HDLC frames, the last one final */
	void send_frames(int k, const std::vector<std::string> &frames)
	{
		for (size_t i = 0; i < frames.size(); i++) {
			write_all(dte.fd[k], shield(frames[i]));
			std::string out;
			std::string r = dte.wait_code(k, fax_result(), 10, &out);
			std::string want = i == frames.size() - 1 ? "OK" :
				"CONNECT";
			say("%s: frame %s -> %s\n", FAX_NAME[k],
			    hex(frames[i].substr(0, 3)).c_str(),
			    r.empty() ? "None" : r.c_str());
			if (r != want)
				fail(std::string(FAX_NAME[k]) +
				     " sending frame: " + r + " " + show(out));
		}
	}

	/* after CONNECT: received data up to DLE ETX and the result */
	std::string recv_until_etx(int k, double t, std::string *r,
				   const char *what)
	{
		double end = now() + t;
		while (now() < end) {
			dte.pump(what ? 0.02 : 0.05);
			bool ended;
			size_t j;
			std::string data = unshield(dte.buf[k], &ended, &j);
			if (ended) {
				dte.buf[k].erase(0, j);
				*r = dte.wait_code(k, fax_result(), 5);
				return data;
			}
		}
		if (what)
			fail(std::string(FAX_NAME[k]) + ": no frame, have " +
			     show(dte.buf[k].substr(0, 80)));
		fail(std::string(FAX_NAME[k]) + ": data did not end (" +
		     std::to_string(dte.buf[k].size()) + " bytes)");
	}

	std::string recv_frame(int k, std::string *r)
	{
		std::string fr = recv_until_etx(k, 10, r, "frame");
		say("%s: got frame %s (%zu bytes) -> %s\n", FAX_NAME[k],
		    hex(fr.substr(0, 3)).c_str(), fr.size(),
		    r->empty() ? "None" : r->c_str());
		return fr;
	}

	/* frames up to the final one (control field 0x13); like fax
	   software, +FRH again after a false start */
	std::vector<std::string> recv_frames(int k)
	{
		std::vector<std::string> frames;
		int retries = 0;
		for (;;) {
			std::string r;
			std::string fr = recv_frame(k, &r);
			if (r == "OK") {
				frames.push_back(fr.substr(0, fr.size() - 2));
				if (fr.size() > 1 && (unsigned char)fr[1] == 0x13)
					return frames;
			} else if (retries < 3)
				retries++;
			else
				fail(std::string(FAX_NAME[k]) + ": bad frame " +
				     show(fr));
			r = cmd(k, "AT+FRH=3", &fax_result(), 10);
			if (r != "CONNECT")
				fail(std::string(FAX_NAME[k]) + " FRH: " + r);
		}
	}

	/* tx sends HDLC frames to rx; like T.30, a command that got no
	   answer is repeated */
	void exchange(int tx, int rx, const std::vector<std::string> &frames,
		      int tries = 3)
	{
		write_all(dte.fd[rx], "AT+FRH=3\r");
		sleep_s(0.1);
		for (int attempt = 0; attempt < tries; attempt++) {
			std::string r = cmd(tx, "AT+FTH=3", &fax_result(), 10);
			if (r != "CONNECT")
				fail(std::string(FAX_NAME[tx]) + " FTH: " + r);
			send_frames(tx, frames);
			std::vector<std::string> got;
			r = dte.wait_code(rx, fax_result(), 6);
			say("%s: AT+FRH=3 -> %s\n", FAX_NAME[rx],
			    r.empty() ? "None" : r.c_str());
			while (r == "CONNECT") {
				std::string fr = recv_frame(rx, &r);
				if (r != "OK")
					break;
				got.push_back(fr.substr(0, fr.size() - 2));
				if (fr.size() > 1 && (unsigned char)fr[1] == 0x13)
					break;
				r = cmd(rx, "AT+FRH=3", &fax_result(), 6);
			}
			if (got == frames)
				return;
			if (!r.empty())     /* gave up on a false start */
				write_all(dte.fd[rx], "AT+FRH=3\r");
			sleep_s(0.1);
		}
		fail(std::string(FAX_NAME[rx]) + " did not get the frames");
	}

	static std::string hex(const std::string &s)
	{
		std::string out;
		char t[4];
		for (unsigned char c : s) {
			snprintf(t, sizeof(t), "%02x", c);
			out += t;
		}
		return out;
	}
};

std::vector<int> bits(const std::string &data)
{
	std::vector<int> out;
	for (unsigned char c : data)
		for (int i = 0; i < 8; i++)
			out.push_back(c >> i & 1);
	return out;
}

void write_file(const std::string &path, const std::string &data)
{
	FILE *f = fopen(path.c_str(), "wbe");
	if (f) {
		fwrite(data.data(), 1, data.size(), f);
		fclose(f);
	}
}

} // namespace

bool fax_call(const std::string &exe_a, const std::string &exe_b, int mod,
	      const std::string &setup)
{
	signal(SIGPIPE, SIG_IGN);
	std::string log_dir = env("LOG_DIR", ".");
	make_dirs(log_dir);
	std::string record = env("RECORD");
	Pipe ab, ba;
	/* the line's writers first: the relays end on their end */
	std::vector<int> fds = { ab.w, ba.w, ab.r, ba.r };
	int a_in = ba.r, b_in = ab.r;
	std::vector<std::thread> relays;
	if (!record.empty()) {
		/* RECORD=<dir>: the line both ways (16 bit, the modems'
		   rate) */
		make_dirs(record);
		Pipe ra, rb;
		for (int f : { ra.r, ra.w, rb.r, rb.w })
			fds.push_back(f);
		auto relay = [](int src, int dst, std::string path) {
			FILE *f = fopen(path.c_str(), "wbe");
			char b[4096];
			ssize_t n;
			while ((n = read(src, b, sizeof(b))) > 0) {
				if (f) {
					fwrite(b, 1, n, f);
					fflush(f);
				}
				write_all(dst, std::string(b, n));
			}
			if (f)
				fclose(f);
		};
		relays.emplace_back(relay, ab.r, rb.w, record + "/a2b.raw");
		relays.emplace_back(relay, ba.r, ra.w, record + "/b2a.raw");
		a_in = ra.r;
		b_in = rb.r;
	}
	Modem m[2];
	Fax fax;
	FaxDte &dte = fax.dte;
	bool page_ok = false;
	/* the DTE (we) has to keep up with the T.30 timers */
	std::vector<std::string> rt = { "MODEM_REALTIME=1" };
	try {
		if (!m[0].start(exe_a, "modemA", a_in, ab.w, "3", rt,
				log_dir + "/modemA.log") ||
		    !m[1].start(exe_b, "modemB", b_in, ba.w, "3", rt,
				log_dir + "/modemB.log"))
			fail("a modem did not start");
		for (int k = 0; k < 2; k++)
			dte.fd[k] = open_pty(m[k].pty);

		bool v17 = mod == 73 || mod == 74 || mod == 97 || mod == 98 ||
			mod == 121 || mod == 122 || mod == 145 || mod == 146;
		int long_mod = (mod == 74 || mod == 98 || mod == 122 ||
				mod == 146) ? mod - 1 : mod;
		int page_mod = v17 ? long_mod + 1 : long_mod;
		static const std::map<int, int> bps_of = { { 24, 2400 },
			{ 48, 4800 }, { 72, 7200 }, { 96, 9600 }, { 73, 7200 },
			{ 97, 9600 }, { 121, 12000 }, { 145, 14400 } };
		if (!bps_of.count(long_mod))
			fail("no such modulation: " + std::to_string(mod));
		int bps = bps_of.at(long_mod);

		for (int k = 0; k < 2; k++) {
			std::vector<std::string> cs = { "ATZ", "AT+FCLASS=1" };
			for (auto &c : commands(setup))
				cs.push_back(c);
			for (auto &c : cs)
				if (fax.cmd(k, c) != "OK")
					fail("setup");
		}
		double t0 = now();
		write_all(dte.fd[1], "ATA\r");
		write_all(dte.fd[0], "ATD\r");
		std::string r = dte.wait_code(1, fax_result(), 30);
		say("B: ATA -> %s\n", r.empty() ? "None" : r.c_str());
		if (r != "CONNECT")
			fail("answer");
		std::string num = "   4321 555 1+";
		std::reverse(num.begin(), num.end());
		num.resize(20, ' ');
		std::string csi = std::string("\xff\x03\x40", 3) + num;
		std::string dis("\xff\x13\x80\x00\xce\xf4\x80\x80\x81\x80\x80"
				"\x80\x18", 13);
		std::string dcs("\xff\x13\x83\x00\xc6\x74", 6);
		fax.send_frames(1, { csi, dis });
		r = dte.wait_code(0, fax_result(), 30);
		say("A: ATD -> %s\n", r.empty() ? "None" : r.c_str());
		if (r != "CONNECT")
			fail("dial");
		if (fax.recv_frames(0) != std::vector<std::string>{ csi, dis })
			fail("A did not get CSI, DIS");

		/* DCS + TCF */
		fax.exchange(0, 1, { dcs });
		write_all(dte.fd[1], "AT+FRM=" + std::to_string(long_mod) + "\r");
		if (fax.cmd(0, "AT+FTS=8") != "OK")
			fail("FTS");
		r = fax.cmd(0, "AT+FTM=" + std::to_string(long_mod),
			    &fax_result(), 10);
		if (r != "CONNECT")
			fail("A FTM: " + r);
		std::string tcf(bps * 3 / 2 / 8, '\0');
		write_all(dte.fd[0], shield(tcf));
		r = dte.wait_code(1, fax_result(), 10);
		say("B: AT+FRM=%d -> %s\n", long_mod,
		    r.empty() ? "None" : r.c_str());
		if (r != "CONNECT")
			fail("B FRM");
		std::string data = fax.recv_until_etx(1, 60, &r, nullptr);
		std::string ra = dte.wait_code(0, fax_result(), 10);
		/* like a T.30 receiver: the longest run of zeros */
		size_t run = 0, cur = 0;
		for (char c : data) {
			cur = c == 0 ? cur + 1 : 0;
			run = std::max(run, cur);
		}
		say("A: TCF sent -> %s, B: TCF %zu/%zu bytes, %zu zeros in a "
		    "row -> %s\n", ra.empty() ? "None" : ra.c_str(),
		    data.size(), tcf.size(), run,
		    r.empty() ? "None" : r.c_str());
		if (!env("SHOW_TCF").empty()) {
			say("non-zero TCF bytes:");
			int shown = 0;
			for (size_t k = 0; k < data.size() && shown < 60; k++)
				if (data[k]) {
					say(" (%zu, 0x%x)", k,
					    (unsigned char)data[k]);
					shown++;
				}
			say("\n");
		}
		if (run < tcf.size() * 2 / 3)
			fail("TCF");

		/* CFR */
		fax.exchange(1, 0, { std::string("\xff\x13\x84", 3) });

		/* the page */
		std::string page;
		unsigned long long x = mod;
		for (int i = 0; i < bps * 4 / 8; i++) {
			x = x * 6364136223846793005ull + 1442695040888963407ull;
			page += (char)(x >> 56);
		}
		write_all(dte.fd[1], "AT+FRM=" + std::to_string(page_mod) + "\r");
		sleep_s(0.05);
		if (fax.cmd(0, "AT+FTS=8") != "OK")
			fail("FTS");
		r = fax.cmd(0, "AT+FTM=" + std::to_string(page_mod),
			    &fax_result(), 10);
		if (r != "CONNECT")
			fail("A FTM page: " + r);
		write_all(dte.fd[0], shield(page));
		r = dte.wait_code(1, fax_result(), 10);
		say("B: AT+FRM=%d -> %s\n", page_mod,
		    r.empty() ? "None" : r.c_str());
		if (r != "CONNECT")
			fail("B FRM page");
		data = fax.recv_until_etx(1, 60, &r, nullptr);
		ra = dte.wait_code(0, fax_result(), 20);
		if (!record.empty()) {
			write_file(record + "/page.rx", data);
			write_file(record + "/page.tx", page);
		}
		/* the data is a bit stream: the page at any bit offset */
		std::vector<int> rb = bits(data), pb = bits(page);
		long i = -1;
		long lim = std::min<long>((long)rb.size() - 128, 4000);
		for (long k = 0; k < lim; k++)
			if (std::equal(pb.begin(), pb.begin() + 128,
				       rb.begin() + k)) {
				i = k;
				break;
			}
		long errs = -1;
		std::vector<long> pos;
		if (i >= 0) {
			errs = 0;
			for (size_t k = 0; k + i < rb.size() && k < pb.size(); k++)
				if (rb[k + i] != pb[k]) {
					errs++;
					pos.push_back(k);
				}
		}
		say("A: page sent -> %s, B: page %zu/%zu bytes at bit %ld, %ld "
		    "bits wrong -> %s\n", ra.empty() ? "None" : ra.c_str(),
		    data.size(), page.size(), i, errs,
		    r.empty() ? "None" : r.c_str());
		if (errs > 0) {
			say("wrong bits at");
			for (size_t k = 0; k < pos.size() && k < 10; k++)
				say(" %ld", pos[k]);
			say(" ... %ld of %zu\n", pos.back(), pb.size());
		}
		/* the end of the page may be cut short by the carrier going
		   off (a real page ends with RTC), and a page without ECM
		   survives a few bit errors (the original library makes some
		   even with itself) */
		const size_t tail = 64;
		if (i >= 0 && rb.size() - i + tail >= pb.size()) {
			size_t wrong = 0;
			for (size_t k = 0; k + tail < pb.size() &&
			     k + i < rb.size(); k++)
				wrong += rb[k + i] != pb[k];
			page_ok = wrong <= pb.size() / 1000;
		}

		/* EOP, MCF, DCN */
		fax.exchange(0, 1, { std::string("\xff\x13\x2f", 3) });
		fax.exchange(1, 0, { std::string("\xff\x13\x8c", 3) });
		if (fax.cmd(0, "AT+FTH=3", &fax_result(), 10) != "CONNECT")
			fail("A FTH DCN");
		fax.send_frames(0, { std::string("\xff\x13\xfb", 3) });
		for (int k = 0; k < 2; k++)
			fax.cmd(k, "ATH");
		say("call took %.1f s\n", now() - t0);
		say("%s\n", page_ok ? "PASS" : "FAIL: page");
	} catch (const Fail &f) {
		say("FAIL: %s\n", f.msg.c_str());
		page_ok = false;
	}
	for (auto &x : m)
		x.stop(false);
	sleep_s(0.2);
	for (int &f : dte.fd)
		if (f >= 0) {
			close(f);
			f = -1;
		}
	close(ab.w);
	close(ba.w);
	for (auto &t : relays)
		t.join();
	for (size_t k = 2; k < fds.size(); k++)
		close(fds[k]);
	return page_ok;
}

} // namespace interop
