/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    modem_test.cpp - the modem simulator: two modems (modemA and modemB)
 *                     whose lines are connected back to back, each with
 *                     a pty for its DTE (printed at the start); dial on
 *                     one, answer on the other
 *
 *    The options are dmodemd's (-c, -d, -l).
 *
 */

#include <stdio.h>
#include <unistd.h>

#include <dsplibs.h>
#include <log.h>

#include "options.h"
#include "pipe_modem.h"

#ifndef MODEM_RATE
#define MODEM_RATE 9600
#endif

/* modemA and modemB back to back on two pipes until SIGINT or SIGTERM */
int main(int argc, char *argv[])
{
	dmd::Options opt;
	int pipe1[2], pipe2[2], ret;

	dcore::log_level = 1;
	dmd::parse_options(argc, argv, opt);
	dcore::log_open("test");
	dmd::print_version();
	prop_dp_init();
	if (pipe(pipe1) < 0 || pipe(pipe2) < 0) {
		perror("pipe");
		return 1;
	}
	dmd::PipeModem a("modemA", MODEM_RATE, opt.country, pipe1[0],
			 pipe2[1]);
	dmd::PipeModem b("modemB", MODEM_RATE, opt.country, pipe2[0],
			 pipe1[1]);
	fprintf(stderr, "created modemA: %s\ncreated modemB: %s\n",
		a.pty().name(), b.pty().name());
	dmd::PipeModem *modems[] = { &a, &b };
	ret = dmd::PipeModem::run(modems, 2);
	prop_dp_exit();
	dcore::log_close();
	return ret;
}
