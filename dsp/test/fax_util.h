/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fax_util.h - what the fax tests share: the test documents, pages
 *    compared, G.711 and noise (test_util.h) for the line between two
 *    machines
 */
#ifndef DMODEM_FAX_UTIL_H
#define DMODEM_FAX_UTIL_H

#include <string>

#include <gtest/gtest.h>

/* a file of this test's own in the build directory (fax/<test>_<what>) */
std::string fax_file(const char *what);

namespace dsp {
struct T4Page;
}

/* a test page (fine: 196 lines per inch) of so many "letters" placed by
   rand() from seed */
void test_page(dsp::T4Page *p, int fine, int seed, int letters);

/* the test document of two pages (one standard, one fine), their seeds
   seed and seed + 1; light: with less on them, quicker to send; rtc:
   each page's data ending in RTC */
testing::AssertionResult write_test_doc(const std::string &path, bool light,
					bool rtc = false, int seed = 1);

/* the same pages in both files */
testing::AssertionResult same_pages(const std::string &a,
				    const std::string &b);

/* mu-law there and back */
short g711_ulaw(short x);

/* Gaussian noise: gauss() */
#include "test_util.h"

#endif
