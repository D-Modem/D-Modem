/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    t4.cpp - ITU-T T.4 (modified Huffman, modified READ) and T.6 (MMR)
 *           coding of fax pages
 *
 *    Pages are kept as bitmaps; the decoders work on the changing
 *    elements of the rows (where the colour changes), as T.4 4.2 does.
 *
 */

#include <cstdlib>
#include <cstring>
#include <mutex>

#include "dm_types.h"
#include "reinit.h"
#include "t4.h"

namespace dsp {


/*
 *    the code tables (T.4 Tables 1-4), as the bits go out
 */

struct code {
	s16 run;
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
	{-1, nullptr}
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
	{-1, nullptr}
};

/* make-up codes for both colours (Table 3) */
static const struct code ext_codes[] = {
	{1792, "00000001000"}, {1856, "00000001100"}, {1920, "00000001101"},
	{1984, "000000010010"}, {2048, "000000010011"}, {2112, "000000010100"},
	{2176, "000000010101"}, {2240, "000000010110"}, {2304, "000000010111"},
	{2368, "000000011100"}, {2432, "000000011101"}, {2496, "000000011110"},
	{2560, "000000011111"},
	{-1, nullptr}
};

/* two dimensional modes (Table 4); the vertical ones by their offset */
enum { M_PASS = 100, M_HORIZ, M_V0 = 110 };   /* M_V0 + d, d -3..3 */

static const struct code mode_codes[] = {
	{M_PASS, "0001"}, {M_HORIZ, "001"},
	{M_V0, "1"}, {M_V0 + 1, "011"}, {M_V0 + 2, "000011"},
	{M_V0 + 3, "0000011"}, {M_V0 - 1, "010"}, {M_V0 - 2, "000010"},
	{M_V0 - 3, "0000010"},
	{-1, nullptr}
};

#define EOL_BITS        12      /* 000000000001 */


/*
 *    decoding trees, built from the tables once
 */

#define TREE_MAX 512

struct tree {
	s16 next[TREE_MAX][2];          /* 0: none */
	s16 value[TREE_MAX];            /* -1: inner node */
	s16 n;
};

static struct tree white_tree, black_tree, mode_tree;
static std::once_flag trees_once;

/* the codes of a table into a decoding tree */
static void tree_add(struct tree *t, const struct code *c)
{
	for (; c->bits; c++) {
		s16 node = 0;
		const char *b;
		for (b = c->bits; *b; b++) {
			s16 k = *b - '0';
			if (!t->next[node][k]) {
				t->next[node][k] = static_cast<s16>(t->n);
				t->value[t->n] = -1;
				t->n++;
			}
			node = t->next[node][k];
		}
		t->value[node] = static_cast<s16>(c->run);
	}
}

/* the decoding trees from the tables (once, by std::call_once) */
static void trees_build(void)
{
	struct tree *t[] = { &white_tree, &black_tree, &mode_tree };
	s16 k;
	for (k = 0; k < 3; k++) {
		zero(*t[k]);
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

/* an empty page width_ pels wide; fine_: 196 lines per inch.  0 */
s16 T4Page::init(s16 width_, s16 fine_)
{
	reinit(*this);
	width = width_;
	fine = fine_;
	return 0;
}

/* free the rows */
void T4Page::release()
{
	free_buffer(bits);
	rows = cap = 0;
}

/* a row added at the bottom; NULL: a copy of the last one (white for
   the first).  The new row in the page, NULL when out of memory */
unsigned char *T4Page::add_row(const unsigned char *row)
{
	s16 stride = T4_STRIDE(width);
	unsigned char *r;
	if (rows == cap) {
		s32 cap_ = cap ? cap * 2 : 1200;
		if (!resize_zeroed(bits, static_cast<size_t>(cap_) * stride))
			return nullptr;
		cap = cap_;
	}
	r = bits.data() + static_cast<size_t>(rows) * stride;
	if (row)
		memcpy(r, row, stride);
	else if (rows)
		memcpy(r, r - stride, stride);
	else
		memset(r, 0, stride);
	rows++;
	return r;
}

/* the len lowest bits of code, the highest first (dropped when out of
   memory) */
void T4Bits::put(u16 code, s16 len)
{
	while (len-- > 0) {
		if (n == cap * 8) {
			size_t cap_ = cap ? cap * 2 : 4096;
			if (!resize_zeroed(buf, cap_))
				return;
			cap = cap_;
		}
		if ((code >> len) & 1)
			buf[n >> 3] |= 0x80 >> (n & 7);
		n++;
	}
}

/* free the bits, empty again */
void T4Bits::release()
{
	reinit(*this);
}

/* a code written as a string of '0' and '1' */
void T4Bits::put_code(const char *bits)
{
	for (; *bits; bits++)
		put(*bits - '0', 1);
}

/* the code of a run in a table, NULL if it has none */
static const char *code_of(const struct code *t, s16 run)
{
	for (; t->bits; t++)
		if (t->run == run)
			return t->bits;
	return nullptr;
}


/*
 *    rows and their changing elements
 */

/* pel x of a row: 1 for black */
static s16 pel(const unsigned char *row, s16 x)
{
	return (row[x >> 3] >> (7 - (x & 7))) & 1;
}

/* where the colour changes (the first to black), then width twice */
static s16 row_changes(const unsigned char *row, s16 width, s16 *ch)
{
	s16 n = 0, x, c = 0;
	for (x = 0; x < width; x++)
		if (pel(row, x) != c) {
			ch[n++] = x;
			c ^= 1;
		}
	ch[n] = ch[n + 1] = width;
	return n;
}

/* a row from its n changes: black from ch[0] to ch[1], from ch[2] to
   ch[3] ..., to the right edge if n is odd */
static void row_from_changes(unsigned char *row, s16 width, const s16 *ch,
			     s16 n)
{
	s16 k, x;
	memset(row, 0, T4_STRIDE(width));
	for (k = 0; k + 1 <= n; k += 2) {
		s16 end = k + 1 < n ? ch[k + 1] : width;
		for (x = ch[k]; x < end && x < width; x++)
			row[x >> 3] |= 0x80 >> (x & 7);
	}
}


/*
 *    the MH encoder
 */

/* a run of one colour in MH: make-up codes (Table 3 for 1792 and up),
   then the terminating code */
void T4Bits::put_run(s16 run, s16 black)
{
	const struct code *t = black ? black_codes : white_codes;
	while (run >= 2560) {
		put_code(code_of(ext_codes, 2560));
		run -= 2560;
	}
	if (run >= 64) {
		s16 m = run / 64 * 64;
		put_code(m > 1728 ? code_of(ext_codes, m) : code_of(t, m));
		run -= m;
	}
	put_code(code_of(t, run));
}

/* an EOL; align: fill zeros before it so that it ends on an octet
   boundary */
void T4Bits::put_eol(s16 align)
{
	if (align)
		while ((n + EOL_BITS) % 8)
			put(0, 1);
	put(1, EOL_BITS);
}

/* the page in MH (one dimensional) coding: an EOL before each row, rows
   padded with fill to at least min_bits, RTC at the end (rtc), EOLs
   ending on a byte boundary (align, as TIFF's T4Options bit 2) */
void T4Page::encode_mh(T4Bits *out, s16 min_bits, s16 align, s16 rtc) const
{
	s16 stride = T4_STRIDE(width), k;
	s32 y;
	std::vector<s16> ch_;
	s16 *ch;

	if (!alloc_zeroed(ch_, width + 2))
		return;
	ch = ch_.data();
	for (y = 0; y < rows; y++) {
		size_t start = out->n;
		s16 n, a0 = 0;
		out->put_eol(align);
		n = row_changes(bits.data() + static_cast<size_t>(y) * stride,
				width, ch);
		for (k = 0; k <= n; k++) {
			s16 end = k < n ? ch[k] : width;
			out->put_run(end - a0, k & 1);
			a0 = end;
		}
		/* fill up to the minimum line time (counted from the EOL) */
		while (out->n - start < static_cast<size_t>(min_bits))
			out->put(0, 1);
	}
	if (rtc)
		for (k = 0; k < 6; k++)
			out->put_eol(align);
}


/*
 *    decoders
 */

struct T4Reader {
	const unsigned char *data;
	size_t pos, end;                /* bits */
	s16 msb_first;

	s16 bit();
	s16 code(const struct tree *t);
	s32 run_length(s16 black);
	s16 decode_1d(s16 width, s16 *ch);
	s16 decode_2d(s16 width, const s16 *ref, s16 nref, s16 *ch);
};

/* the next bit, -1 at the end of the data */
s16 T4Reader::bit()
{
	s16 b;
	if (pos >= end)
		return -1;
	b = data[pos >> 3];
	b = msb_first ? b >> (7 - (pos & 7)) : b >> (pos & 7);
	pos++;
	return b & 1;
}

/* a code of tree t: its value; -1 at the end of the data, -2 for bits
   that are no code */
s16 T4Reader::code(const struct tree *t)
{
	s16 node = 0, b;
	do {
		if ((b = bit()) < 0)
			return -1;
		node = t->next[node][b];
		if (!node)
			return -2;              /* no such code */
	} while (t->value[node] < 0);
	return t->value[node];
}

/* a run of one colour: make-up codes, then a terminating one */
s32 T4Reader::run_length(s16 black)
{
	const struct tree *t = black ? &black_tree : &white_tree;
	s32 run = 0;
	s16 v;
	do {
		v = code(t);
		if (v < 0)
			return v;
		run += v;
	} while (v >= 64);
	return run;
}

/* one MH row into ch; the number of changes, -1 on an error */
s16 T4Reader::decode_1d(s16 width, s16 *ch)
{
	s32 x = 0, run;
	s16 n = 0, black = 0;
	while (x < width) {
		run = run_length(black);
		if (run < 0)
			return -1;
		x += run;
		if (x > width)
			return -1;
		if (x < width) {
			/* (runs of 0 one after another: ch has width + 4) */
			if (n > width)
				return -1;
			ch[n++] = x;
		}
		black ^= 1;
	}
	ch[n] = ch[n + 1] = width;
	return n;
}

/* one row two dimensionally, against the nref changes of the row above
   (ref[nref], ref[nref + 1] are the width) */
s16 T4Reader::decode_2d(s16 width, const s16 *ref, s16 nref, s16 *ch)
{
	s16 a0 = -1, colour = 0, n = 0, b1, b2, i, mode;

	while (a0 < width) {
		/* b1: the first change on the row above right of a0 to the
		   colour opposite a0's (even ones are changes to black) */
		for (i = 0; i < nref; i++)
			if (ref[i] > a0 && (i & 1) == colour)
				break;
		b1 = i < nref ? ref[i] : width;
		b2 = i + 1 < nref ? ref[i + 1] : width;
		mode = code(&mode_tree);
		if (mode < 0)
			return -1;
		if (mode == M_PASS) {
			if (b2 >= width)
				return -1;
			a0 = b2;
		} else if (mode == M_HORIZ) {
			s32 x = a0 < 0 ? 0 : a0, r1, r2;
			r1 = run_length(colour);
			r2 = r1 < 0 ? -1 : run_length(!colour);
			if (r1 < 0 || r2 < 0 || x + r1 + r2 > width)
				return -1;
			ch[n++] = x + r1;
			ch[n++] = x + r1 + r2;
			a0 = x + r1 + r2;
		} else {
			s16 a1 = b1 + (mode - M_V0);
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

/* rows to add to page_ as they come; mr_: MR coding (a tag bit after
   each EOL) */
void T4Rx::init(T4Page *page_, s16 mr_)
{
	std::call_once(trees_once, trees_build);
	reinit(*this);
	page = page_;
	width = page_->width;
	mr = mr_;
	alloc_zeroed(ref, T4_STRIDE(width));
}

/* free the buffers (the page stays) */
void T4Rx::release()
{
	line.release();
	free_buffer(ref);
}

/* no 1 among the bits: nothing but fill */
s16 T4Bits::line_empty() const
{
	size_t k;
	for (k = 0; k < (n + 7) / 8; k++)
		if (buf[k])
			return 0;
	return 1;
}

/* the bits of a row, between two EOLs */
void T4Rx::row(s16 two_d)
{
	s16 w = width, n, nref;
	std::vector<s16> ch_, ref_;
	s16 *ch;
	T4Reader rd = { line.buf.data(), 0, line.n, 1 };
	unsigned char *row;

	if (ref.empty() || !alloc_zeroed(ch_, w + 4))
		return;
	ch = ch_.data();
	if (two_d) {
		if (alloc_zeroed(ref_, w + 4)) {
			nref = row_changes(ref.data(), w, ref_.data());
			n = rd.decode_2d(w, ref_.data(), nref, ch);
		} else
			n = -1;
	} else {
		n = rd.decode_1d(w, ch);
	}
	if (n < 0) {
		/* a bad row: the one above again */
		page->bad_rows++;
		page->add_row(page->rows ? nullptr : ref.data());
	} else {
		row = page->add_row(nullptr);
		if (row) {
			row_from_changes(row, w, ch, n);
			memcpy(ref.data(), row, T4_STRIDE(w));
		}
	}
}

/* one bit; 1 once the page has ended (RTC).  An EOL completes the row
   before it */
s16 T4Rx::bit(s16 bit)
{
	if (done)
		return 1;
	if (tag_pending) {
		/* MR: after the EOL, 1 for a one dimensional row */
		tag_pending = 0;
		next_2d = !bit;
		return 0;
	}
	if (!bit) {
		zeros++;
		line.put(0, 1);
		return 0;
	}
	if (zeros >= EOL_BITS - 1) {
		/* an EOL: the row before it is complete (the zeros before
		   it stay: the last code of the row may end in some, and the
		   decoder stops at the width) */
		if (started && line.n && !line.line_empty()) {
			row(mr && next_2d);
			eols = 1;
		} else {
			eols++;
		}
		std::fill(line.buf.begin(), line.buf.end(), 0);
		line.n = 0;
		started = 1;
		zeros = 0;
		if (mr)
			tag_pending = 1;
		if (eols >= 6) {             /* RTC */
			done = 1;
			return 1;
		}
		return 0;
	}
	zeros = 0;
	if (started)
		line.put(1, 1);
	return 0;
}

/* the end of the data without RTC (the carrier went): the last row */
void T4Rx::end()
{
	if (!done && started && line.n && !line.line_empty())
		row(mr && next_2d);
	done = 1;
}


/*
 *    whole pages from a buffer (TIFF)
 */

/* a whole page from a buffer: compression as TIFF's (2 MH without EOLs,
   3 T.4 with EOLs, 1D or, t4opts & 1, 2D; 4 T.6), msb_first as TIFF's
   FillOrder 1; rows: how many to expect (0: until the end of data).
   The rows are added to the page; 0, or -1 when out of memory */
s16 T4Page::decode(const unsigned char *data, size_t len, s16 compression, u32 t4opts, s16 msb_first, s32 rows_)
{
	s16 w = width, n, nref;
	s32 y = 0;
	std::vector<s16> ch_, ref_;
	s16 *ch, *ref;
	unsigned char *row;
	T4Reader rd = { data, 0, len * 8, msb_first };

	std::call_once(trees_once, trees_build);
	if (compression == 3) {
		/* with EOLs: as on the line */
		T4Rx rx;
		size_t i;
		s32 start = rows;       /* (the page's rows so far: strips) */
		rx.init(this, t4opts & 1);
		for (i = 0; i < rd.end && (!rows_ || rows - start < rows_); i++)
			if (rx.bit(rd.bit()))
				break;
		rx.end();
		rx.release();
		return 0;
	}
	if (!alloc_zeroed(ch_, w + 4) || !alloc_zeroed(ref_, w + 4))
		return -1;
	ch = ch_.data();
	ref = ref_.data();
	ref[0] = ref[1] = w;            /* white above the first row */
	nref = 0;
	while (!rows_ || y < rows_) {
		if (compression == 2) {
			n = rd.decode_1d(w, ch);
			rd.pos = (rd.pos + 7) / 8 * 8;  /* rows byte aligned */
		} else {
			n = rd.decode_2d(w, ref, nref, ch);
		}
		if (n < 0)
			break;                  /* the end (EOFB) or an error */
		row = add_row(nullptr);
		if (!row)
			break;
		row_from_changes(row, w, ch, n);
		nref = row_changes(row, w, ref);
		y++;
		if (rd.pos >= rd.end)
			break;
	}
	return 0;
}

} // namespace dsp
