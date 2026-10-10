/*
 *
 *    t4.c - ITU-T T.4 (modified Huffman, modified READ) and T.6 (MMR)
 *           coding of fax pages
 *
 *    Pages are kept as bitmaps; the decoders work on the changing
 *    elements of the rows (where the colour changes), as T.4 4.2 does.
 *
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "t4.h"


/*
 *    the code tables (T.4 Tables 1-4), as the bits go out
 */

struct code {
	int run;
	const char *bits;
};

static const struct code white_codes[] = {
	{0, "00110101"}, {1, "000111"}, {2, "0111"}, {3, "1000"},
	{4, "1011"}, {5, "1100"}, {6, "1110"}, {7, "1111"},
	{8, "10011"}, {9, "10100"}, {10, "00111"}, {11, "01000"},
	{12, "001000"}, {13, "000011"}, {14, "110100"}, {15, "110101"},
	{16, "101010"}, {17, "101011"}, {18, "0100111"}, {19, "0001100"},
	{20, "0001000"}, {21, "0010111"}, {22, "0000011"}, {23, "0000100"},
	{24, "0101000"}, {25, "0101011"}, {26, "0010011"}, {27, "0100100"},
	{28, "0011000"}, {29, "00000010"}, {30, "00000011"}, {31, "00011010"},
	{32, "00011011"}, {33, "00010010"}, {34, "00010011"}, {35, "00010100"},
	{36, "00010101"}, {37, "00010110"}, {38, "00010111"}, {39, "00101000"},
	{40, "00101001"}, {41, "00101010"}, {42, "00101011"}, {43, "00101100"},
	{44, "00101101"}, {45, "00000100"}, {46, "00000101"}, {47, "00001010"},
	{48, "00001011"}, {49, "01010010"}, {50, "01010011"}, {51, "01010100"},
	{52, "01010101"}, {53, "00100100"}, {54, "00100101"}, {55, "01011000"},
	{56, "01011001"}, {57, "01011010"}, {58, "01011011"}, {59, "01001010"},
	{60, "01001011"}, {61, "00110010"}, {62, "00110011"}, {63, "00110100"},
	{64, "11011"}, {128, "10010"}, {192, "010111"}, {256, "0110111"},
	{320, "00110110"}, {384, "00110111"}, {448, "01100100"},
	{512, "01100101"}, {576, "01101000"}, {640, "01100111"},
	{704, "011001100"}, {768, "011001101"}, {832, "011010010"},
	{896, "011010011"}, {960, "011010100"}, {1024, "011010101"},
	{1088, "011010110"}, {1152, "011010111"}, {1216, "011011000"},
	{1280, "011011001"}, {1344, "011011010"}, {1408, "011011011"},
	{1472, "010011000"}, {1536, "010011001"}, {1600, "010011010"},
	{1664, "011000"}, {1728, "010011011"},
	{-1, NULL}
};

