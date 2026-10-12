/*
 *    fax_docs.cpp - the fax tests' documents and their comparison
 *    (fax_util.h)
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fax_util.h"
#include "t4.h"

using namespace dsp;

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

static void set(T4Page *p, int x, int y)
{
	if (x >= 0 && x < p->width && y >= 0 && y < p->rows)
		p->bits[(size_t)y * T4_STRIDE(p->width) + (x >> 3)] |= 0x80 >> (x & 7);
}

/* a page with something of everything: text-like strokes, boxes, a
   checkerboard, single pels, runs reaching the edges */
static void test_page(T4Page *p, int fine, int seed, int letters)
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

testing::AssertionResult write_test_doc(const std::string &path, bool light)
{
	TiffOut t;
	T4Page p;
	int letters = light ? 300 : 4000;
	if (t.open(path.c_str()))
		return testing::AssertionFailure() << "cannot write " << path;
	test_page(&p, 0, 1, letters);
	t.write_page(&p);
	p.release();
	test_page(&p, 1, 2, letters);
	t.write_page(&p);
	p.release();
	if (t.close())
		return testing::AssertionFailure() << "cannot write " << path;
	return testing::AssertionSuccess();
}

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
		    memcmp(pa.bits, pb.bits, (size_t)pa.rows * T4_STRIDE(T4_WIDTH))) {
			int y, diff = 0, first = -1;
			for (y = 0; y < pa.rows && y < pb.rows; y++)
				if (memcmp(pa.bits + (size_t)y * 216,
					   pb.bits + (size_t)y * 216, 216)) {
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
	ta.close();
	tb.close();
	if (!why.empty())
		return testing::AssertionFailure() << a << " vs " << b << ": " << why;
	return testing::AssertionSuccess() << n << " pages the same";
}

short g711_ulaw(short x)
{
	int s = x < 0, m = s ? -x : x, e, q;
	if (m > 32635)
		m = 32635;
	m += 0x84;
	for (e = 7; e > 0 && !(m & (0x4000 >> (7 - e))); e--)
		;
	q = (m >> (e + 3)) & 15;
	m = ((q << 3) + 0x84) << e;
	m -= 0x84;
	return (short)(s ? -m : m);
}

double gauss()
{
	double u = (rand() + 1.0) / (RAND_MAX + 2.0);
	double v = (rand() + 1.0) / (RAND_MAX + 2.0);
	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}
