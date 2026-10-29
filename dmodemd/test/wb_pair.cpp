/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    wb_pair.cpp - a bare pair between two WB sticks for the tests: the
 *                  pair of fake_pico_wb.cpp's sticks, line_exchange's
 *                  counterpart for the wideband line
 *
 *    wb_pair <rate> <dir> <a> <b>
 *
 *    Each end's line is the socket <dir>/<end>.sock, which a fake stick
 *    connects to (fake_pico_wb.cpp: 'C' the pair's samples to it, 'P'
 *    its modem's samples).  Between them is the simulated pair of the
 *    V.wb tests (dsp/test/vwb_line.h: the cable's loss and delay, the
 *    transformers, noise, each side's own signal through its hybrid, b's
 *    clock apart from a's).  Every 5 ms, on the clock, each end gets the
 *    samples its codec has taken meanwhile (a's 5 ms, b's 5 ms of its own
 *    clock), and plays as many as the stick does: from what its modem
 *    has sent, after a head start of 3 ms; when that runs dry, silence
 *    until there is a head start again (an underrun); past 4096 samples
 *    queued the rest is dropped (an overrun).
 *
 *    PAIR_KM=<km> (1), PAIR_SNR=<dB> (60, the noise below a tone as
 *    sent), PAIR_PPM=<ppm> (b's clock fast by: 100), PAIR_ECHO=<dB>
 *    (-15, the hybrid's leak).
 *
 *    What happens is said on stdout, a line each ("ready", "1001 plays",
 *    "1001 underrun", "1001 gone", ...).  It runs until SIGTERM, SIGINT or
 *    its stdin's end.
 *
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <deque>
#include <string>
#include <vector>

#include "vwb_line.h"

/* (no MSG_NOSIGNAL: SIGPIPE is ignored here anyway) */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

volatile sig_atomic_t quit = 0;

/* SIGTERM, SIGINT: the main loop ends */
void on_signal(int)
{
	quit = 1;
}

/* the stick's FIFO of its modem's samples (firmware/PROTOCOL.md) */
const size_t FIFO_MAX = 4096;

/* one end of the pair: its socket, its stick's connections, what its
   modem has sent and not yet played, whether it plays (its head start
   reached) */
struct End {
	std::string name;
	int listen = -1, cap = -1, play = -1;
	std::deque<int16_t> played;
	int half = -1;
	bool playing = false, said_play = false;
	unsigned long underruns = 0, overruns = 0;
};

int rate = 96000;
End ends[2];

/* an end's socket listening */
int listen_at(const std::string &path)
{
	struct sockaddr_un a = {};
	/* (closed on exec: fcntl(), as Haiku has no SOCK_CLOEXEC) */
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (fd >= 0)
		fcntl(fd, F_SETFD, FD_CLOEXEC);
	unlink(path.c_str());
	a.sun_family = AF_UNIX;
	strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
	if (fd < 0 || bind(fd, reinterpret_cast<struct sockaddr *>(&a),
			   sizeof(a)) < 0 || listen(fd, 8) < 0) {
		perror(path.c_str());
		exit(1);
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* (as declared above) a line on stdout */
void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

/* the connections that came, each to its end by its first octet */
void accept_all()
{
	for (End &e : ends) {
		int fd;
		while ((fd = accept(e.listen, nullptr, nullptr)) >= 0) {
			fcntl(fd, F_SETFD, FD_CLOEXEC);
			char kind = 0;
			struct pollfd p = { fd, POLLIN, 0 };
			if (poll(&p, 1, 1000) != 1 || read(fd, &kind, 1) != 1 ||
			    (kind != 'C' && kind != 'P')) {
				close(fd);
				continue;
			}
			fcntl(fd, F_SETFL, O_NONBLOCK);
			int &slot = kind == 'C' ? e.cap : e.play;
			if (slot >= 0)
				close(slot);
			slot = fd;
			if (kind == 'C')
				say("%s connected", e.name.c_str());
		}
	}
}

/* what came from the sticks: their modems' samples (past the FIFO's
   room dropped); a closed connection forgotten */
void read_all()
{
	for (End &e : ends) {
		unsigned char b[8192];
		ssize_t n;
		if (e.play >= 0) {
			while ((n = read(e.play, b, sizeof(b))) > 0)
				for (ssize_t i = 0; i < n; i++) {
					if (e.half < 0) {
						e.half = b[i];
						continue;
					}
					int16_t s = (int16_t)(e.half | b[i] << 8);
					e.half = -1;
					if (e.played.size() < FIFO_MAX)
						e.played.push_back(s);
					else if (e.overruns++ == 0)
						say("%s overrun", e.name.c_str());
				}
			if (n == 0) {
				close(e.play);
				e.play = -1;
				e.played.clear();
				e.playing = e.said_play = false;
			}
		}
		if (e.cap >= 0) {
			/* (the capture sends nothing; its end is seen here) */
			n = recv(e.cap, b, sizeof(b), MSG_DONTWAIT);
			if (n == 0) {
				close(e.cap);
				e.cap = -1;
				say("%s gone", e.name.c_str());
			}
		}
	}
}

/* n samples e's stick plays: its modem's after the head start, silence
   when they run dry (until the head start again) */
void plays(End &e, size_t n, std::vector<s16> &out)
{
	size_t head = (size_t)rate * 3 / 1000;
	out.assign(n, 0);
	for (size_t i = 0; i < n; i++) {
		if (!e.playing && e.played.size() >= head) {
			e.playing = true;
			if (!e.said_play) {
				e.said_play = true;
				say("%s plays", e.name.c_str());
			}
		}
		if (!e.playing)
			continue;
		if (e.played.empty()) {
			e.playing = false;
			if (e.underruns++ == 0)
				say("%s underrun", e.name.c_str());
			continue;
		}
		out[i] = e.played.front();
		e.played.pop_front();
	}
}

/* samples to an end's stick, if it is there */
void send_to(End &e, const std::vector<s16> &in)
{
	if (e.cap >= 0 && !in.empty())
		(void)send(e.cap, in.data(), in.size() * 2,
			   MSG_DONTWAIT | MSG_NOSIGNAL);
}

/* 5 ms of the pair */
void tick(VwbLine &line)
{
	std::vector<s16> a_in, b_in, a_out, b_out;

	accept_all();
	read_all();
	line.inputs(rate / 200, a_in, b_in);
	send_to(ends[0], a_in);
	send_to(ends[1], b_in);
	plays(ends[0], a_in.size(), a_out);
	plays(ends[1], b_in.size(), b_out);
	line.outputs(a_out, b_out);
}

/* t in ns (CLOCK_MONOTONIC's) */
long long ns(const struct timespec &t)
{
	return t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* the next tick (next) after the one due at due: on the clock, tick_ns
   on; but late (a busy machine: a CI runner), half a tick apart until
   the clock is caught up, rather than a burst of ticks at once, which
   the modems would take as their line's samples all coming together (a
   stick's playback padded with silence, then too far ahead and cut);
   over a second late, the clock is now */
void pace(long long &due, struct timespec &next, long tick_ns)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	due += tick_ns;
	if (ns(now) - due > 1000000000LL)
		due = ns(now);
	long long at = due;
	if (at < ns(now) + tick_ns / 2)
		at = ns(now) + tick_ns / 2;
	next.tv_sec = (time_t)(at / 1000000000LL);
	next.tv_nsec = (long)(at % 1000000000LL);
}

/* asleep until at (CLOCK_MONOTONIC): clock_nanosleep(), or where there
   is none (macOS) nanosleep() for what is left of it */
void sleep_until(const struct timespec &at)
{
#ifdef TIMER_ABSTIME
	clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, nullptr);
#else
	struct timespec now, d;
	clock_gettime(CLOCK_MONOTONIC, &now);
	d.tv_sec = at.tv_sec - now.tv_sec;
	d.tv_nsec = at.tv_nsec - now.tv_nsec;
	if (d.tv_nsec < 0) {
		d.tv_nsec += 1000000000L;
		d.tv_sec--;
	}
	if (d.tv_sec >= 0)
		nanosleep(&d, nullptr);
#endif
}

/* the environment's number name, or dflt */
double env_or(const char *name, double dflt)
{
	const char *e = getenv(name);
	return e && *e ? atof(e) : dflt;
}

} // namespace

/* the pair: its ends' sockets, a tick each 5 ms until a signal or
   stdin's end (the usage: 2) */
int main(int argc, char *argv[])
{
	struct timespec next;

	if (argc != 5) {
		fprintf(stderr, "usage: %s <rate> <dir> <a> <b>\n", argv[0]);
		return 2;
	}
	rate = atoi(argv[1]);
	if (rate != 48000 && rate != 96000 && rate != 192000) {
		fprintf(stderr, "%s: the rate: 48000, 96000 or 192000\n", argv[0]);
		return 2;
	}
	VwbLineConfig c;
	c.fs = rate;
	c.km = env_or("PAIR_KM", 1);
	c.snr_db = env_or("PAIR_SNR", 60);
	c.ppm = env_or("PAIR_PPM", 100);
	c.echo_db = env_or("PAIR_ECHO", -15);
	VwbLine line(c);
	for (int i = 0; i < 2; i++) {
		ends[i].name = argv[3 + i];
		ends[i].listen = listen_at(std::string(argv[2]) + "/" +
					   argv[3 + i] + ".sock");
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	fcntl(0, F_SETFL, O_NONBLOCK);
	say("ready");
	clock_gettime(CLOCK_MONOTONIC, &next);
	long long due = ns(next);
	while (!quit) {
		char ch;
		if (read(0, &ch, 1) == 0)
			break;
		pace(due, next, 5000000L);
		sleep_until(next);
		tick(line);
	}
	for (End &e : ends)
		if (e.underruns || e.overruns)
			say("%s: %lu underruns, %lu overruns", e.name.c_str(),
			    e.underruns, e.overruns);
	return 0;
}
