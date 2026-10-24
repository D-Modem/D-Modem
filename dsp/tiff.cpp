/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    tiff.cpp - TIFF class F files for fax pages: written with MH coding
 *             (one page per IFD), read with any of the fax codings or
 *             uncompressed
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utility>

#include "reinit.h"
#include "t4.h"

namespace dsp {


/*
 *    writing
 */

/* 16 and 32 bit values, little endian (the files we write are "II") */
static void put16(unsigned char *p, unsigned v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
}

/* a 32 bit one */
static void put32(unsigned char *p, unsigned long v)
{
	put16(p, v & 0xffff);
	put16(p + 2, (v >> 16) & 0xffff);
}

/* a new file at path, with the header; -1 on an error */
int TiffOut::open(const char *path)
{
	unsigned char h[8] = { 'I', 'I', 42, 0, 0, 0, 0, 0 };
	reinit(*this);
	f = fopen(path, "wb");
	if (!f)
		return -1;
	if (fwrite(h, 1, 8, static_cast<FILE*>(f)) != 8) {
		fclose(static_cast<FILE*>(f));
		f = NULL;
		return -1;
	}
	last_ifd_next = 4;           /* the header's first IFD offset */
	return 0;
}

enum { T_SHORT = 3, T_LONG = 4, T_RATIONAL = 5 };

/* an IFD entry at e, a single SHORT in the first half of the value
   field; where the next one goes */
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

/* a page appended: its rows in MH with byte aligned EOLs (one strip;
   RTC after them with rtc), then its IFD with the resolutions, linked from the IFD before it, and
   all of it in the file (a whole TIFF of the pages so far, before
   close()).  0, or -1 on a write error */
int TiffOut::write_page(const T4Page *p)
{
	FILE *f_ = static_cast<FILE*>(f);
	T4Bits b;
	unsigned char ifd[2 + 20 * 12 + 4 + 16], *e = ifd + 2, link[4];
	long data_at, ifd_at, res_at;
	size_t len;
	int n = 0;

	if (!f_)
		return -1;
	p->encode_mh(&b, 0, 1, rtc);
	len = (b.n + 7) / 8;
	fseek(f_, 0, SEEK_END);
	data_at = ftell(f_);
	if (len && fwrite(b.buf.data(), 1, len, f_) != len) {
		b.release();
		return -1;
	}
	b.release();
	if (ftell(f_) & 1)
		fputc(0, f_);                    /* IFDs on a word boundary */
	ifd_at = ftell(f_);
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
	put16(e + 8, pages);
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
	if (fwrite(ifd, 1, e - ifd, f_) != (size_t)(e - ifd))
		return -1;

	/* link it in */
	put32(link, ifd_at);
	fseek(f_, last_ifd_next, SEEK_SET);
	if (fwrite(link, 1, 4, f_) != 4)
		return -1;
	last_ifd_next = ifd_at + 2 + n * 12;
	pages++;
	return fflush(f_) ? -1 : 0;
}

/* close the file; -1 if that failed */
int TiffOut::close()
{
	int r = 0;
	if (f) {
		r = fclose(static_cast<FILE*>(f)) ? -1 : 0;
		f = NULL;
	}
	return r;
}


/*
 *    reading
 */

/* a 16 bit value at offset at, in the file's byte order; 0 beyond it */
unsigned TiffIn::get16(unsigned long at) const
{
	const unsigned char *p;
	if (at + 2 > len)
		return 0;
	p = data.data() + at;
	return big_endian ? (p[0] << 8) | p[1] : p[0] | (p[1] << 8);
}

/* a 32 bit value at offset at, in the file's byte order; 0 beyond it */
unsigned long TiffIn::get32(unsigned long at) const
{
	if (at + 4 > len)
		return 0;
	return big_endian ?
		((unsigned long)get16(at) << 16) | get16(at + 2) :
		get16(at) | ((unsigned long)get16(at + 2) << 16);
}

/* the whole file at path into memory, its header checked; 0, or -1 if
   it can't be read or isn't TIFF */
int TiffIn::open(const char *path)
{
	FILE *f = fopen(path, "rb");
	long len_;

	reinit(*this);
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	len_ = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len_ < 8 || !alloc_zeroed(data, (size_t)len_) ||
	    fread(data.data(), 1, len_, f) != (size_t)len_) {
		fclose(f);
		free_buffer(data);
		return -1;
	}
	fclose(f);
	len = len_;
	if (data[0] == 'M' && data[1] == 'M')
		big_endian = 1;
	else if (data[0] != 'I' || data[1] != 'I') {
		close();
		return -1;
	}
	if (get16(2) != 42) {
		close();
		return -1;
	}
	next_ifd = get32(4);
	return 0;
}

