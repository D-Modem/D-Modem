/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    half_open.cpp - one modem (the open data pumps on the modem core)
 *                     whose line's audio is read from fd 3 and written to
 *                     fd 4, so that interop_call can put it back to back
 *                     with a modem in another process: another open one,
 *                     the original library or the other soft modems.
 *                     Built with the original library in the open pumps'
 *                     place (smartlink_glue.cpp) it is half_smartlink.
 *
 *    Prints "created <name>: <pty>" for interop_call.  MODEM_NAME names it,
 *    MODEM_REALTIME=1 runs it no faster than the sample clock (with
 *    PIPE_SYNC it runs so on hook anyway, PipeModem::pace()),
 *    MODEM_SPEED=<x> else no faster than x times the sample clock, and
 *    MODEM_REC=<prefix> records its line in <prefix>.<name>.in and .out
 *    (both modems of a call can be given the same prefix).
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include <string>

#include <dsplibs.h>
#include <log.h>

#include "options.h"
#include "pipe_modem.h"

#ifndef MODEM_RATE
#define MODEM_RATE 9600
#endif

/* the modem (dmodemd's options: country, ...) on fds 3 and 4 until its
   line ends; PipeModem::run's result */
int main(int argc, char *argv[])
{
	dmd::Options opt;
	const char *name = getenv("MODEM_NAME") ? getenv("MODEM_NAME") :
		"modem";
	int ret;

	dcore::log_level = 1;
	dmd::parse_options(argc, argv, opt);
	dcore::log_open("half");
	prop_dp_init();
	dmd::PipeModem *pm = new dmd::PipeModem(name, MODEM_RATE, opt.country,
						 3, 4);
	pm->set_realtime(getenv("MODEM_REALTIME") != NULL);
	pm->set_speed(getenv("MODEM_SPEED") ? atof(getenv("MODEM_SPEED")) :
		      0);
	/* MODEM_REC=<prefix>: the line to <prefix>.<name>.in and .out */
	static std::string rec;
	if (getenv("MODEM_REC")) {
		rec = std::string(getenv("MODEM_REC")) + "." + name;
		pm->set_record(rec.c_str());
	}
	printf("created %s: %s\n", name, pm->pty().name());
	fflush(stdout);
	ret = dmd::PipeModem::run(&pm, 1);
	delete pm;
	prop_dp_exit();
	dcore::log_close();
	return ret;
}
