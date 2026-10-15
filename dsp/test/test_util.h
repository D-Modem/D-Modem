/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    test_util.h - what the DSP's test programs share: the DSP's log
 *    (dsplibs_debug_level and dsplibs_debug_printf, defined in
 *    test_util.cpp: to stderr, DSP_DEBUG's level) and Gaussian noise
 */
#ifndef DMODEM_TEST_UTIL_H
#define DMODEM_TEST_UTIL_H

/* Gaussian noise of unit power: Box-Muller on two of rand()'s, never
   log(0) */
double gauss();

#endif
