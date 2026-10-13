/*
 *
 *    core_script - two modems (A and B) back to back, driven by a script
 *                  as a DTE would drive them; prints what each one tells
 *                  its DTE, line by line with the time (ms of audio)
 *
 *    Built twice: with the new core and with slmodemd's old one.  The
 *    same script must give the same transcript (ctest core_equiv_*).
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
 *
 */

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
};

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
	sides[0].in = sides[1].out;
	sides[1].in = sides[0].out;
	samples += BLOCK;
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