static const struct code black_codes[] = {
	{0, "0000110111"}, {1, "010"}, {2, "11"}, {3, "10"}, {4, "011"},
	{5, "0011"}, {6, "0010"}, {7, "00011"}, {8, "000101"}, {9, "000100"},
	{10, "0000100"}, {11, "0000101"}, {12, "0000111"}, {13, "00000100"},
	{14, "00000111"}, {15, "000011000"}, {16, "0000010111"},
	{17, "0000011000"}, {18, "0000001000"}, {19, "00001100111"},
	{20, "00001101000"}, {21, "00001101100"}, {22, "00000110111"},
	{23, "00000101000"}, {24, "00000010111"}, {25, "00000011000"},
	{26, "000011001010"}, {27, "000011001011"}, {28, "000011001100"},
	{29, "000011001101"}, {30, "000001101000"}, {31, "000001101001"},
	{32, "000001101010"}, {33, "000001101011"}, {34, "000011010010"},
	{35, "000011010011"}, {36, "000011010100"}, {37, "000011010101"},
	{38, "000011010110"}, {39, "000011010111"}, {40, "000001101100"},
	{41, "000001101101"}, {42, "000011011010"}, {43, "000011011011"},
	{44, "000001010100"}, {45, "000001010101"}, {46, "000001010110"},
	{47, "000001010111"}, {48, "000001100100"}, {49, "000001100101"},
	{50, "000001010010"}, {51, "000001010011"}, {52, "000000100100"},
	{53, "000000110111"}, {54, "000000111000"}, {55, "000000100111"},
	{56, "000000101000"}, {57, "000001011000"}, {58, "000001011001"},
	{59, "000000101011"}, {60, "000000101100"}, {61, "000001011010"},
	{62, "000001100110"}, {63, "000001100111"},
	{64, "0000001111"}, {128, "000011001000"}, {192, "000011001001"},
	{256, "000001011011"}, {320, "000000110011"}, {384, "000000110100"},
	{448, "000000110101"}, {512, "0000001101100"}, {576, "0000001101101"},
	{640, "0000001001010"}, {704, "0000001001011"}, {768, "0000001001100"},
	{832, "0000001001101"}, {896, "0000001110010"}, {960, "0000001110011"},
	{1024, "0000001110100"}, {1088, "0000001110101"},
	{1152, "0000001110110"}, {1216, "0000001110111"},
	{1280, "0000001010010"}, {1344, "0000001010011"},
	{1408, "0000001010100"}, {1472, "0000001010101"},
	{1536, "0000001011010"}, {1600, "0000001011011"},
	{1664, "0000001100100"}, {1728, "0000001100101"},
	{-1, NULL}
};

/* make-up codes for both colours (Table 3) */
static const struct code ext_codes[] = {
	{1792, "00000001000"}, {1856, "00000001100"}, {1920, "00000001101"},
	{1984, "000000010010"}, {2048, "000000010011"}, {2112, "000000010100"},
	{2176, "000000010101"}, {2240, "000000010110"}, {2304, "000000010111"},
	{2368, "000000011100"}, {2432, "000000011101"}, {2496, "000000011110"},
	{2560, "000000011111"},
	{-1, NULL}
};

/* two dimensional modes (Table 4); the vertical ones by their offset */
enum { M_PASS = 100, M_HORIZ, M_V0 = 110 };   /* M_V0 + d, d -3..3 */

static const struct code mode_codes[] = {
	{M_PASS, "0001"}, {M_HORIZ, "001"},
	{M_V0, "1"}, {M_V0 + 1, "011"}, {M_V0 + 2, "000011"},
	{M_V0 + 3, "0000011"}, {M_V0 - 1, "010"}, {M_V0 - 2, "000010"},
	{M_V0 - 3, "0000010"},
	{-1, NULL}
};

#define EOL_BITS        12      /* 000000000001 */


/*
 *    decoding trees, built from the tables once
 */

#define TREE_MAX 512

struct tree {
	short next[TREE_MAX][2];        /* 0: none */
	short value[TREE_MAX];          /* -1: inner node */
	int n;
};

static struct tree white_tree, black_tree, mode_tree;
static pthread_once_t trees_once = PTHREAD_ONCE_INIT;

static void tree_add(struct tree *t, const struct code *c)
{
	for (; c->bits; c++) {
		int node = 0;
		const char *b;
		for (b = c->bits; *b; b++) {
			int k = *b - '0';
			if (!t->next[node][k]) {
				t->next[node][k] = (short)t->n;
				t->value[t->n] = -1;
				t->n++;
			}
			node = t->next[node][k];
		}
		t->value[node] = (short)c->run;
	}
}

static void trees_build(void)
{
	struct tree *t[] = { &white_tree, &black_tree, &mode_tree };
	int k;
	for (k = 0; k < 3; k++) {
		memset(t[k], 0, sizeof(*t[k]));
		t[k]->n = 1;
		t[k]->value[0] = -1;
	}
	tree_add(&white_tree, white_codes);
	tree_add(&white_tree, ext_codes);
	tree_add(&black_tree, black_codes);
	tree_add(&black_tree, ext_codes);
	tree_add(&mode_tree, mode_codes);
}


/*
 *    pages and bit strings
 */

int t4_page_init(struct t4_page *p, int width, int fine)
{
	memset(p, 0, sizeof(*p));
	p->width = width;
	p->fine = fine;
	return 0;
}

void t4_page_free(struct t4_page *p)
{
	free(p->bits);
	p->bits = NULL;
	p->rows = p->cap = 0;
}

