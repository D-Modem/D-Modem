/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    courier_peer.cpp - D-Modem as the far end of courier-emu's closed
 *                       loop (its tools/probe_v90modem_closed_loop.py):
 *                       the original USRobotics Courier firmware, run by
 *                       courier-emu, calls, and this answers it on a
 *                       digital (G.711) line.
 *
 *    stdin: frames of a little-endian 16 bit count and that many mu-law
 *    octets from the line (A-law with COURIER_PEER_LAW=alaw); stdout:
 *    the same number of octets to the line, for each frame.  The log
 *    goes to stderr.
 *
 *    COURIER_PEER_AT: the AT commands before the answer (AT+MS=X2 by
 *    default); COURIER_PEER_DEBUG: the log level (default 5);
 *    COURIER_PEER_SEND: text to send once online (and again every
 *    COURIER_PEER_EVERY ms, numbered, when that is set); COURIER_PEER_DIAL:
 *    dial that number instead of answering; COURIER_PEER_ANALOG=1: the
 *    modem on an analogue line at 9600 Hz behind a codec (the line's
 *    G.711 to and from it resampled), as a customer's modem calling an
 *    I-modem.  What the DTE gets is in the log, as "dte: ...".
 *
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

#include "dmodem.h"
#include "g711.h"

/* a band-limited resampler (windowed sinc, 16 taps either side), from
   rate in to rate out, as a stream: the line's codec */
struct Resample {
	double in, out, t = 0, fc;
	std::vector<double> x;          /* the inputs not yet behind us */
	long base = 0;                  /* x[0]'s index */

	/* from rate i to rate o (the band to the lower one's half) */
	Resample(double i, double o) : in(i), out(o), fc(o < i ? o / i : 1) {}
	/* n inputs v in; the outputs they complete onto y */
	void run(const double *v, int n, std::vector<double> &y)
	{
		x.insert(x.end(), v, v + n);
		double step = in / out;
		while (t + 17 < base + (double)x.size()) {
			long c = (long)floor(t);
			double s = 0;
			for (long j = c - 15; j <= c + 16; j++) {
				if (j < base)
					continue;
				double d = t - j, a = M_PI * fc * d;
				double w = 0.5 + 0.5 * cos(M_PI * d / 17);
				s += x[j - base] * w * fc *
					(fabs(a) < 1e-9 ? 1 : sin(a) / a);
			}
			y.push_back(s);
			t += step;
		}
		long drop = (long)floor(t) - 20 - base;
		if (drop > 0) {
			x.erase(x.begin(), x.begin() + drop);
			base += drop;
		}
	}
};

/* n octets from stdin: false at its end */
static bool read_all(void *p, size_t n)
{
	unsigned char *b = (unsigned char *)p;
	while (n) {
		ssize_t r = read(0, b, n);
		if (r <= 0)
			return false;
		b += r;
		n -= (size_t)r;
	}
	return true;
}

/* n octets to stdout */
static bool write_all(const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	while (n) {
		ssize_t r = write(1, b, n);
		if (r <= 0)
			return false;
		b += r;
		n -= (size_t)r;
	}
	return true;
}

/* a log line to stderr */
static void log_line(const char *line)
{
	fputs(line, stderr);
	size_t n = strlen(line);
	if (!n || line[n - 1] != '\n')
		fputc('\n', stderr);
}

