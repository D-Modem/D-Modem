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
 *    octets from the line; stdout: the same number of octets to the
 *    line, for each frame.  The log goes to stderr.
 *
 *    COURIER_PEER_AT: the AT commands before the answer (AT+MS=X2 by
 *    default); COURIER_PEER_DEBUG: the log level (default 5);
 *    COURIER_PEER_SEND: text to send once online; COURIER_PEER_DIAL:
 *    dial that number instead of answering.  What the DTE gets
 *    is in the log, as "dte: ...".
 *
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

#include "dmodem.h"

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
	cfg.law = DMODEM_LAW_ULAW;
	std::string init = std::string(at ? at : "AT+MS=X2") + "\r";
	cfg.init = init.c_str();
	struct dmodem *d = dmodem_create(&cfg);
	if (!d)
		return 1;
	bool answered = false, sent = false;
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
		if (!answered) {
			const char *dial = getenv("COURIER_PEER_DIAL");
			if (dial)
				dmodem_dial(d, dial);
			else
				dmodem_answer(d);
			answered = true;
		}
		dmodem_poll_dte(d);
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
		if (st.link == DMODEM_ONLINE && send && !sent) {
			if (write(sv[1], send, strlen(send)) > 0)
				sent = true;
		}
		if (!write_all(out.data(), n))
			break;
	}
	if (!dte.empty())
		fprintf(stderr, "dte: %s\n", dte.c_str());
	dmodem_destroy(d);
	return 0;
}