unsigned char *t4_page_add_row(struct t4_page *p, const unsigned char *row)
{
	int stride = T4_STRIDE(p->width);
	unsigned char *r;
	if (p->rows == p->cap) {
		int cap = p->cap ? p->cap * 2 : 1200;
		unsigned char *b = realloc(p->bits, (size_t)cap * stride);
		if (!b)
			return NULL;
		p->bits = b;
		p->cap = cap;
	}
	r = p->bits + (size_t)p->rows * stride;
	if (row)
		memcpy(r, row, stride);
	else if (p->rows)
		memcpy(r, r - stride, stride);
	else
		memset(r, 0, stride);
	p->rows++;
	return r;
}

void t4_bits_put(struct t4_bits *b, unsigned code, int len)
{
	while (len-- > 0) {
		if (b->n == b->cap * 8) {
			size_t cap = b->cap ? b->cap * 2 : 4096;
			unsigned char *nb = realloc(b->buf, cap);
			if (!nb)
				return;
			memset(nb + b->cap, 0, cap - b->cap);
			b->buf = nb;
			b->cap = cap;
		}
		if ((code >> len) & 1)
			b->buf[b->n >> 3] |= 0x80 >> (b->n & 7);
		b->n++;
	}
}

void t4_bits_free(struct t4_bits *b)
{
	free(b->buf);
	memset(b, 0, sizeof(*b));
}

static void put_code(struct t4_bits *b, const char *bits)
{
	for (; *bits; bits++)
		t4_bits_put(b, *bits - '0', 1);
}

static const char *code_of(const struct code *t, int run)
{
	for (; t->bits; t++)
		if (t->run == run)
			return t->bits;
	return NULL;
}


/*
 *    rows and their changing elements
 */

static int pel(const unsigned char *row, int x)
{
	return (row[x >> 3] >> (7 - (x & 7))) & 1;
}

/* where the colour changes (the first to black), then width twice */
static int row_changes(const unsigned char *row, int width, int *ch)
{
	int n = 0, x, c = 0;
	for (x = 0; x < width; x++)
		if (pel(row, x) != c) {
			ch[n++] = x;
			c ^= 1;
		}
	ch[n] = ch[n + 1] = width;
	return n;
}

static void row_from_changes(unsigned char *row, int width, const int *ch,
			     int n)
{
	int k, x;
	memset(row, 0, T4_STRIDE(width));
	for (k = 0; k + 1 <= n; k += 2) {
		int end = k + 1 < n ? ch[k + 1] : width;
		for (x = ch[k]; x < end && x < width; x++)
			row[x >> 3] |= 0x80 >> (x & 7);
	}
}


/*
 *    the MH encoder
 */

static void put_run(struct t4_bits *b, int run, int black)
{
	const struct code *t = black ? black_codes : white_codes;
	while (run >= 2560) {
		put_code(b, code_of(ext_codes, 2560));
		run -= 2560;
	}
	if (run >= 64) {
		int m = run / 64 * 64;
		put_code(b, m > 1728 ? code_of(ext_codes, m) : code_of(t, m));
		run -= m;
	}
	put_code(b, code_of(t, run));
}

static void put_eol(struct t4_bits *b, int align)
{
	if (align)
		while ((b->n + EOL_BITS) % 8)
			t4_bits_put(b, 0, 1);
	t4_bits_put(b, 1, EOL_BITS);
}

void t4_encode_mh(const struct t4_page *p, struct t4_bits *out,
		  int min_bits, int align, int rtc)
{
	int stride = T4_STRIDE(p->width), y, k;
	int *ch = malloc((p->width + 2) * sizeof(int));

	if (!ch)
		return;
	for (y = 0; y < p->rows; y++) {
		size_t start = out->n;
		int n, a0 = 0;
		put_eol(out, align);
		n = row_changes(p->bits + (size_t)y * stride, p->width, ch);
		for (k = 0; k <= n; k++) {
			int end = k < n ? ch[k] : p->width;
			put_run(out, end - a0, k & 1);
			a0 = end;
		}
		/* fill up to the minimum line time (counted from the EOL) */
		while (out->n - start < (size_t)min_bits)
			t4_bits_put(out, 0, 1);
	}
	if (rtc)
		for (k = 0; k < 6; k++)
			put_eol(out, align);
	free(ch);
}


