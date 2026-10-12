/*
 *    fax_util.h - what the fax tests share: the test documents, pages
 *    compared, G.711 and noise for the line between two machines
 */
#ifndef DMODEM_FAX_UTIL_H
#define DMODEM_FAX_UTIL_H

#include <string>

#include <gtest/gtest.h>

/* a file of this test's own in the build directory (fax/<test>_<what>) */
std::string fax_file(const char *what);

/* the test document of two pages (one standard, one fine); light: with
   less on them, quicker to send */
testing::AssertionResult write_test_doc(const std::string &path, bool light);

/* the same pages in both files */
testing::AssertionResult same_pages(const std::string &a,
				    const std::string &b);

/* mu-law there and back */
short g711_ulaw(short x);

/* Gaussian noise of unit power (rand()) */
double gauss();

#endif
