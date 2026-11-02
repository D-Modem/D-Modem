/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t4.h - fax page images: ITU-T T.4 (MH, MR) and T.6 (MMR) coding,
 *           and TIFF-F files
 *
 */

#ifndef __T4_H__
#define __T4_H__

#include <cstddef>
#include <vector>

#include "dm_types.h"

namespace dsp {

#define T4_WIDTH        1728    /* A4 at 204 pels per inch */

struct T4Bits;

/* a page: rows of width bits, 1 = black, the first pel in the top bit
   of the first byte */
struct T4Page {
	s16 width = 0;
	s32 rows = 0, cap = 0;
	s16 fine = 0;                   /* 196 lines per inch (else 98) */
	std::vector<unsigned char> bits;
	s32 bad_rows = 0;               /* received with errors */
	/* a colour (3: sRGB) or grey-scale (1) page too, 0 if not: its 8 bit
	   samples, width by rows, a row after another (bits then has it
	   dithered, for what takes only black and white) */
	s16 comps = 0;
	std::vector<unsigned char> pix;

	s16 init(s16 width_, s16 fine_);
	void release();
	/* a row added at the bottom; NULL: a copy of the last one (white for
	   the first) */
	unsigned char *add_row(const unsigned char *row);
	/* a page in MH (one dimensional) coding: an EOL before each row, rows
	   padded with fill to at least min_bits, RTC at the end (rtc), EOLs
	   ending on a byte boundary (align, as TIFF's T4Options bit 2) */
	void encode_mh(T4Bits *out, s16 min_bits, s16 align, s16 rtc) const;
	/* a whole page from a buffer: compression as TIFF's (2 MH without
	   EOLs, 3 T.4 with EOLs, 1D or, t4opts & 1, 2D; 4 T.6), msb_first as
	   TIFF's FillOrder 1; rows: how many to expect (0: until the end of
	   data) */
	s16 decode(const unsigned char *data, size_t len, s16 compression, u32 t4opts, s16 msb_first, s32 rows_);

	/* src to this page's width (scaled down, or centred), every
	   row_step-th row */
	void fit(const T4Page *src, s16 row_step);
	/* a page of comps samples a pel (w by h of them in pix_) to this
	   page's width, as fit() does, and dithered into its rows */
	void set_pix(const unsigned char *pix_, s16 comps_, u32 w, u32 h);
	/* bits from pix: each pel's lightness against an 8x8 ordered
	   dither */
	void dither();
};

#define T4_STRIDE(w)    (((w) + 7) / 8)

/* a growing string of bits, in the order they go out */
struct T4Bits {
	std::vector<unsigned char> buf;
	size_t n = 0, cap = 0;          /* bits */

	void put(u16 code, s16 len);
	void release();
	void put_code(const char *bits);
	void put_run(s16 run, s16 black);
	void put_eol(s16 align);
	s16 line_empty() const;
};

/* bit i of b, counting in the order they go out */
static inline s16 t4_bits_get(const T4Bits *b, size_t i)
{
	return (b->buf[i >> 3] >> (7 - (i & 7))) & 1;
}

/* a page coming in bit by bit, as on the line (MH or MR with EOLs) */
struct T4Rx {
	s16 width, mr;
	T4Page *page;
	T4Bits line;            /* bits of the row being received */
	s32 zeros;                      /* zeros in a row (an EOL coming) */
	s16 eols;                       /* EOLs in a row with nothing in
					   between (6: RTC) */
	s16 started;                    /* the first EOL went by */
	s16 next_2d;                    /* MR: the tag bit after the EOL */
	s16 tag_pending;
	s16 done;
	std::vector<unsigned char> ref; /* the last good row */

	void init(T4Page *page_, s16 mr_);
	void release();
	void row(s16 two_d);
	/* one bit; 1 once the page has ended (RTC) */
	s16 bit(s16 bit);
	/* the end of the data without RTC (the carrier went): the last
	   row */
	void end();
};

/* TIFF-F: pages appended to a file, written at the end */
struct TiffOut {
	void *f;
	s16 pages;
	long last_ifd_next;             /* where to link the next page */
	s16 rtc;                        /* RTC after each page's rows, for
					   readers that look for it (efax);
					   set after open() */

	s16 open(const char *path);
	/* a page appended (one with comps in colour or grey, uncompressed) */
	s16 write_page(const T4Page *p);
	s16 write_pix(const T4Page *p);
	s16 close();
};


/* the pages TiffIn reads from one file at most */
#define TIFF_MAX_PAGES 1024

/* the pages of a TIFF file, read one at a time */
struct TiffIn {
	std::vector<unsigned char> data;
	size_t len;
	s16 big_endian;
	u32 next_ifd;
	s16 page;
	/* the IFDs read, for a chain that links back; plain data, as it
	   sits in calloc()ed C structs */
	u32 ifds[TIFF_MAX_PAGES];

	u16 get16(u64 at) const;
	u32 get32(u64 at) const;
	s16 open(const char *path);
	void close();
	u32 tag_value(u64 e, u64 index) const;
	double tag_rational(u64 e) const;
	/* the next page (scaled to T4_WIDTH if narrower or wider); 0 when
	   there are no more, -1 on an error.  A page of 8 bit grey or RGB
	   samples (uncompressed) comes in colour (T4Page's comps) at 200
	   pels an inch */
	s16 read_page(T4Page *p);
	s16 read_pix(T4Page *p, u64 so_e, u64 sc_e, u64 nstrips, u64 width, u64 rows, u64 rps, u64 comp,
		     u64 spp, u64 photo, u64 planar, double xres, double yres);
};

} // namespace dsp

#endif /* __T4_H__ */
