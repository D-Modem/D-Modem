/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    fax_docs.cpp - the fax tests' documents and their comparison
 *    (fax_util.h)
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fax_util.h"
#include "g711.h"
#include "t4.h"

using namespace dsp;

/* (fax_util.h) fax/<suite>_<test>_<what>, a parameterized test's "/" as
   "_"; fax/ made if need be */
std::string fax_file(const char *what)
{
	const testing::TestInfo *t =
		testing::UnitTest::GetInstance()->current_test_info();
	std::string n = std::string(t->test_suite_name()) + "_" + t->name();
	for (char &c : n)
		if (c == '/')
			c = '_';
	mkdir("fax", 0777);
	return "fax/" + n + "_" + what;
}

/* pel (x, y) of p black; nothing outside the page */
static void set(T4Page *p, int x, int y)
{
	if (x >= 0 && x < p->width && y >= 0 && y < p->rows)
		p->bits[(size_t)y * T4_STRIDE(p->width) + (x >> 3)] |= 0x80 >> (x & 7);
}

/* (fax_util.h) a page with something of everything: text-like
   strokes, boxes, a checkerboard, single pels, runs reaching the edges */
void test_page(T4Page *p, int fine, int seed, int letters)
{
	int y, x, k, rows = fine ? 2200 : 1100;
	srand(seed);
	p->init(T4_WIDTH, fine);
	for (y = 0; y < rows; y++)
		p->add_row(NULL);
	for (k = 0; k < letters; k++) {         /* "letters" */
		int x0 = rand() % T4_WIDTH, y0 = rand() % rows;
		int w = 2 + rand() % 18, h = 2 + rand() % 30;
		for (y = y0; y < y0 + h; y++)
			for (x = x0; x < x0 + w; x++)
				if ((x - x0 < 3) || (y - y0 < 2) || rand() % 7 == 0)
					set(p, x, y);
	}
	for (y = 100; y < 300; y++)             /* a checkerboard */
		for (x = 200; x < 600; x++)
			if (((x / 3) ^ (y / 3)) & 1)
				set(p, x, y);
	for (y = 400; y < 420; y++)             /* edge to edge */
		for (x = 0; x < T4_WIDTH; x++)
			set(p, x, y);
	for (y = 500; y < 700; y++) {           /* edges, single pels */
		set(p, 0, y);
		set(p, T4_WIDTH - 1, y);
		set(p, (y * 37) % T4_WIDTH, y);
	}
}

/* (fax_util.h) a standard page (seed) and a fine one (seed + 1) into a
   TIFF; light: 300 "letters" a page instead of 4000 */
testing::AssertionResult write_test_doc(const std::string &path, bool light,
					bool rtc, int seed)
{
	TiffOut t;
	T4Page p;
	int letters = light ? 300 : 4000;
	if (t.open(path.c_str()))
		return testing::AssertionFailure() << "cannot write " << path;
	t.rtc = rtc;
	test_page(&p, 0, seed, letters);
	t.write_page(&p);
	p.release();
	test_page(&p, 1, seed + 1, letters);
	t.write_page(&p);
	p.release();
	if (t.close())
		return testing::AssertionFailure() << "cannot write " << path;
	return testing::AssertionSuccess();
}

/* (fax_util.h) page by page: rows, resolution and the bits; what differs
   in the failure's message, and different page counts */
testing::AssertionResult same_pages(const std::string &a,
				    const std::string &b)
{
	TiffIn ta, tb;
	T4Page pa, pb;
	int n = 0, ra, rb;
	std::string why;

	if (ta.open(a.c_str()) || tb.open(b.c_str()))
		return testing::AssertionFailure() << "cannot open " << a <<
			" or " << b;
	for (;;) {
		ra = ta.read_page(&pa);
		rb = tb.read_page(&pb);
		if (ra != 1 || rb != 1)
			break;
		n++;
		if (pa.rows != pb.rows || pa.fine != pb.fine ||
		    memcmp(pa.bits.data(), pb.bits.data(), (size_t)pa.rows * T4_STRIDE(T4_WIDTH))) {
			int y, diff = 0, first = -1;
			for (y = 0; y < pa.rows && y < pb.rows; y++)
				if (memcmp(pa.bits.data() + (size_t)y * 216,
					   pb.bits.data() + (size_t)y * 216, 216)) {
					if (first < 0)
						first = y;
					diff++;
				}
			why += "page " + std::to_string(n) + ": rows " +
				std::to_string(pa.rows) + "/" +
				std::to_string(pb.rows) + ", fine " +
				std::to_string(pa.fine) + "/" +
				std::to_string(pb.fine) + ", " +
				std::to_string(diff) + " rows differ (first " +
				std::to_string(first) + "); ";
		}
		pa.release();
		pb.release();
	}
	if (ra != rb)
		why += "page counts differ (" + std::to_string(ra) + " " +
			std::to_string(rb) + ")";
	/* (two broken or empty files are not the same document) */
	if (ra < 0 || rb < 0)
		why += "read error; ";
	if (!n)
		why += "no pages; ";
	ta.close();
	tb.close();
	if (!why.empty())
		return testing::AssertionFailure() << a << " vs " << b << ": " << why;
	return testing::AssertionSuccess() << n << " pages the same";
}

/* (fax_util.h) x coded to a mu-law octet (G.711's sign and magnitude,
   clipped at 32635) and decoded again to the middle of its interval */
short g711_ulaw(short x)
{
	return (short)dsp::g711::decode(0, dsp::g711::encode_sm(0, x));
}

