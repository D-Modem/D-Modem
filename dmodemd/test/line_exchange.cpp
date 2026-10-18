/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    line_exchange.cpp - a telephone exchange for the ALSA line's tests:
 *                        the lines of fake_modem_card.cpp's cards, a
 *                        number each, calls between them
 *
 *    line_exchange <rate> <dir> <number>...
 *
 *    Each number's line is the socket <dir>/<number>.sock, which a card
 *    ("dmline") connects to (fake_modem_card.cpp).  Every 20 ms, on the
 *    clock, each line's capture gets 20 ms of what it hears:
 *
 *    - on hook, while called: the ring signal, 2 s of 20 Hz near full
 *      scale and 4 s of nothing, heard while the card has its line
 *      monitor on ("Caller ID Switch"), and after the first ring the
 *      caller's number and the name "D-MODEM" in caller ID (Bell 202,
 *      MDMF);
 *    - off hook: a dial tone (350 + 440 Hz) until the first digit,
 *      then nothing until the number's digits are all there (as many as
 *      a number has), in DTMF or in pulses: while dialling, the hook
 *      broken for 200 ms at most is a pulse, a digit ends when it has
 *      been made again for 300 ms (n pulses n, ten 0), and only on
 *      hook for longer than 300 ms is the line hung up; then the ringing tone (440 + 480 Hz, 2 s on, 4 s
 *      off) while the number is called, a busy tone (480 + 620 Hz, 0.5 s
 *      on and off) if it is off hook or unknown; once it answers (goes
 *      off hook), the other end at -3 dB with some noise and an echo of
 *      its own at -24 dB; once the other end has hung up, a second of
 *      nothing, then the busy tone.
 *
 *    What happens is said on stdout, a line each ("1001 off hook",
 *    "1001 dialled 1002", "1002 rings 2", "1001 1002 talk", ...).  It
 *    runs until SIGTERM, SIGINT or its stdin's end.
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

#include "dsp.h"

