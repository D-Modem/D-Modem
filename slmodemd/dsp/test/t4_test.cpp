/*
 *    t4_test.cpp - the fax page coding and TIFF files
 *
 *    t4_test write <file.tif>          a test document of two pages
 *    t4_test light <file.tif>          the same with less on them
 *    t4_test compare <a.tif> <b.tif>   the same pages? (exit status)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t4.h"

using namespace dsp;

static void set(struct t4_page *p, int x, int y)
{
	if (x >= 0 && x < p->width && y >= 0 && y < p->rows)
		p->bits[(size_t)y * T4_STRIDE(p->width) + (x >> 3)] |= 0x80 >> (x & 7);
}

/* a page with something of everything: text-like strokes, boxes, a
   checkerboard, single pels, runs reaching the edges */
static int letters = 4000;

static void test_page(struct t4_page *p, int fine, int seed)
{
	int y, x, k, rows = fine ? 2200 : 1100;
	srand(seed);
	t4_page_init(p, T4_WIDTH, fine);
	for (y = 0; y < rows; y++)
		t4_page_add_row(p, NULL);
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

static int compare(const char *a, const char *b)
{
	struct tiff_in ta, tb;
	struct t4_page pa, pb;
	int n = 0, bad = 0, ra, rb;

	if (tiff_in_open(&ta, a) || tiff_in_open(&tb, b)) {
		fprintf(stderr, "cannot open\n");
		return 2;
	}
	for (;;) {
		ra = tiff_in_page(&ta, &pa);
		rb = tiff_in_page(&tb, &pb);
		if (ra != 1 || rb != 1)
			break;
		n++;
		if (pa.rows != pb.rows || pa.fine != pb.fine ||
		    memcmp(pa.bits, pb.bits, (size_t)pa.rows * T4_STRIDE(T4_WIDTH))) {
			int y, diff = 0;
			for (y = 0; y < pa.rows && y < pb.rows; y++)
				if (memcmp(pa.bits + (size_t)y * 216,
					   pb.bits + (size_t)y * 216, 216)) {
					int x, nx = 0, x0 = -1;
					for (x = 0; x < 1728; x++)
						if (((pa.bits[(size_t)y * 216 + x / 8] ^
						      pb.bits[(size_t)y * 216 + x / 8]) >> (7 - x % 8)) & 1) {
							nx++;
							if (x0 < 0)
								x0 = x;
						}
					if (!diff)
						printf("page %d: row %d differs first, in %d "
						       "pixels from x %d\n", n, y, nx, x0);
					diff++;
				}
			printf("page %d differs: rows %d/%d fine %d/%d, %d rows differ\n",
			       n, pa.rows, pb.rows, pa.fine, pb.fine, diff);
			bad++;
		}
		t4_page_free(&pa);
		t4_page_free(&pb);
	}
	if (ra != rb) {
		printf("page counts differ (%d %d)\n", ra, rb);
		bad++;
	}
	printf("%d pages compared: %s\n", n, bad ? "FAIL" : "PASS");
	tiff_in_close(&ta);
	tiff_in_close(&tb);
	return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "light")) {
		letters = 300;
		argv[1] = const_cast<char *>("write");
	}
	if (argc == 3 && !strcmp(argv[1], "write")) {
		struct tiff_out t;
		struct t4_page p;
		if (tiff_out_open(&t, argv[2]))
			return 2;
		test_page(&p, 0, 1);
		tiff_out_page(&t, &p);
		t4_page_free(&p);
		test_page(&p, 1, 2);
		tiff_out_page(&t, &p);
		t4_page_free(&p);
		return tiff_out_close(&t) ? 2 : 0;
	}
	if (argc == 4 && !strcmp(argv[1], "compare"))
		return compare(argv[2], argv[3]);
	fprintf(stderr, "usage: t4_test write <tif> | compare <a> <b>\n");
	return 2;
}
