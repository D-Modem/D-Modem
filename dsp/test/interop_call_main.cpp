/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    interop_call_main.cpp - interop_call: a call between two modem
 *                            processes by hand, as the interop tests
 *                            make them (interop_call.h)
 *
 *    interop_call <caller exe> <answerer exe> ["ATX3;AT+MS=122"]
 *    interop_call --fax <sender exe> <receiver exe> [mod] [AT setup]
 *
 *    The environment says the rest (NET, STREAM, CALLS, WAV, LOG_DIR,
 *    ...: see interop_call.h).  Exits 0 if the call went through.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "interop_call.h"

/* a data call (interop::data_call) or with --fax a fax call
   (interop::fax_call, modulation 96 and ATX3 by default); exit status
   0 if it went through, 1 if not, 2 for the usage */
int main(int argc, char *argv[])
{
	if (argc > 1 && !strcmp(argv[1], "--fax")) {
		if (argc < 4) {
			fprintf(stderr, "usage: %s --fax <sender> <receiver> "
				"[mod] [AT setup]\n", argv[0]);
			return 2;
		}
		return interop::fax_call(argv[2], argv[3],
					 argc > 4 ? atoi(argv[4]) : 96,
					 argc > 5 ? argv[5] : "ATX3") ? 0 : 1;
	}
	if (argc < 3) {
		fprintf(stderr, "usage: %s <caller> <answerer> [AT commands]\n"
			"       %s --fax <sender> <receiver> [mod] [AT setup]\n",
			argv[0], argv[0]);
		return 2;
	}
	return interop::data_call(argv[1], argv[2], argc > 3 ? argv[3] : "")
		? 0 : 1;
}
