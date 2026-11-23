/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    embed_cat.c - a test program for dmodem_embed.h, in C: the modem in
 *                  a thread on an anonymous SIP line, its DTE on stdin
 *                  and stdout.  The log of the modem goes to stderr.
 *
 *    usage: embed_cat [-p sip_port] [-L] [-d log_level] [-s]
 *
 *    At the end of stdin, the program closes its descriptor.  Then the
 *    modem stops, and the program writes "exit <status>" to stderr.
 *    With -s, the program calls dmodem_embed_stop() first.
 *    Exit status 0, or 1 when the modem could not start.
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dmodem_embed.h"

/* a line of the modem's log to stderr */
static void on_log(void *ctx, const char *line)
{
	(void)ctx;
	fprintf(stderr, "log: %s\n", line);
}

/* the modem's end to stderr */
static void on_exit_cb(void *ctx, int status)
{
	(void)ctx;
	fprintf(stderr, "exit %d\n", status);
}

/* the modem on stdin and stdout until stdin ends */
int main(int argc, char *argv[])
{
	dmodem_embed_config cfg;
	dmodem_embed *e;
	int fd, i, in_open = 1, stop = 0;
	char buf[4096];

	dmodem_embed_config_init(&cfg);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-p") && i + 1 < argc)
			cfg.sip_port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-L"))
			cfg.sip_listen = 1;
		else if (!strcmp(argv[i], "-s"))
			stop = 1;
		else if (!strcmp(argv[i], "-d") && i + 1 < argc)
			cfg.log_level = atoi(argv[++i]);
	}
	/* (no account: anonymous) */
	cfg.sip_log_level = 1;
	fprintf(stderr, "D-Modem %s, lines 0x%x\n", dmodem_embed_version(),
		dmodem_embed_lines());
	e = dmodem_embed_new();
	if (!e)
		return 1;
	dmodem_embed_on_log(e, on_log, NULL);
	dmodem_embed_on_exit(e, on_exit_cb, NULL);
	fd = dmodem_embed_start(e, &cfg);
	if (fd < 0) {
		fprintf(stderr, "start: %s\n", dmodem_embed_error(e));
		dmodem_embed_free(e);
		return 1;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	while (fd >= 0) {
		struct pollfd p[2] = {
			{ fd, POLLIN, 0 }, { 0, in_open ? POLLIN : 0, 0 }
		};
		if (poll(p, 2, 200) < 0)
			break;
		if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			ssize_t n = read(fd, buf, sizeof(buf));
			if (n == 0)
				break;
			if (n > 0)
				fwrite(buf, 1, (size_t)n, stdout);
		}
		if (in_open && (p[1].revents & (POLLIN | POLLHUP))) {
			ssize_t n = read(0, buf, sizeof(buf));
			if (n <= 0) {
				/* the end of stdin: the modem stopped
				   (-s), the DTE closed */
				in_open = 0;
				if (stop)
					dmodem_embed_stop(e);
				close(fd);
				fd = -1;
				break;
			}
			if (write(fd, buf, (size_t)n) < 0)
				break;
		}
		if (!dmodem_embed_running(e))
			break;
	}
	/* (the thread ends by itself when the DTE closed) */
	while (dmodem_embed_running(e))
		usleep(10000);
	dmodem_embed_free(e);
	if (fd >= 0)
		close(fd);
	return 0;
}
