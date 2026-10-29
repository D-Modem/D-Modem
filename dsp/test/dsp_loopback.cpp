/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dsp_loopback.cpp - the open data pumps back to back (loopback.h), from
 *    the command line; pump_test runs the cases of the test suite
 *
 *    usage: dsp_loopback <dp id> [snr_db] [seconds] [srate] [gain] [ppm]
 *                        [echo_db] [far_echo_db]
 *
 *    CALLER_DP=<id> gives the caller another data pump than the answerer
 *    (automode); FAR_DELAY=<samples> delays the far echo more, as over
 *    VoIP; FLAT=1: no band limitation; DUMP=<name>: what each side hears
 *    into <name>.a and <name>.c; CODEC=<spec>: a speech codec on the
 *    line each way (codec_sim.h, 8000 Hz: CODEC=amr:12.2);
 *    DSP_DEBUG=<level>: the pumps' log.
 *
 *    echo_db adds an echo of each modem's own signal to its input
 *    (hybrid echo, about 2 ms delay) at that level relative to the
 *    transmitted signal, e.g. -10; far_echo_db the echo from the far
 *    end's hybrid: each modem's own signal after a round trip over the
 *    line.
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include "loopback.h"

/* one loopback run as the arguments and the environment say: both
   sides' results, then PASS or FAIL (exit status 1: a side not ok or
   a pump failed) */
int main(int argc, char **argv)
{
	LoopbackConfig c;
	LoopbackResult r;
	bool fail;

	c.dp = argc > 1 ? atoi(argv[1]) : 122;     /* V.22bis */
	if (argc > 2)
		c.snr = atof(argv[2]);
	if (argc > 3)
		c.secs = atof(argv[3]);
	if (argc > 4)
		c.srate = atoi(argv[4]);
	if (argc > 5)
		c.gain = atof(argv[5]);
	if (argc > 6)
		c.ppm = atof(argv[6]);
	if (argc > 7)
		c.echo_db = atof(argv[7]);
	if (argc > 8)
		c.far_db = atof(argv[8]);
	if (getenv("CALLER_DP"))
		c.caller_dp = atoi(getenv("CALLER_DP"));
	if (getenv("FAR_DELAY"))
		c.far_delay = atoi(getenv("FAR_DELAY"));
	c.flat = getenv("FLAT") != nullptr;
	c.dump = getenv("DUMP");
	c.codec = getenv("CODEC");
	r = loopback_run(c);
	fail = r.dp_failed;
	for (int k = 0; k < 2; k++) {
		printf("%s\n", r.side[k].text(k ? "answerer" : "caller").c_str());
		fail |= !r.side[k].ok();
	}
	printf("%s\n", fail ? "FAIL" : "PASS");
	return fail;
}
