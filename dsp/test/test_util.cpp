/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    test_util.cpp - the DSP's log and Gaussian noise for the test
 *    programs (test_util.h)
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsplibs.h"
#include "test_util.h"

unsigned int dsplibs_debug_level = 0;

/* the DSP's log (DSP_DBG, from level 2): to stderr */
int dsplibs_debug_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	return 0;
}

/* see test_util.h */
double gauss()
{
	double u = (rand() + 1.0) / (RAND_MAX + 2.0);
	double v = (rand() + 1.0) / (RAND_MAX + 2.0);
	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}
