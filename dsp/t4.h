/*
 *
 *    t4.h - fax page images: ITU-T T.4 (MH, MR) and T.6 (MMR) coding,
 *           and TIFF-F files
 *
 */

#ifndef __T4_H__
#define __T4_H__

#include <stddef.h>

namespace dsp {

#define T4_WIDTH        1728    /* A4 at 204 pels per inch */

struct T4Bits;

/* a page: rows of width bits, 1 = black, the first pel in the top bit
   of the first byte */
struct T4Page {
	int width, rows, cap;
	int fine;                       /* 196 lines per inch (else 98) */
	unsigned char *bits;
	int bad_rows;                   /* received with errors */

	int init(int width_, int fine_);
	void release();
	/* a row added at the bottom; NULL: a copy of the last one (white for
	   the first) */
	unsigned char *add_row(const unsigned char *row);
	/* a page in MH (one dimensional) coding: an EOL before each row, rows
	   padded with fill to at least min_bits, RTC at the end (rtc), EOLs
	   ending on a byte boundary (align, as TIFF's T4Options bit 2) */
	void encode_mh(T4Bits *out, int min_bits, int align, int rtc) const;
	/* a whole page from a buffer: compression as TIFF's (2 MH without
	   EOLs, 3 T.4 with EOLs, 1D or, t4opts & 1, 2D; 4 T.6), msb_first as
	   TIFF's FillOrder 1; rows: how many to expect (0: until the end of
	   data) */
	int decode(const unsigned char *data, size_t len, int compression, int t4opts, int msb_first, int rows_);

	/* src to this page's width (scaled down, or centred), every
	   row_step-th row */
	void fit(const T4Page *src, int row_step);
};

#define T4_STRIDE(w)    (((w) + 7) / 8)

/* a growing string of bits, in the order they go out */
struct T4Bits {
	unsigned char *buf;
	size_t n, cap;                  /* bits */

	void put(unsigned code, int len);
	void release();
	void put_code(const char *bits);
	void put_run(int run, int black);
	void put_eol(int align);
	int line_empty() const;
};

/* bit i of b, counting in the order they go out */
static inline int t4_bits_get(const T4Bits *b, size_t i)
{
	return (b->buf[i >> 3] >> (7 - (i & 7))) & 1;
}

/* a page coming in bit by bit, as on the line (MH or MR with EOLs) */
struct T4Rx {
	int width, mr;
	T4Page *page;
	T4Bits line;            /* bits of the row being received */
	int zeros;                      /* zeros in a row (an EOL coming) */
	int eols;                       /* EOLs in a row with nothing in
					   between (6: RTC) */
	int started;                    /* the first EOL went by */
	int next_2d;                    /* MR: the tag bit after the EOL */
	int tag_pending;
	int done;
	unsigned char *ref;             /* the last good row */

	void init(T4Page *page_, int mr_);
	void release();
	void row(int two_d);
	/* one bit; 1 once the page has ended (RTC) */
	int bit(int bit);
	/* the end of the data without RTC (the carrier went): the last
	   row */
	void end();
};

/* TIFF-F: pages appended to a file, written at the end */
struct TiffOut {
	void *f;
	int pages;
	long last_ifd_next;             /* where to link the next page */

	int open(const char *path);
	int write_page(const T4Page *p);
	int close();
};


/* the pages TiffIn reads from one file at most */
#define TIFF_MAX_PAGES 1024

/* the pages of a TIFF file, read one at a time */
struct TiffIn {
	unsigned char *data;
	size_t len;
	int big_endian;
	unsigned long next_ifd;
	int page;
	/* the IFDs read, for a chain that links back; plain data, as it
	   sits in calloc()ed C structs */
	unsigned long ifds[TIFF_MAX_PAGES];

	unsigned get16(unsigned long at) const;
	unsigned long get32(unsigned long at) const;
	int open(const char *path);
	void close();
	unsigned long tag_value(unsigned long e, unsigned long index) const;
	double tag_rational(unsigned long e) const;
	/* the next page (scaled to T4_WIDTH if narrower or wider); 0 when
	   there are no more, -1 on an error */
	int read_page(T4Page *p);
};

} // namespace dsp

#endif /* __T4_H__ */
