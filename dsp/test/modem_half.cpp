/*
 *
 *    modem_half.cpp - one modem (the open data pumps on the modem core)
 *                     whose line's audio is read from fd 3 and written to
 *                     fd 4, so that interop.py can put it back to back
 *                     with a modem in another process: the original
 *                     library (modem_half_legacy.cpp) or the Agere modem
 *
 *    Prints "created <name>: <pty>" for the script.  MODEM_NAME names it,
 *    MODEM_REALTIME=1 runs it no faster than the sample clock, and
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

extern int dp_dummy_init(void);
extern int dp_sinus_init(void);

int main(int argc, char *argv[])
{
	slm::Options opt;
	const char *name = getenv("MODEM_NAME") ? getenv("MODEM_NAME") :
		"modem";
	int ret;

	dcore::log_level = 1;
	slm::parse_options(argc, argv, opt);
	dcore::log_open("half");
	dp_dummy_init();
	dp_sinus_init();
	prop_dp_init();
	slm::PipeModem *pm = new slm::PipeModem(name, MODEM_RATE, opt.country,
						 3, 4);
	pm->set_realtime(getenv("MODEM_REALTIME") != NULL);
	pm->set_record(getenv("MODEM_REC"));
	printf("created %s: %s\n", name, pm->pty().name());
	fflush(stdout);
	ret = slm::PipeModem::run(&pm, 1);
	delete pm;
	prop_dp_exit();
	dcore::log_close();
	return ret;
}
