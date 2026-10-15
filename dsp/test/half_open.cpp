/*
 *
 *    half_open.cpp - one modem (the open data pumps on the modem core)
 *                     whose line's audio is read from fd 3 and written to
 *                     fd 4, so that interop_call can put it back to back
 *                     with a modem in another process: the original
 *                     library (half_smartlink) or the Agere modem
 *
 *    Prints "created <name>: <pty>" for interop_call.  MODEM_NAME names it,
 *    MODEM_REALTIME=1 runs it no faster than the sample clock (with
 *    PIPE_SYNC it runs so on hook anyway, PipeModem::pace()),
 *    MODEM_SPEED=<x> else no faster than x times the sample clock, and
 *    MODEM_REC=<prefix> records its line in <prefix>.in and .out.
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include <dsplibs.h>
#include <log.h>

#include "options.h"
#include "pipe_modem.h"

#ifndef MODEM_RATE
#define MODEM_RATE 9600
#endif

/* the modem (slmodemd's options: country, ...) on fds 3 and 4 until its
   line ends; PipeModem::run's result */
int main(int argc, char *argv[])
{
	slm::Options opt;
	const char *name = getenv("MODEM_NAME") ? getenv("MODEM_NAME") :
		"modem";
	int ret;

	dcore::log_level = 1;
	slm::parse_options(argc, argv, opt);
	dcore::log_open("half");
	prop_dp_init();
	slm::PipeModem *pm = new slm::PipeModem(name, MODEM_RATE, opt.country,
						 3, 4);
	pm->set_realtime(getenv("MODEM_REALTIME") != NULL);
	pm->set_speed(getenv("MODEM_SPEED") ? atof(getenv("MODEM_SPEED")) :
		      0);
	pm->set_record(getenv("MODEM_REC"));
	printf("created %s: %s\n", name, pm->pty().name());
	fflush(stdout);
	ret = slm::PipeModem::run(&pm, 1);
	delete pm;
	prop_dp_exit();
	dcore::log_close();
	return ret;
}
