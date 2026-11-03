/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t44.h - ITU-T T.44 mixed raster content (MRC), mode 1, for T.4
 *            Annex H: a colour or grey page in stripes of up to three
 *            layers, a bi-level mask (the text's shapes, sharp) that
 *            picks between a background and a foreground image (each
 *            JPEG or T.43 at its own resolution, or one colour)
 *
 */

#ifndef __T44_H__
#define __T44_H__

#include <cstddef>
#include <vector>

#include "dm_types.h"

namespace dsp {

/* the mask's coders (T.44 Table 1) */
#define T44_MASK_MH     0x01
#define T44_MASK_MR     0x02
#define T44_MASK_MMR    0x04
#define T44_MASK_JBIG   0x08    /* T.85 */
/* the image layers' (Table 2: CIELAB) */
#define T44_IMAGE_JPEG  0x01    /* T.42 */
#define T44_IMAGE_T43   0x02

/* an image: comps (3 sRGB, 1 grey) 8 bit samples a pel, a row after
   another */
struct T44Image {
	u32 width = 0, height = 0;
	s16 comps = 0;
	s16 dpi = 200;                  /* the mask's resolution */
	std::vector<unsigned char> pix;
};

/* how a page is coded */
struct T44Options {
	s16 mask_coder = T44_MASK_JBIG; /* T44_MASK_JBIG or T44_MASK_MH */
	u32 stripe = 256;               /* lines in a stripe (T.4 H.5.3's
					   most without DIS bit 95) */
	s16 image_dpi = 100;            /* the background and foreground
					   images' with text over them (the
					   mask's or a half of it) */
	s16 quality = 75;               /* their JPEG's */
};

/* the page whose stripes have a mask: text or lines over the rest */
bool t44_worth(const T44Image &img);

/* the page (at img.dpi, its width the mask's) as MRC mode 1: SOP, its
   stripes (SOSt, the mask, background, foreground), EOP */
void t44_encode(const T44Image &img, const T44Options &opt, std::vector<unsigned char> *out);

/* an MRC page (mode 1, any coders of Tables 1 and 2 but JBIG2 and
   T.45) to an image of at most max_rows lines: grey if all of it is,
   else sRGB.  0 when whole, 1 when it ended early (the stripes so far),
   -1 when it is none */
s16 t44_decode(const unsigned char *data, size_t len, u32 max_rows, T44Image *img);

} // namespace dsp

#endif /* __T44_H__ */
