/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    core_script - two modems (A and B) back to back, driven by a script
 *                  as a DTE would drive them; prints what each one tells
 *                  its DTE, line by line with the time (ms of audio)
 *
 *    A script's transcript must be the one in its .expect (ctest
 *    core_equiv: slmodemd's old core's, or the core's own where it
 *    differs on purpose).
 *
 *    core_script <script>
 *
 *    The script, one command a line (# comments):
 *      A <text>            the text from A's DTE (\r \n \\ \xNN)
 *      run <ms>            the audio runs
 *      until A <text> <ms> run until A has told its DTE text (since the
 *                          last until for A), at most ms
 *      send A <bytes>      A's DTE sends lines of text, as fast as the
 *                          modem takes them
 *      ring A              a ring signal on A's line (1 s of bursts)
 *      signal A, cancel A  a ringing call (SIP) comes, goes
 *      hangup A            the program hangs A up
 *      state               the states of the calls and their rates
 *      tone A <f>[+<f>] [<on> <off>], tone A off
 *                          a tone on A's line besides B (a dial tone,
 *                          ringing): its frequencies (Hz), steady or on
 *                          and off ms at a time; none
 *      echo A <dB> <ms> [<dB> <ms>], echo A off
 *                          A's signal back on its line (a hybrid's
 *                          echo), a block (20 ms) and ms later at dB;
 *                          a second one; none
 *
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include <string>
#include <vector>

#include "script.h"

static const int RATE = 8000, BLOCK = 160;      /* 20 ms */

/* one modem and its DTE */
struct Side {
	char name;
	Core *core;
	std::string line;       /* the DTE's line so far */
	std::string since;      /* for until */
	std::string tx;         /* the DTE's data still to send */
	std::vector<int16_t> in, out;
	/* a tone on its line: frequencies, on and off samples (off 0:
	   steady), the samples since it began */
	std::vector<double> tone;
	unsigned long tone_on = 0, tone_off = 0, tone_n = 0;
	/* its echoes (gain, delay in samples), what it sent (the last
	   ECHO_HIST samples) */
	std::vector<std::pair<double, int>> echo;
	std::vector<int16_t> sent;
};

static const int ECHO_HIST = 1 << 15;

static unsigned long samples, ms;
static Side sides[2];

/* the script's text with its escapes (backslash r, n, xNN, or the
   next character as it is) */
static std::string unescape(const char *s)
{
	std::string r;
	for (; *s; s++) {
		if (*s != '\\' || !s[1]) {
			r += *s;
			continue;
		}
		s++;
		switch (*s) {
		case 'r': r += '\r'; break;
		case 'n': r += '\n'; break;
		case 'x': {
			/* up to two hex digits, as many as there are */
			char h[3] = { 0, 0, 0 };
			for (int k = 0; k < 2 && isxdigit((unsigned char)s[1]);
			     k++)
				h[k] = *++s;
			r += (char)strtoul(h, nullptr, 16);
			break;
		}
		default: r += *s; break;
		}
	}
	return r;
}

/* a line the modem told its DTE, with the time, controls visible */
static void print_line(Side &s, const std::string &l)
{
	printf("%7lu %c: ", ms, s.name);
	for (unsigned char c : l) {
		if (c == '\r')
			printf("\\r");
		else if (c == '\n')
			printf("\\n");
		else if (c < 32 || c > 126)
			printf("\\x%02x", c);
		else
			putchar(c);
	}
	putchar('\n');
}

/* what the modem told its DTE: printed line by line, kept for until */
static void collect(Side &s)
{
	std::string o = s.core->dte_out();
	for (char c : o) {
		s.line += c;
		if (c == '\n') {
			print_line(s, s.line);
			s.line.clear();
		}
	}
	s.since += o;
}

/* both modems' */
static void collect_all()
{
	collect(sides[0]);
	collect(sides[1]);
}

/* the DTE's data to send, as much as the modem has room for */
static void feed(Side &s)
{
	if (s.tx.empty())
		return;
	int n = s.core->dte_space();
	if (n > (int)s.tx.size())
		n = s.tx.size();
	if (n <= 0)
		return;
	n = s.core->dte_in(s.tx.data(), n);
	s.tx.erase(0, n);
}

/* the side's tone (if any) on its line's samples */
static void add_tone(Side &s)
{
	if (s.tone.empty())
		return;
	for (int16_t &x : s.in) {
		unsigned long t = s.tone_n++;
		if (s.tone_off && t % (s.tone_on + s.tone_off) >= s.tone_on)
			continue;
		double y = x;
		for (double f : s.tone)
			y += 3000 * sin(2 * M_PI * f * t / RATE);
		x = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
	}
}

/* the side's echoes on its line's samples: what it sent a block and
   their delays before (samples) */