/* free the file's data */
void TiffIn::close()
{
	free_buffer(data);
}

/* a tag's value: the index-th one of an array of SHORTs or LONGs */
unsigned long TiffIn::tag_value(unsigned long e, unsigned long index) const
{
	unsigned type = get16(e + 2);
	unsigned long count = get32(e + 4), at = e + 8;
	int size = type == T_SHORT ? 2 : 4;

	if (index >= count)
		return 0;
	if (count * size > 4)
		at = get32(e + 8);
	at += index * size;
	return type == T_SHORT ? get16(at) : get32(at);
}

/* a RATIONAL tag's value (a SHORT or LONG one's too) */
double TiffIn::tag_rational(unsigned long e) const
{
	unsigned long at = get32(e + 8), d;
	if (get16(e + 2) != T_RATIONAL)
		return tag_value(e, 0);
	d = get32(at + 4);
	return d ? (double)get32(at) / d : 0;
}

/* a page of another width (or resolution) to the fax's */
void T4Page::fit(const T4Page *src, int row_step)
{
	int y, x, ss = T4_STRIDE(src->width), ds = T4_STRIDE(width);
	std::vector<unsigned char> row_;
	unsigned char *row;
	if (!alloc_zeroed(row_, ds))
		return;
	row = row_.data();
	for (y = 0; y < src->rows; y += row_step) {
		const unsigned char *s = src->bits.data() + (size_t)y * ss;
		memset(row, 0, ds);
		for (x = 0; x < width; x++) {
			int sx;
			if (src->width > width)
				sx = (int)((long)x * src->width / width);
			else
				sx = x - (width - src->width) / 2;
			if (sx >= 0 && sx < src->width &&
			    (s[sx >> 3] & (0x80 >> (sx & 7))))
				row[x >> 3] |= 0x80 >> (x & 7);
		}
		add_row(row);
	}
}

/* the next page (scaled to T4_WIDTH if narrower or wider, superfine to
   fine); 1, 0 when there are no more, -1 on an error */
int TiffIn::read_page(T4Page *p)
{
	unsigned long e, ifd = next_ifd, nstrips = 0, so_e = 0, sc_e = 0;
	unsigned long width = 0, rows = 0, rps = 0, bps = 1, comp = 1;
	unsigned long photo = 0, fill = 1, t4opts = 0, unit = 2, s, k;
	double yres = 98;
	unsigned n, i;
	T4Page src;
	int stride, fine, step = 1;

	/* none, or one read before: a chain that links back would give
	   its pages again and again */
	if (!ifd || page >= TIFF_MAX_PAGES)
		return 0;
	for (int i = 0; i < page; i++)
		if (ifds[i] == ifd)
			return 0;
	ifds[page] = ifd;
	if (ifd + 2 > len)
		return -1;
	n = get16(ifd);
	for (i = 0; i < n; i++) {
		e = ifd + 2 + i * 12;
		switch (get16(e)) {
		case 256: width = tag_value(e, 0); break;
		case 257: rows = tag_value(e, 0); break;
		case 258: bps = tag_value(e, 0); break;
		case 259: comp = tag_value(e, 0); break;
		case 262: photo = tag_value(e, 0); break;
		case 266: fill = tag_value(e, 0); break;
		case 273: so_e = e; nstrips = get32(e + 4); break;
		case 278: rps = tag_value(e, 0); break;
		case 279: sc_e = e; break;
		case 283: yres = tag_rational(e); break;
		case 292: t4opts = tag_value(e, 0); break;
		case 296: unit = tag_value(e, 0); break;
		}
	}
	next_ifd = get32(ifd + 2 + n * 12);
	page++;
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

	src.init((int)width, fine);
	stride = T4_STRIDE(width);
	for (s = 0; s < nstrips; s++) {
		unsigned long off = tag_value(so_e, s);
		unsigned long len_ = tag_value(sc_e, s);
		unsigned long want = rows - src.rows < rps ? rows - src.rows : rps;
		if (off >= len)
			break;
		if (off + len_ > len)
			len_ = len - off;
		if (comp == 1) {
			for (k = 0; k < want && (k + 1) * stride <= len_; k++)
				src.add_row(data.data() + off + k * stride);
		} else {
			src.decode(data.data() + off, len_, (int)comp,
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

	p->init(T4_WIDTH, fine);
	if (src.width == T4_WIDTH && step == 1) {
		*p = std::move(src);
		p->fine = fine;
		return 1;
	}
	p->fit(&src, step);
	src.release();
	return 1;
}

} // namespace dsp
