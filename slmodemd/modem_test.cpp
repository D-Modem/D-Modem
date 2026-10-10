/*
 *
 *    modem_test.cpp - the modem simulator: two modems (modemA and modemB)
 *                     whose lines are connected back to back, each with
 *                     a pty for its DTE (printed at the start); dial on
 *                     one, answer on the other
 *
 *    The options are slmodemd's (-c, -d, -l).
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

extern int  dp_dummy_init(void);
extern void dp_dummy_exit(void);
extern int  dp_sinus_init(void);
extern void dp_sinus_exit(void);

int main(int argc, char *argv[])
{
	slm::Options opt;
	int pipe1[2], pipe2[2], ret;

	dcore::log_level = 1;
	slm::parse_options(argc, argv, opt);
	dcore::log_open("test");
	slm::print_version();
	dp_dummy_init();
	dp_sinus_init();
	prop_dp_init();
	if (pipe(pipe1) < 0 || pipe(pipe2) < 0) {
		perror("pipe");
		return 1;
	}
	slm::PipeModem a("modemA", MODEM_RATE, opt.country, pipe1[0],
			 pipe2[1]);
	slm::PipeModem b("modemB", MODEM_RATE, opt.country, pipe2[0],
			 pipe1[1]);
	fprintf(stderr, "created modemA: %s\ncreated modemB: %s\n",
		a.pty().name(), b.pty().name());
	slm::PipeModem *modems[] = { &a, &b };
	ret = slm::PipeModem::run(modems, 2);
	dp_dummy_exit();
	dp_sinus_exit();
	prop_dp_exit();
	dcore::log_close();
	return ret;
}