namespace {

volatile sig_atomic_t quit = 0;

/* SIGTERM, SIGINT: the main loop ends */
void on_signal(int)
{
	quit = 1;
}

enum class Mode { Idle, DialTone, Dialling, RingBack, Busy, Talk, Gone };

struct Line {
	std::string number;
	int listen = -1, cap = -1, play = -1, ctl = -1;
	bool off_hook = false, monitor = false;
	std::deque<int16_t> played;     /* from the modem, not yet heard */
	Mode mode = Mode::Idle;
	long mode_t = 0;                /* samples in the mode */
	Line *peer = nullptr;           /* Talk, RingBack: the other end */
	std::string digits;
	dsp::DtmfDetect dtmf;
	/* pulse dialling: when the hook broke (ms; -1: not), the pulses of
	   the digit so far, when it was made after the last one */
	long broken_at = -1;
	int pulses = 0;
	long made_at = 0;
	/* called: by whom, the samples since, the rings so far, caller ID */
	Line *caller = nullptr;
	long ring_t = 0;
	int rings = 0;
	std::vector<int16_t> cid;
};

int rate = 9600, frame = 192;
/* a line's echo of its own (LINE_ECHO=<dB>, -24 by default; "off") */
float echo = 0.063f;
/* the loss between two lines (LINE_LOSS=<dB>, 3 by default) */
float gain = 0.708f;
std::vector<Line> lines;
unsigned seed = 1;

/* a line's socket listening */
int listen_at(const std::string &path)
{
	struct sockaddr_un a = {};
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

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

/* l in mode m, from its start */
void set_mode(Line &l, Mode m)
{
	l.mode = m;
	l.mode_t = 0;
}

/* Bell 202 at rate: bits of 1200 Hz (1) and 2200 Hz (0), 1200 a second,
   at -13 dBm0 a tone */
struct Fsk {
	std::vector<int16_t> out;
	double phase = 0, clock = 0;
	/* bit b */
	void bit(int b)
	{
		clock += (double)rate / 1200;
		while (clock >= 1) {
			clock -= 1;
			phase += 2 * M_PI * (b ? 1200 : 2200) / rate;
			out.push_back((int16_t)(5000 * sin(phase)));
		}
	}
	/* octet c: a start bit, its 8 bits (LSB first), a stop
	   bit */
	void octet(unsigned char c)
	{
		bit(0);
		for (int i = 0; i < 8; i++)
			bit(c >> i & 1);
		bit(1);
	}
};

/* caller ID of number for a line called: the channel seizure (300 bits
   of 0101...), 180 marks, the MDMF message (date and time, the number,
   the name) and its checksum */
std::vector<int16_t> caller_id(const std::string &number)
{
	std::vector<unsigned char> m = { 0x80, 0 };
	auto param = [&](unsigned char type, const std::string &v) {
		m.push_back(type);
		m.push_back((unsigned char)v.size());
		m.insert(m.end(), v.begin(), v.end());
	};
	Fsk f;
	unsigned sum = 0;

	param(0x01, "10161530");
	param(0x02, number);
	param(0x07, "D-MODEM");
	m[1] = (unsigned char)(m.size() - 2);
	for (unsigned char c : m)
		sum += c;
	m.push_back((unsigned char)(0x100 - (sum & 0xff)));
	for (int i = 0; i < 300; i++)
		f.bit(i & 1);
	for (int i = 0; i < 180; i++)
		f.bit(1);
	for (unsigned char c : m)
		f.octet(c);
	for (int i = 0; i < 10; i++)
		f.bit(1);
	return f.out;
}

/* t (samples) of tones f1 and f2 (0: none), on and off ms at a time */
float tones(long t, double f1, double f2, int on, int off)
{
	long ms = t * 1000 / rate;
	if (off && ms % (on + off) >= on)
		return 0;
	double s = (double)t / rate;
	return 5000 * (float)(sin(2 * M_PI * f1 * s) +
			      (f2 ? sin(2 * M_PI * f2 * s) : 0));
}

/* a sample of noise at rms (uniform, pseudo-random) */
float noise(float rms)
{
	seed = seed * 1103515245 + 12345;
	return rms * 1.7f * ((float)((seed >> 16) & 0x7fff) / 16384 - 1);
}

/* the line of number, nullptr if none */
Line *find(const std::string &number)
{
	for (Line &l : lines)
		if (l.number == number)
			return &l;
	return nullptr;
}

void route(Line &l);

/* the exchange's time, ms (20 a tick) */
long now_ms = 0;

/* a digit dialled (DTMF or pulses): the dial tone off; the number
   routed once it has all its digits */
void add_digit(Line &l, char d)
{
	l.digits += d;
	if (l.mode == Mode::DialTone)
		set_mode(l, Mode::Dialling);
	if (l.digits.size() == l.number.size())
		route(l);
}

/* the line on hook: its call over */
void hang_up(Line &l)
{
	say("%s on hook", l.number.c_str());
	if (l.mode == Mode::Talk && l.peer) {
		set_mode(*l.peer, Mode::Gone);
		l.peer->peer = nullptr;
	}
	if (l.mode == Mode::RingBack && l.peer)
		l.peer->caller = nullptr;
	l.peer = nullptr;
	l.pulses = 0;
	set_mode(l, Mode::Idle);
}

/* the line's hook (the card's switch 0) or line monitor (1) changed */
void control(Line &l, int sw, bool on)
{
	if (sw == 1) {
		l.monitor = on;
		return;
	}
	if (on == l.off_hook)
		return;
	l.off_hook = on;
	/* while dialling, a break is a pulse, or the hang-up (tick()
	   tells once it is long enough) */
	bool dialling = l.mode == Mode::DialTone || l.mode == Mode::Dialling;
	if (!on && dialling) {
		l.broken_at = now_ms;
		return;
	}
	if (on && l.broken_at >= 0) {
		if (now_ms - l.broken_at <= 200) {
			l.pulses++;
			l.made_at = now_ms;
		}
		l.broken_at = -1;
		return;
	}
	if (on) {
		if (l.caller) {
			Line *c = l.caller;
			say("%s answers", l.number.c_str());
			l.caller = nullptr;
			set_mode(l, Mode::Talk);
			l.peer = c;
			set_mode(*c, Mode::Talk);
			c->peer = &l;
			say("%s %s talk", c->number.c_str(), l.number.c_str());
			return;
		}
		say("%s off hook", l.number.c_str());
		l.digits.clear();
		l.dtmf.init(rate);
		set_mode(l, Mode::DialTone);
		return;
	}
	hang_up(l);
}

/* a line's pulse dialling as time goes on: a break long enough to be
   the hang-up, a digit's end */
void pulse_time(Line &l)
{
	if (l.broken_at >= 0 && now_ms - l.broken_at > 300) {
		l.broken_at = -1;
		hang_up(l);
		return;
	}
	if (l.pulses && l.off_hook && l.broken_at < 0 &&
	    now_ms - l.made_at > 300) {
		char d = (char)('0' + l.pulses % 10);
		say("%s pulse digit %c", l.number.c_str(), d);
		l.pulses = 0;
		if (l.mode == Mode::DialTone || l.mode == Mode::Dialling)
			add_digit(l, d);
	}
}

/* a whole number dialled: its line called, or busy */
void route(Line &l)
{
	Line *to = find(l.digits);

	say("%s dialled %s", l.number.c_str(), l.digits.c_str());
	if (!to || to == &l || to->off_hook || to->caller) {
		say("%s busy", l.digits.c_str());
		set_mode(l, Mode::Busy);
		return;
	}
	set_mode(l, Mode::RingBack);
	l.peer = to;
	to->caller = &l;
	to->ring_t = 0;
	to->rings = 0;
	to->cid = caller_id(l.number);
}

/* what the modem sent this frame (out) heard by the exchange: its DTMF
   while dialling */
void heard(Line &l, const int16_t *out)
{
	if (l.mode != Mode::DialTone && l.mode != Mode::Dialling)
		return;
	for (int i = 0; i < frame; i++)
		if (l.dtmf.sample(out[i]) == dsp::DtmfDetect::ON) {
			add_digit(l, l.dtmf.digit);
			if (l.mode != Mode::DialTone &&
			    l.mode != Mode::Dialling)
				return;
		}
}

/* this frame of what line l hears into in (out: what each line sent) */
void hears(Line &l, const std::vector<std::vector<int16_t>> &out,
	   int16_t *in)
{
	size_t me = (size_t)(&l - &lines[0]);

	for (int i = 0; i < frame; i++) {
		float x = noise(4);
		long t = l.mode_t + i;
		if (!l.off_hook) {
			if (l.caller && l.monitor) {
				long r = l.ring_t + i, ms = r * 1000 / rate;
				if (ms % 6000 < 2000)
					x = 24000 * (float)sin(2 * M_PI * 20 *
							       r / rate);
				long c = r - (long)rate * 2500 / 1000;
				if (ms < 6000 && c >= 0 &&
				    c < (long)l.cid.size())
					x += l.cid[c];
			}
		} else {
			switch (l.mode) {
			case Mode::DialTone:
				x += tones(t, 350, 440, 1, 0);
				break;
			case Mode::RingBack:
				x += tones(l.peer ? l.peer->ring_t + i : t,
					   440, 480, 2000, 4000);
				break;
			case Mode::Busy:
				x += tones(t, 480, 620, 500, 500);
				break;
			case Mode::Gone:
				if (t >= rate)
					x += tones(t, 480, 620, 500, 500);
				break;
			case Mode::Talk: {
				size_t p = (size_t)(l.peer - &lines[0]);
				x += gain * out[p][i] + echo * out[me][i] +
					noise(20);
				break;
			}
			default:
				break;
			}
		}
		in[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
	}
}

/* the connections that came, each to its line by its first octet */
void accept_all()
{
	for (Line &l : lines) {
		int fd;
		while ((fd = accept4(l.listen, nullptr, nullptr,
				     SOCK_CLOEXEC)) >= 0) {
			char kind = 0;
			struct pollfd p = { fd, POLLIN, 0 };
			if (poll(&p, 1, 1000) != 1 || read(fd, &kind, 1) != 1) {
				close(fd);
				continue;
			}
			fcntl(fd, F_SETFL, O_NONBLOCK);
			int &slot = kind == 'C' ? l.cap : kind == 'P' ? l.play :
				l.ctl;
			if (slot >= 0)
				close(slot);
			slot = fd;
		}
	}
}

/* what came from the cards: their switches, their samples; a closed
   connection forgotten (a closed control: on hook) */
void read_all()
{
	for (Line &l : lines) {
		unsigned char b[4096];
		ssize_t n;
		if (l.ctl >= 0) {
			while ((n = read(l.ctl, b, 2)) == 2)
				control(l, b[0], b[1]);
			if (n == 0) {
				close(l.ctl);
				l.ctl = -1;
				control(l, 0, false);
			}
		}
		if (l.play >= 0) {
			while ((n = read(l.play, b, sizeof(b) & ~1u)) > 0)
				for (ssize_t i = 0; i + 1 < n; i += 2)
					l.played.push_back((int16_t)(b[i] |
								     b[i + 1] << 8));
			if (n == 0) {
				close(l.play);
				l.play = -1;
			}
		}
		if (l.cap >= 0) {
			/* (the capture sends nothing; its end is seen
			   here) */
			n = recv(l.cap, b, sizeof(b), MSG_DONTWAIT);
			if (n == 0) {
				close(l.cap);
				l.cap = -1;
			}
		}
	}
}

/* 20 ms of the exchange */
void tick()
{
	std::vector<std::vector<int16_t>> out(lines.size(),
					      std::vector<int16_t>(frame, 0));
	std::vector<int16_t> in(frame);

	now_ms += 20;
	accept_all();
	read_all();
	for (Line &l : lines)
		pulse_time(l);
	for (size_t k = 0; k < lines.size(); k++) {
		Line &l = lines[k];
		for (int i = 0; i < frame && !l.played.empty(); i++) {
			out[k][i] = l.played.front();
			l.played.pop_front();
		}
		/* (a backlog of more than 0.5 s is no line's) */
		while ((int)l.played.size() > rate / 2)
			l.played.pop_front();
		heard(l, out[k].data());
	}
	for (Line &l : lines) {
		hears(l, out, in.data());
		if (l.cap >= 0)
			(void)send(l.cap, in.data(), in.size() * 2,
				   MSG_DONTWAIT | MSG_NOSIGNAL);
		l.mode_t += frame;
		if (l.caller) {
			long before = l.ring_t * 1000 / rate;
			l.ring_t += frame;
			if (before % 6000 > l.ring_t * 1000 / rate % 6000 ||
			    before == 0) {
				l.rings++;
				say("%s rings %d", l.number.c_str(), l.rings);
			}
			if (l.rings > 10) {
				say("%s no answer", l.number.c_str());
				l.caller->peer = nullptr;
				set_mode(*l.caller, Mode::Busy);
				l.caller = nullptr;
			}
		}
	}
}

} // namespace

/* the exchange: its lines' sockets, a tick each 20 ms
   until a signal or stdin's end (the usage: 2) */
int main(int argc, char *argv[])
{
	struct timespec next;

	if (argc < 4) {
		fprintf(stderr, "usage: %s <rate> <dir> <number>...\n", argv[0]);
		return 2;
	}
	rate = atoi(argv[1]);
	if (const char *e = getenv("LINE_ECHO"))
		echo = strcmp(e, "off") ? powf(10, (float)atof(e) / 20) : 0;
	if (const char *e = getenv("LINE_LOSS"))
		gain = powf(10, -(float)atof(e) / 20);
	frame = rate / 50;
	lines.resize((size_t)(argc - 3));
	for (int i = 3; i < argc; i++) {
		Line &l = lines[(size_t)(i - 3)];
		l.number = argv[i];
		l.listen = listen_at(std::string(argv[2]) + "/" + argv[i] +
				     ".sock");
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	fcntl(0, F_SETFL, O_NONBLOCK);
	say("ready");
	clock_gettime(CLOCK_MONOTONIC, &next);
	while (!quit) {
		char c;
		if (read(0, &c, 1) == 0)
			break;
		next.tv_nsec += 20000000L;
		if (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
		tick();
	}
	return 0;
}
