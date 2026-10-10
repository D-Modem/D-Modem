/*
 *
 *    tiff.cpp - TIFF class F files for fax pages: written with MH coding
 *             (one page per IFD), read with any of the fax codings or
 *             uncompressed
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t4.h"


/*
 *    writing
 */

static void put16(unsigned char *p, unsigned v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
}

static void put32(unsigned char *p, unsigned long v)
{
	put16(p, v & 0xffff);
	put16(p + 2, (v >> 16) & 0xffff);
}

int tiff_out_open(struct tiff_out *t, const char *path)
{
	unsigned char h[8] = { 'I', 'I', 42, 0, 0, 0, 0, 0 };
	memset(t, 0, sizeof(*t));
	t->f = fopen(path, "wb");
	if (!t->f)
		return -1;
	if (fwrite(h, 1, 8, static_cast<FILE*>(t->f)) != 8) {
		fclose(static_cast<FILE*>(t->f));
		t->f = NULL;
		return -1;
	}
	t->last_ifd_next = 4;           /* the header's first IFD offset */
	return 0;
}

enum { T_SHORT = 3, T_LONG = 4, T_RATIONAL = 5 };

static unsigned char *entry(unsigned char *e, unsigned tag, unsigned type,
			    unsigned long count, unsigned long value)
{
	put16(e, tag);
	put16(e + 2, type);
	put32(e + 4, count);
	if (type == T_SHORT && count == 1)
		put32(e + 8, value & 0xffff);
	else
		put32(e + 8, value);
	return e + 12;
}

int tiff_out_page(struct tiff_out *t, const struct t4_page *p)
{
	FILE *f = static_cast<FILE*>(t->f);
	struct t4_bits b = { 0 };
	unsigned char ifd[2 + 20 * 12 + 4 + 16], *e = ifd + 2, link[4];
	long data_at, ifd_at, res_at;
	size_t len;
	int n = 0;

	if (!f)
		return -1;
	t4_encode_mh(p, &b, 0, 1, 0);
	len = (b.n + 7) / 8;
	fseek(f, 0, SEEK_END);
	data_at = ftell(f);
	if (len && fwrite(b.buf, 1, len, f) != len) {
		t4_bits_free(&b);
		return -1;
	}
	t4_bits_free(&b);
	if (ftell(f) & 1)
		fputc(0, f);                    /* IFDs on a word boundary */
	ifd_at = ftell(f);
	res_at = ifd_at + 2 + 18 * 12 + 4;

	e = entry(e, 254, T_LONG, 1, 2), n++;           /* a page */
	e = entry(e, 256, T_LONG, 1, p->width), n++;
	e = entry(e, 257, T_LONG, 1, p->rows), n++;
	e = entry(e, 258, T_SHORT, 1, 1), n++;          /* bits per sample */
	e = entry(e, 259, T_SHORT, 1, 3), n++;          /* T.4 */
	e = entry(e, 262, T_SHORT, 1, 0), n++;          /* white is zero */
	e = entry(e, 266, T_SHORT, 1, 1), n++;          /* fill order */
	e = entry(e, 273, T_LONG, 1, data_at), n++;
	e = entry(e, 274, T_SHORT, 1, 1), n++;          /* orientation */
	e = entry(e, 277, T_SHORT, 1, 1), n++;          /* samples */
	e = entry(e, 278, T_LONG, 1, p->rows), n++;     /* rows per strip */
	e = entry(e, 279, T_LONG, 1, len), n++;
	e = entry(e, 282, T_RATIONAL, 1, res_at), n++;
	e = entry(e, 283, T_RATIONAL, 1, res_at + 8), n++;
	e = entry(e, 284, T_SHORT, 1, 1), n++;          /* planar */
	e = entry(e, 292, T_LONG, 1, 4), n++;           /* 1D, fill bits */
	e = entry(e, 296, T_SHORT, 1, 2), n++;          /* inches */
	put16(e, 297);                                  /* page number */
	put16(e + 2, T_SHORT);
	put32(e + 4, 2);
	put16(e + 8, t->pages);
	put16(e + 10, 0);
	e += 12, n++;
	put16(ifd, n);
	put32(e, 0);                                    /* the last page */
	e += 4;
	put32(e, 204);
	put32(e + 4, 1);
	put32(e + 8, p->fine ? 196 : 98);
	put32(e + 12, 1);
	e += 16;
	if (fwrite(ifd, 1, e - ifd, f) != (size_t)(e - ifd))
		return -1;

	/* link it in */
	put32(link, ifd_at);
	fseek(f, t->last_ifd_next, SEEK_SET);
	if (fwrite(link, 1, 4, f) != 4)
		return -1;
	t->last_ifd_next = ifd_at + 2 + n * 12;
	t->pages++;
	return 0;
}

