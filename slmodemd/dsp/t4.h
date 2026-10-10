/*
 *
 *    t4.h - fax page images: ITU-T T.4 (MH, MR) and T.6 (MMR) coding,
 *           and TIFF-F files
 *
 */

#ifndef __T4_H__
#define __T4_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define T4_WIDTH        1728    /* A4 at 204 pels per inch */

/* a page: rows of width bits, 1 = black, the first pel in the top bit
   of the first byte */
struct t4_page {
	int width, rows, cap;
	int fine;                       /* 196 lines per inch (else 98) */
	unsigned char *bits;
	int bad_rows;                   /* received with errors */
};

#define T4_STRIDE(w)    (((w) + 7) / 8)

int  t4_page_init(struct t4_page *p, int width, int fine);
void t4_page_free(struct t4_page *p);
/* a row added at the bottom; NULL: a copy of the last one (white for
   the first) */
unsigned char *t4_page_add_row(struct t4_page *p, const unsigned char *row);

/* a growing string of bits, in the order they go out */
struct t4_bits {
	unsigned char *buf;
	size_t n, cap;                  /* bits */
};

void t4_bits_put(struct t4_bits *b, unsigned code, int len);
void t4_bits_free(struct t4_bits *b);
static inline int t4_bits_get(const struct t4_bits *b, size_t i)
{
	return (b->buf[i >> 3] >> (7 - (i & 7))) & 1;
}

/* a page in MH (one dimensional) coding: an EOL before each row, rows
   padded with fill to at least min_bits, RTC at the end (rtc), EOLs
   ending on a byte boundary (align, as TIFF's T4Options bit 2) */
void t4_encode_mh(const struct t4_page *p, struct t4_bits *out,
		  int min_bits, int align, int rtc);

/* a page coming in bit by bit, as on the line (MH or MR with EOLs) */
struct t4_rx {
	int width, mr;
	struct t4_page *page;
	struct t4_bits line;            /* bits of the row being received */
	int zeros;                      /* zeros in a row (an EOL coming) */
	int eols;                       /* EOLs in a row with nothing in
					   between (6: RTC) */
	int started;                    /* the first EOL went by */
	int next_2d;                    /* MR: the tag bit after the EOL */
	int tag_pending;
	int done;
	unsigned char *ref;             /* the last good row */
};

void t4_rx_init(struct t4_rx *r, struct t4_page *page, int mr);
/* one bit; 1 once the page has ended (RTC) */
int  t4_rx_bit(struct t4_rx *r, int bit);
/* the end of the data without RTC (the carrier went): the last row */
void t4_rx_end(struct t4_rx *r);
void t4_rx_free(struct t4_rx *r);

/* a whole page from a buffer: compression as TIFF's (2 MH without EOLs,
   3 T.4 with EOLs, 1D or, t4opts & 1, 2D; 4 T.6), msb_first as TIFF's
   FillOrder 1; rows: how many to expect (0: until the end of data) */
int t4_decode(struct t4_page *p, const unsigned char *data, size_t len,
	      int compression, int t4opts, int msb_first, int rows);

/* TIFF-F: pages appended to a file, written at the end */
struct tiff_out {
	void *f;
	int pages;
	long last_ifd_next;             /* where to link the next page */
};

int  tiff_out_open(struct tiff_out *t, const char *path);
int  tiff_out_page(struct tiff_out *t, const struct t4_page *p);
int  tiff_out_close(struct tiff_out *t);

/* the pages of a TIFF file, read one at a time */
struct tiff_in {
	unsigned char *data;
	size_t len;
	int big_endian;
	unsigned long next_ifd;
	int page;
};

int  tiff_in_open(struct tiff_in *t, const char *path);
/* the next page (scaled to T4_WIDTH if narrower or wider); 0 when there
   are no more, -1 on an error */
int  tiff_in_page(struct tiff_in *t, struct t4_page *p);
void tiff_in_close(struct tiff_in *t);

#ifdef __cplusplus
}
#endif

#endif /* __T4_H__ */