static void add_echo(Side &s)
{
	if (s.sent.empty())
		s.sent.assign(ECHO_HIST, 0);
	for (int i = 0; i < BLOCK; i++) {
		/* this block's sample i, in the side's count */
		unsigned long t = samples + i;
		double y = s.in[i];
		for (const auto &e : s.echo)
			if (t >= (unsigned long)(BLOCK + e.second))
				y += e.first * s.sent[(t - BLOCK - e.second) %
						      ECHO_HIST];
		s.in[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
	}
}

/* one block of audio: each side's output reaches the other a block later */
static void block()
{
	for (Side &s : sides) {
		feed(s);
		if (s.core->started())
			s.core->process(s.in.data(), s.out.data(), BLOCK);
		else
			std::fill(s.out.begin(), s.out.end(), 0);
	}
	/* what each sent (the samples of this block), for its echoes */
	for (Side &s : sides) {
		if (s.sent.empty())
			s.sent.assign(ECHO_HIST, 0);
		for (int i = 0; i < BLOCK; i++)
			s.sent[(samples + i) % ECHO_HIST] = s.out[i];
	}
	sides[0].in = sides[1].out;
	sides[1].in = sides[0].out;
	samples += BLOCK;
	for (Side &s : sides) {
		add_tone(s);
		if (!s.echo.empty())
			add_echo(s);
	}
	ms = samples * 1000 / RATE;
	for (Side &s : sides)
		s.core->poll();
	collect_all();
}

/* t ms of audio */
static void run(unsigned long t)
{
	unsigned long end = ms + t;
	while (ms < end)
		block();
}

/* A or B */
static Side &side(const char *n)
{
	return sides[*n == 'B' ? 1 : 0];
}

/* n octets of numbered lines of text from who */
static std::string text_data(char who, long n)
{
	std::string d;
	for (int line = 0; (long)d.size() < n; line++) {
		char l[80];
		snprintf(l, sizeof(l), "%c%04d The quick brown fox jumps over "
			 "the lazy dog 0123456789\r\n", who, line);
		d += l;
	}
	d.resize(n);
	return d;
}

/* runs the script argv[1]; 2 on a usage or script error */
int main(int argc, char **argv)
{
	FILE *f;
	char buf[1024];

	if (argc < 2 || !(f = fopen(argv[1], "r"))) {
		fprintf(stderr, "usage: core_script <script>\n");
		return 2;
	}
	core_init();
	for (int i = 0; i < 2; i++) {
		static const char *names[] = { "A", "B" };
		sides[i].name = names[i][0];
		sides[i].core = core_create(names[i], &ms);
		sides[i].in.assign(BLOCK, 0);
		sides[i].out.assign(BLOCK, 0);
	}
	while (fgets(buf, sizeof(buf), f)) {
		char cmd[32] = "", a[32] = "";
		buf[strcspn(buf, "\n")] = 0;
		if (!buf[0] || buf[0] == '#')
			continue;
		sscanf(buf, "%31s %31s", cmd, a);
		if (!strcmp(cmd, "A") || !strcmp(cmd, "B")) {
			std::string t = unescape(buf + 2);
			printf("%7lu %s< %s\n", ms, cmd, buf + 2);
			side(cmd).core->dte_in(t.data(), t.size());
			collect_all();
		} else if (!strcmp(cmd, "run")) {
			run(atol(a));
		} else if (!strcmp(cmd, "until")) {
			Side &s = side(a);
			char *p = buf + 6 + strlen(a) + 1, *e = strrchr(p, ' ');
			unsigned long end;
			std::string t;
			if (!e)
				continue;
			*e = 0;
			t = unescape(p);
			end = ms + atol(e + 1);
			while (s.since.find(t) == std::string::npos && ms < end)
				block();
			printf("%7lu until %c '%s': %s\n", ms, s.name, p,
			       s.since.find(t) != std::string::npos ? "yes" :
			       "TIMEOUT");
			s.since.clear();
		} else if (!strcmp(cmd, "send")) {
			Side &s = side(a);
			s.tx += text_data(s.name, atol(buf + 5 + strlen(a) + 1));
		} else if (!strcmp(cmd, "ring")) {
			Side &s = side(a);
			/* 1 s of ring signal: a burst every 50 ms */
			for (int i = 0; i < 20; i++) {
				s.core->ring();
				run(50);
			}
		} else if (!strcmp(cmd, "signal") || !strcmp(cmd, "cancel")) {
			side(a).core->ring_signal(!strcmp(cmd, "signal"));
			collect_all();
		} else if (!strcmp(cmd, "tone")) {
			Side &s = side(a);
			char f[64] = "";
			unsigned long on = 0, off = 0;
			sscanf(buf, "%*s %*s %63s %lu %lu", f, &on, &off);
			s.tone.clear();
			s.tone_on = on * RATE / 1000;
			s.tone_off = off * RATE / 1000;
			s.tone_n = 0;
			for (char *t = strtok(f, "+"); t && strcmp(t, "off");
			     t = strtok(nullptr, "+"))
				s.tone.push_back(atof(t));
		} else if (!strcmp(cmd, "echo")) {
			Side &s = side(a);
			double db[2], ms[2];
			int n = sscanf(buf, "%*s %*s %lf %lf %lf %lf", &db[0],
				       &ms[0], &db[1], &ms[1]);
			s.echo.clear();
			for (int k = 0; k + 1 < n && k < 4; k += 2)
				s.echo.push_back({ pow(10, db[k / 2] / 20),
						   (int)(ms[k / 2] * RATE /
							 1000) });
		} else if (!strcmp(cmd, "hangup")) {
			side(a).core->hangup();
			collect_all();
		} else if (!strcmp(cmd, "state")) {
			for (Side &s : sides) {
				unsigned r = s.core->rates();
				printf("%7lu %c: state %d, tx %u rx %u, %s\n", ms,
				       s.name, s.core->link_state(),
				       (r >> 16) * 100, (r & 0xffff) * 100,
				       s.core->started() ? "started" : "idle");
			}
		} else {
			fprintf(stderr, "unknown command: %s\n", buf);
			return 2;
		}
	}
	for (Side &s : sides)
		if (!s.line.empty())
			print_line(s, s.line);
	for (Side &s : sides)
		delete s.core;
	return 0;
}