int tiff_out_close(struct tiff_out *t)
{
	int r = 0;
	if (t->f) {
		r = fclose(static_cast<FILE*>(t->f)) ? -1 : 0;
		t->f = NULL;
	}
	return r;
}


/*
 *    reading
 */

static unsigned get16(const struct tiff_in *t, unsigned long at)
{
	const unsigned char *p = t->data + at;
	if (at + 2 > t->len)
		return 0;
	return t->big_endian ? (p[0] << 8) | p[1] : p[0] | (p[1] << 8);
}

static unsigned long get32(const struct tiff_in *t, unsigned long at)
{
	if (at + 4 > t->len)
		return 0;
	return t->big_endian ?
		((unsigned long)get16(t, at) << 16) | get16(t, at + 2) :
		get16(t, at) | ((unsigned long)get16(t, at + 2) << 16);
}

int tiff_in_open(struct tiff_in *t, const char *path)
{
	FILE *f = fopen(path, "rb");
	long len;

	memset(t, 0, sizeof(*t));
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len < 8 || !(t->data = static_cast<unsigned char*>(malloc(len))) ||
	    fread(t->data, 1, len, f) != (size_t)len) {
		fclose(f);
		free(t->data);
		t->data = NULL;
		return -1;
	}
	fclose(f);
	t->len = len;
	if (t->data[0] == 'M' && t->data[1] == 'M')
		t->big_endian = 1;
	else if (t->data[0] != 'I' || t->data[1] != 'I') {
		tiff_in_close(t);
		return -1;
	}
	if (get16(t, 2) != 42) {
		tiff_in_close(t);
		return -1;
	}
	t->next_ifd = get32(t, 4);
	return 0;
}

void tiff_in_close(struct tiff_in *t)
{
	free(t->data);
	t->data = NULL;
}

/* a tag's value: the index-th one of an array of SHORTs or LONGs */
static unsigned long tag_value(const struct tiff_in *t, unsigned long e,
			       unsigned long index)
{
	unsigned type = get16(t, e + 2);
	unsigned long count = get32(t, e + 4), at = e + 8;
	int size = type == T_SHORT ? 2 : 4;

	if (index >= count)
		return 0;
	if (count * size > 4)
		at = get32(t, e + 8);
	at += index * size;
	return type == T_SHORT ? get16(t, at) : get32(t, at);
}

static double tag_rational(const struct tiff_in *t, unsigned long e)
{
	unsigned long at = get32(t, e + 8), d;
	if (get16(t, e + 2) != T_RATIONAL)
		return tag_value(t, e, 0);
	d = get32(t, at + 4);
	return d ? (double)get32(t, at) / d : 0;
}

