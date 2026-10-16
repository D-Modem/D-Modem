/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    fax_doc.cpp - the fax tests' document for the Asterisk and
 *    FreeSWITCH fax scripts
 *
 *    fax_doc write <file.tif>          the test document of two pages
 *    fax_doc light <file.tif>          the same with less on them
 *    fax_doc efax <file.tif>           the light one for efax to send:
 *                                      each page's data ends in RTC (it
 *                                      reads a page up to RTC)
 *    fax_doc compare <a.tif> <b.tif>   the same pages? (exit status)
 */

#include <stdio.h>
#include <string.h>

#include "fax_util.h"

/* fax_doc write|light <tif> or compare <a> <b>: PASS or FAIL and why;
   exit status 0, 1 (FAIL) or 2 (usage) */
int main(int argc, char **argv)
{
	testing::AssertionResult r = testing::AssertionFailure();

	if (argc == 3 && (!strcmp(argv[1], "write") || !strcmp(argv[1], "light") ||
			  !strcmp(argv[1], "efax")))
		r = write_test_doc(argv[2], strcmp(argv[1], "write") != 0,
				   !strcmp(argv[1], "efax"));
	else if (argc == 4 && !strcmp(argv[1], "compare"))
		r = same_pages(argv[2], argv[3]);
	else {
		fprintf(stderr, "usage: fax_doc write|light|efax <tif> | "
			"compare <a> <b>\n");
		return 2;
	}
	printf("%s: %s\n", r ? "PASS" : "FAIL", r.message());
	return r ? 0 : 1;
}
