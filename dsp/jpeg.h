/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    jpeg.h - ITU-T T.81 baseline JPEG as T.4 Annex E uses it for colour
 *             and grey-scale fax (T.42's CIELAB), and the conversions
 *             between sRGB and T.42's 8-bit CIELAB
 *
 */

#ifndef __JPEG_H__
#define __JPEG_H__

#include <cstddef>
#include <vector>

#include "dm_types.h"

namespace dsp {

/* an image of 8-bit samples, comps (1 or 3) of them a pel, a row after
   another */
struct JpegImage {
	u32 width = 0, height = 0;
	s16 comps = 0;
	std::vector<unsigned char> pix;
	s16 dpi = 0;                    /* G3FAX APP1's resolution (0: none) */
};

/* how an image is coded */
struct JpegOptions {
	s16 quality = 75;               /* 1 to 100, scaling T.81 K.1/K.2 */
	s16 subsample = 1;              /* 3 components: a* and b* at half
					   the pels each way (T.4's 4:1:1) */
	s16 dpi = 200;                  /* in the G3FAX APP1 (0: none) */
	u16 restart = 0;                /* MCUs between RSTn (0: none) */
	s16 dnl = 0;                    /* lines given by DNL after the scan,
					   Y 0 in the frame header */
};

/* a baseline JPEG (SOI, the G3FAX APP1, DQT, DHT with T.81's K.3 to K.6
   tables, SOF0, one interleaved scan, EOI) */
void jpeg_encode(const JpegImage &img, const JpegOptions &opt, std::vector<unsigned char> *out);

/* a baseline (or extended 8-bit Huffman, SOF1) JPEG's first frame,
   its components brought to the full size; 0 when whole, 1 when the
   data ended early (the lines so far, the rest grey), -1 when it is
   none; at most max_rows lines */
s16 jpeg_decode(const unsigned char *data, size_t len, u32 max_rows, JpegImage *img);

/* T.42's default CIELAB (D50, L* 0 to 100, a* -85 to 85, b* -75 to 125
   in 8 bits, T.4 E.6.4) from sRGB, and back; n pels, 3 octets each */
void t42_rgb_to_lab(const unsigned char *rgb, unsigned char *lab, size_t n);
void t42_lab_to_rgb(const unsigned char *lab, unsigned char *rgb, size_t n);
/* the same for grey-scale: L* from an sRGB grey level, and back */
void t42_grey_to_l(const unsigned char *grey, unsigned char *l, size_t n);
void t42_l_to_grey(const unsigned char *l, unsigned char *grey, size_t n);

} // namespace dsp

#endif /* __JPEG_H__ */