/* a page of another width (or resolution) to the fax's */
static void fit_page(struct t4_page *dst, const struct t4_page *src,
		     int row_step)
{
	int y, x, ss = T4_STRIDE(src->width), ds = T4_STRIDE(dst->width);
	unsigned char *row = static_cast<unsigned char*>(calloc(1, ds));
	if (!row)
		return;
	for (y = 0; y < src->rows; y += row_step) {
		const unsigned char *s = src->bits + (size_t)y * ss;
		memset(row, 0, ds);
		for (x = 0; x < dst->width; x++) {
			int sx;
			if (src->width > dst->width)
				sx = (int)((long)x * src->width / dst->width);
			else
				sx = x - (dst->width - src->width) / 2;
			if (sx >= 0 && sx < src->width &&
			    (s[sx >> 3] & (0x80 >> (sx & 7))))
				row[x >> 3] |= 0x80 >> (x & 7);
		}
		t4_page_add_row(dst, row);
	}
	free(row);
}

int tiff_in_page(struct tiff_in *t, struct t4_page *p)
{
	unsigned long e, ifd = t->next_ifd, nstrips = 0, so_e = 0, sc_e = 0;
	unsigned long width = 0, rows = 0, rps = 0, bps = 1, comp = 1;
	unsigned long photo = 0, fill = 1, t4opts = 0, unit = 2, s, k;
	double yres = 98;
	unsigned n, i;
	struct t4_page src;
	int stride, fine, step = 1;

	if (!ifd)
		return 0;
	if (ifd + 2 > t->len)
		return -1;
	n = get16(t, ifd);
	for (i = 0; i < n; i++) {
		e = ifd + 2 + i * 12;
		switch (get16(t, e)) {
		case 256: width = tag_value(t, e, 0); break;
		case 257: rows = tag_value(t, e, 0); break;
		case 258: bps = tag_value(t, e, 0); break;
		case 259: comp = tag_value(t, e, 0); break;
		case 262: photo = tag_value(t, e, 0); break;
		case 266: fill = tag_value(t, e, 0); break;
		case 273: so_e = e; nstrips = get32(t, e + 4); break;
		case 278: rps = tag_value(t, e, 0); break;
		case 279: sc_e = e; break;
		case 283: yres = tag_rational(t, e); break;
		case 292: t4opts = tag_value(t, e, 0); break;
		case 296: unit = tag_value(t, e, 0); break;
		}
	}
	t->next_ifd = get32(t, ifd + 2 + n * 12);
	t->page++;
	if (!width || width > 10000 || bps != 1 || !so_e || !sc_e ||
	    (comp != 1 && comp != 2 && comp != 3 && comp != 4))
		return -1;
	if (!rps || rps > rows)
		rps = rows;
	if (unit == 3)
		yres *= 2.54;
	fine = yres > 150;
	if (yres > 300)
		step = 2;                       /* superfine: every other row */

	t4_page_init(&src, (int)width, fine);
	stride = T4_STRIDE(width);
	for (s = 0; s < nstrips; s++) {
		unsigned long off = tag_value(t, so_e, s);
		unsigned long len = tag_value(t, sc_e, s);
		unsigned long want = rows - src.rows < rps ? rows - src.rows : rps;
		if (off >= t->len)
			break;
		if (off + len > t->len)
			len = t->len - off;
		if (comp == 1) {
			for (k = 0; k < want && (k + 1) * stride <= len; k++)
				t4_page_add_row(&src, t->data + off + k * stride);
		} else {
			t4_decode(&src, t->data + off, len, (int)comp,
				  (int)t4opts, fill != 2, (int)want);
		}
	}
	if (fill == 2 && comp == 1) {
		/* the pels of each byte the other way round */
		size_t j;
		for (j = 0; j < (size_t)src.rows * stride; j++) {
			unsigned char c = src.bits[j], r = 0;
			for (k = 0; k < 8; k++)
				r |= ((c >> k) & 1) << (7 - k);
			src.bits[j] = r;
		}
	}
	if (photo == 1) {                       /* black is zero */
		size_t j;
		for (j = 0; j < (size_t)src.rows * stride; j++)
			src.bits[j] ^= 0xff;
	}

	t4_page_init(p, T4_WIDTH, fine);
	if (src.width == T4_WIDTH && step == 1) {
		*p = src;
		p->fine = fine;
		return 1;
	}
	fit_page(p, &src, step);
	t4_page_free(&src);
	return 1;
}