/*
 *    decoders
 */

struct reader {
	const unsigned char *data;
	size_t pos, end;                /* bits */
	int msb_first;
};

static int rd_bit(struct reader *r)
{
	int b;
	if (r->pos >= r->end)
		return -1;
	b = r->data[r->pos >> 3];
	b = r->msb_first ? b >> (7 - (r->pos & 7)) : b >> (r->pos & 7);
	r->pos++;
	return b & 1;
}

static int rd_code(struct reader *r, const struct tree *t)
{
	int node = 0, b;
	do {
		if ((b = rd_bit(r)) < 0)
			return -1;
		node = t->next[node][b];
		if (!node)
			return -2;              /* no such code */
	} while (t->value[node] < 0);
	return t->value[node];
}

/* a run of one colour: make-up codes, then a terminating one */
static int rd_run(struct reader *r, int black)
{
	const struct tree *t = black ? &black_tree : &white_tree;
	int run = 0, v;
	do {
		v = rd_code(r, t);
		if (v < 0)
			return v;
		run += v;
	} while (v >= 64);
	return run;
}

/* one MH row into ch; the number of changes, -1 on an error */
static int decode_1d(struct reader *r, int width, int *ch)
{
	int x = 0, n = 0, black = 0, run;
	while (x < width) {
		run = rd_run(r, black);
		if (run < 0)
			return -1;
		x += run;
		if (x > width)
			return -1;
		if (x < width)
			ch[n++] = x;
		black ^= 1;
	}
	ch[n] = ch[n + 1] = width;
	return n;
}

/* one row two dimensionally, against the nref changes of the row above
   (ref[nref], ref[nref + 1] are the width) */
static int decode_2d(struct reader *r, int width, const int *ref, int nref,
		     int *ch)
{
	int a0 = -1, colour = 0, n = 0, b1, b2, i, mode;

	while (a0 < width) {
		/* b1: the first change on the row above right of a0 to the
		   colour opposite a0's (even ones are changes to black) */
		for (i = 0; i < nref; i++)
			if (ref[i] > a0 && (i & 1) == colour)
				break;
		b1 = i < nref ? ref[i] : width;
		b2 = i + 1 < nref ? ref[i + 1] : width;
		mode = rd_code(r, &mode_tree);
		if (mode < 0)
			return -1;
		if (mode == M_PASS) {
			if (b2 >= width)
				return -1;
			a0 = b2;
		} else if (mode == M_HORIZ) {
			int x = a0 < 0 ? 0 : a0, r1, r2;
			r1 = rd_run(r, colour);
			r2 = r1 < 0 ? -1 : rd_run(r, !colour);
			if (r1 < 0 || r2 < 0 || x + r1 + r2 > width)
				return -1;
			ch[n++] = x + r1;
			ch[n++] = x + r1 + r2;
			a0 = x + r1 + r2;
		} else {
			int a1 = b1 + (mode - M_V0);
			if (a1 < 0 || a1 > width || a1 < a0)
				return -1;
			ch[n++] = a1;
			a0 = a1;
			colour ^= 1;
		}
		if (n > width + 1)
			return -1;
	}
	/* changes at the right edge mean nothing */
	while (n && ch[n - 1] >= width)
		n--;
	ch[n] = ch[n + 1] = width;
	return n;
}


/*
 *    receiving, bit by bit, with EOLs
 */

void t4_rx_init(struct t4_rx *r, struct t4_page *page, int mr)
{
	pthread_once(&trees_once, trees_build);
	memset(r, 0, sizeof(*r));
	r->page = page;
	r->width = page->width;
	r->mr = mr;
	r->ref = calloc(1, T4_STRIDE(r->width));
}

void t4_rx_free(struct t4_rx *r)
{
	t4_bits_free(&r->line);
	free(r->ref);
	r->ref = NULL;
}

static int line_empty(const struct t4_bits *b)
{
	size_t k;
	for (k = 0; k < (b->n + 7) / 8; k++)
		if (b->buf[k])
			return 0;
	return 1;
}

