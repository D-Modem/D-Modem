/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    capid.cpp - dmodem-capid: a CAPI 2.0 controller on a D-Modem, for
 *                libcapi20's applications (capi4hylafax, chan_capi,
 *                pppd's capiplugin, isdn4linux-era BBS software) through
 *                its remote CAPI module
 *
 *    dmodem-capid [-l [addr:]port] [-i commands] <modem>
 *
 *      <modem>   the modem's DTE: dmodemd's pty (/dev/ttyDM0), a serial
 *                port, or tcp:host:port
 *      -l        where the applications connect (6000, every address)
 *      -i        AT commands after the controller's own setup
 *
 *    /etc/capi20.conf on the applications' machine:
 *      REMOTE rcapi <host> 6000
 *
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

#include "rcapi_server.h"

namespace {

/* how to run it, to stderr */
void usage()
{
	fprintf(stderr,
		"usage: dmodem-capid [-l [addr:]port] [-i commands] "
		"<modem pty | tcp:host:port>\n");
}

} // namespace

/* the server until the modem goes away */
int main(int argc, char **argv)
{
	std::string addr, init;
	int port = 6000, c;
	while ((c = getopt(argc, argv, "l:i:h")) != -1) {
		switch (c) {
		case 'l': {
			std::string l = optarg;
			size_t colon = l.rfind(':');
			if (colon != std::string::npos) {
				addr = l.substr(0, colon);
				l = l.substr(colon + 1);
			}
			port = atoi(l.c_str());
			break;
		}
		case 'i':
			init = optarg;
			break;
		default:
			usage();
			return c == 'h' ? 0 : 2;
		}
	}
	if (optind != argc - 1) {
		usage();
		return 2;
	}
	int fd = capi::open_modem(argv[optind]);
	if (fd < 0) {
		fprintf(stderr, "dmodem-capid: cannot open %s: %s\n",
			argv[optind], strerror(errno));
		return 1;
	}
	capi::Server server(fd, init);
	int p = server.listen(addr.empty() ? nullptr : addr.c_str(), port);
	if (p < 0) {
		fprintf(stderr, "dmodem-capid: cannot listen on port %d\n",
			port);
		return 1;
	}
	fprintf(stderr, "dmodem-capid: %s, port %d\n", argv[optind], p);
	while (server.step(-1))
		;
	fprintf(stderr, "dmodem-capid: the modem is gone\n");
	return 1;
}
