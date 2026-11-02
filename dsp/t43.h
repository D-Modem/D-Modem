/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t43.h - ITU-T T.43 lossless colour and grey-scale fax pages: the
 *            image's bit-planes coded with T.82 (JBIG) behind a header
 *            (BCIH) that tells the image type, for T.4 Annex G
 *
 */

#ifndef __T43_H__
#define __T43_H__

#include <cstddef>
#include <vector>

#include "dm_types.h"

namespace dsp {

/* the image types of the G3FAX0 entry (T.43 7.2.2.2) */
#define T43_RGB         0       /* 1 bit a colour: red, green, blue */
#define T43_CMY         1       /* ... cyan, magenta, yellow */
#define T43_CMYK        2       /* ... and black */
#define T43_PALETTE     16      /* indices into a CIELAB table, 8 bits a
				   component */
#define T43_PALETTE12   17      /* ... 12 bits a component */
#define T43_GREY        32      /* L* */
#define T43_COLOUR      48      /* L*, a*, b* */

/* an image: its samples (comps of them a pel, a row after another),
   each of bits[] bits; and for a palette its CIELAB table */
struct T43Image {
	s16 type = T43_GREY;
	s16 bits[4] = {8, 0, 0, 0};     /* a component's planes (T.43's
					   "number of bits"; RGB, CMY,
					   CMYK, palette and grey: all in
					   the first) */
	u32 width = 0, height = 0;
	s16 dpi = 200;
	std::vector<u16> pix;
	std::vector<u16> palette;       /* L*, a*, b* of each entry */

	/* the samples of a pel */
	s16 comps() const;
};

/* how an image is coded */
struct T43Options {
	u32 l0 = 128;                   /* lines in a stripe (T.43's basic
					   value) */
	s16 plane_interleave = 0;       /* a plane after another (DIS bit
					   37), not a stripe of each */
};

/* the BCIE: the BCIH (G3FAX0, G3FAX3 for a palette, ECIH), the BIE of
   the bit-planes (Gray coded for grey and colour), the ending marker */
void t43_encode(const T43Image &img, const T43Options &opt, std::vector<unsigned char> *out);

/* a BCIE to an image of at most max_rows lines; 0 when whole, 1 when
   it ended early (the lines so far), -1 when it is none */
s16 t43_decode(const unsigned char *data, size_t len, u32 max_rows, T43Image *img);

/* an image in sRGB (3 octets a pel) or, grey, 1; *comps: which */
void t43_to_srgb(const T43Image &img, std::vector<unsigned char> *out, s16 *comps);

} // namespace dsp

#endif /* __T43_H__ */