/* the AT commands, then a frame out for each frame in */
int main()
{
	const char *lv = getenv("COURIER_PEER_DEBUG");
	const char *at = getenv("COURIER_PEER_AT");
	const char *send = getenv("COURIER_PEER_SEND");
	dmodem_set_log(log_line);
	dmodem_set_debug(lv ? atoi(lv) : 5);
	if (dmodem_init())
		return 1;
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
		return 1;
	fcntl(sv[0], F_SETFL, O_NONBLOCK);
	fcntl(sv[1], F_SETFL, O_NONBLOCK);
	struct dmodem_config cfg = {};
	cfg.name = "courier-peer";
	cfg.dte_fd = sv[0];
	const char *law = getenv("COURIER_PEER_LAW");
	cfg.law = law && !strcmp(law, "alaw") ? DMODEM_LAW_ALAW :
		DMODEM_LAW_ULAW;
	std::string init = std::string(at ? at : "AT+MS=X2") + "\r";
	cfg.init = init.c_str();
	const char *an = getenv("COURIER_PEER_ANALOG");
	bool analog = an && atoi(an);
	struct dmodem *d = analog ? dmodem_create_rate(&cfg, 9600) :
		dmodem_create(&cfg);
	if (!d)
		return 1;
	bool alaw = cfg.law == DMODEM_LAW_ALAW;
	Resample up(8000, 9600), down(9600, 8000);
	std::vector<double> lin8, lin96, back;
	std::vector<int16_t> pin, pout;
	bool answered = false, sent = false;
	const char *every_s = getenv("COURIER_PEER_EVERY");
	long every = every_s ? atol(every_s) * 8 : 0, samples = 0, next = 0,
	     count = 0;
	std::string dte;
	std::vector<unsigned char> in, out;
	for (;;) {
		uint16_t n;
		if (!read_all(&n, 2))
			break;
		in.resize(n);
		out.assign(n, 0xff);
		if (n && !read_all(in.data(), n))
			break;
		/* (once the AT commands are in: 100 ms) */
		if (!answered && samples >= 800) {
			const char *dial = getenv("COURIER_PEER_DIAL");
			if (dial)
				dmodem_dial(d, dial);
			else
				dmodem_answer(d);
			answered = true;
		}
		dmodem_poll_dte(d);
		if (analog) {
			/* the line's octets to 9600 Hz, the modem's samples
			   back to 8000 Hz and the law (as many as came) */
			lin8.resize(n);
			for (int i = 0; i < n; i++)
				lin8[i] = dsp::g711::decode(alaw, in[i]);
			lin96.clear();
			up.run(lin8.data(), n, lin96);
			pin.resize(lin96.size());
			pout.assign(lin96.size(), 0);
			for (size_t i = 0; i < lin96.size(); i++)
				pin[i] = (int16_t)lrint(fmax(-32768, fmin(32767,
								    lin96[i])));
			if (!pin.empty())
				dmodem_process(d, pin.data(), pout.data(),
					       (int32_t)pin.size());
			std::vector<double> o(pout.begin(), pout.end());
			down.run(o.data(), (int)o.size(), back);
			for (int i = 0; i < n && !back.empty(); i++) {
				double v = back.front();
				back.erase(back.begin());
				out[i] = dsp::g711::encode(alaw, (int32_t)lrint(
					fmax(-32768, fmin(32767, v))));
			}
		} else
			dmodem_process_g711(d, in.data(), out.data(), n);
		char buf[512];
		ssize_t r;
		while ((r = read(sv[1], buf, sizeof buf)) > 0) {
			for (ssize_t i = 0; i < r; i++) {
				char c = buf[i];
				if (c == '\n' || c == '\r') {
					if (!dte.empty())
						fprintf(stderr, "dte: %s\n",
							dte.c_str());
					dte.clear();
				} else if ((unsigned char)c >= 32) {
					dte += c;
				}
			}
		}
		struct dmodem_status st;
		dmodem_status(d, &st);
		samples += n;
		if (st.link == DMODEM_ONLINE && send && !sent) {
			if (write(sv[1], send, strlen(send)) > 0)
				sent = true;
			next = samples + every;
		} else if (st.link == DMODEM_ONLINE && send && every &&
			   samples >= next) {
			std::string t = "\r\n[" + std::to_string(++count) +
				" at " + std::to_string(samples / 8000) +
				" s] " + send;
			if (write(sv[1], t.data(), t.size()) > 0)
				next = samples + every;
		}
		if (!write_all(out.data(), n))
			break;
	}
	if (!dte.empty())
		fprintf(stderr, "dte: %s\n", dte.c_str());
	dmodem_destroy(d);
	return 0;
}