/* the bits of a row, between two EOLs */
static void rx_row(struct t4_rx *r, int two_d)
{
	int w = r->width, n, nref;
	int *ch = malloc((w + 4) * sizeof(int)), *ref = NULL;
	struct reader rd = { r->line.buf, 0, r->line.n, 1 };
	unsigned char *row;

	if (!ch)
		return;
	if (two_d) {
		ref = malloc((w + 4) * sizeof(int));
		nref = row_changes(r->ref, w, ref);
		n = ref ? decode_2d(&rd, w, ref, nref, ch) : -1;
	} else {
		n = decode_1d(&rd, w, ch);
	}
	if (n < 0) {
		/* a bad row: the one above again */
		r->page->bad_rows++;
		t4_page_add_row(r->page, r->page->rows ? NULL : r->ref);
	} else {
		row = t4_page_add_row(r->page, NULL);
		if (row) {
			row_from_changes(row, w, ch, n);
			memcpy(r->ref, row, T4_STRIDE(w));
		}
	}
	free(ch);
	free(ref);
}

int t4_rx_bit(struct t4_rx *r, int bit)
{
	if (r->done)
		return 1;
	if (r->tag_pending) {
		/* MR: after the EOL, 1 for a one dimensional row */
		r->tag_pending = 0;
		r->next_2d = !bit;
		return 0;
	}
	if (!bit) {
		r->zeros++;
		t4_bits_put(&r->line, 0, 1);
		return 0;
	}
	if (r->zeros >= EOL_BITS - 1) {
		/* an EOL: the row before it is complete (the zeros before
		   it stay: the last code of the row may end in some, and the
		   decoder stops at the width) */
		if (r->started && r->line.n && !line_empty(&r->line)) {
			rx_row(r, r->mr && r->next_2d);
			r->eols = 1;
		} else {
			r->eols++;
		}
		memset(r->line.buf, 0, r->line.cap);
		r->line.n = 0;
		r->started = 1;
		r->zeros = 0;
		if (r->mr)
			r->tag_pending = 1;
		if (r->eols >= 6) {             /* RTC */
			r->done = 1;
			return 1;
		}
		return 0;
	}
	r->zeros = 0;
	if (r->started)
		t4_bits_put(&r->line, 1, 1);
	return 0;
}

void t4_rx_end(struct t4_rx *r)
{
	if (!r->done && r->started && r->line.n && !line_empty(&r->line))
		rx_row(r, r->mr && r->next_2d);
	r->done = 1;
}


/*
 *    whole pages from a buffer (TIFF)
 */

int t4_decode(struct t4_page *p, const unsigned char *data, size_t len,
	      int compression, int t4opts, int msb_first, int rows)
{
	int w = p->width, n, nref, y = 0;
	int *ch, *ref;
	unsigned char *row;
	struct reader rd = { data, 0, len * 8, msb_first };

	pthread_once(&trees_once, trees_build);
	if (compression == 3) {
		/* with EOLs: as on the line */
		struct t4_rx rx;
		size_t i;
		t4_rx_init(&rx, p, t4opts & 1);
		for (i = 0; i < rd.end && (!rows || p->rows < rows); i++)
			if (t4_rx_bit(&rx, rd_bit(&rd)))
				break;
		t4_rx_end(&rx);
		t4_rx_free(&rx);
		return 0;
	}
	ch = malloc((w + 4) * sizeof(int));
	ref = malloc((w + 4) * sizeof(int));
	if (!ch || !ref) {
		free(ch);
		free(ref);
		return -1;
	}
	ref[0] = ref[1] = w;            /* white above the first row */
	nref = 0;
	while (!rows || y < rows) {
		if (compression == 2) {
			n = decode_1d(&rd, w, ch);
			rd.pos = (rd.pos + 7) / 8 * 8;  /* rows byte aligned */
		} else {
			n = decode_2d(&rd, w, ref, nref, ch);
		}
		if (n < 0)
			break;                  /* the end (EOFB) or an error */
		row = t4_page_add_row(p, NULL);
		if (!row)
			break;
		row_from_changes(row, w, ch, n);
		nref = row_changes(row, w, ref);
		y++;
		if (rd.pos >= rd.end)
			break;
	}
	free(ch);
	free(ref);
	return 0;
}
