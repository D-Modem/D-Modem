/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    jbig.cpp - ITU-T T.82 (JBIG1) bi-level image coding as T.85
 *               profiles it for fax
 *
 * A T.85 page is one bi-image entity (BIE): a 20 byte header (BIH) and
 * the image in stripes of L0 lines.  Each stripe is coded with the QM
 * arithmetic coder, the pixel's context taken from a template of ten
 * pixels coded before it (three lines, or with LRLTWO two), one of
 * them (the AT pixel) movable.  With TPBON a line the same as the one
 * above is sent as one "typical" pseudo-pixel.  The coded bytes have
 * each 0xff escaped as ff 00 and end with ff 02 (or ff 03, which also
 * resets the probabilities).  Marker segments between stripes move the
 * AT pixel (ATMOVE), shorten the page (NEWLEN), carry comments, or end
 * it (ABORT).
 *
 * T.43 codes an image of more bit-planes in one BIE: each plane is
 * coded as above, with its own probabilities, and their stripes come a
 * plane after another, or a stripe of each plane in turn.
 *
 * Progressive coding (D differential layers) halves the image D times
 * with Table 17's resolution reduction, codes the lowest layer as
 * above, then each differential layer from the one below it: a
 * template of high and low resolution pixels and the pixel's phase,
 * with pixels skipped that typical prediction (TPDON) or deterministic
 * prediction (DPON, Tables 19 to 22) gives.
 *
 */

#include <string.h>

#include <algorithm>

#include "jbig.h"

namespace dsp {

/* T.82 Table 17 (the Recommendation's order: index 0 first, in the top
   bit) */
const unsigned char jbig_rr_table[512] = {
	0x11, 0x73, 0xff, 0xff, 0x33, 0xff, 0xff, 0xff, 0x01, 0x77, 0xff, 0xff,
	0x37, 0xff, 0xff, 0xff, 0x37, 0xff, 0xff, 0xff, 0x7d, 0xff, 0xff, 0xff,
	0x37, 0xff, 0xff, 0xff, 0xff, 0x7d, 0xff, 0xff, 0x01, 0x37, 0xfd, 0xff,
	0x3f, 0xff, 0xff, 0xff, 0x37, 0x7f, 0xff, 0x7f, 0x7f, 0x7f, 0x7f, 0xff,
	0x35, 0xff, 0xf7, 0xff, 0xdf, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0x01, 0x23, 0x05, 0x3b, 0x11, 0x23, 0x71, 0xff,
	0x01, 0x75, 0x3b, 0x7f, 0x00, 0x53, 0xfe, 0xff, 0x01, 0x41, 0x7f, 0xff,
	0x09, 0xb7, 0xff, 0xff, 0x00, 0x53, 0x7f, 0xfb, 0x93, 0x79, 0xff, 0xff,
	0x01, 0x00, 0x73, 0xff, 0x31, 0x13, 0x75, 0xff, 0x00, 0x41, 0xb7, 0xee,
	0x01, 0x21, 0xfc, 0xff, 0x00, 0x93, 0x75, 0xff, 0x11, 0x6b, 0xf5, 0xff,
	0xe9, 0xf7, 0xff, 0xfb, 0xb7, 0xff, 0xfb, 0xff, 0x01, 0x23, 0x01, 0x3f,
	0x11, 0x01, 0x77, 0xff, 0x01, 0x75, 0x6b, 0x7f, 0x00, 0x53, 0xfe, 0xff,
	0x01, 0x61, 0x7f, 0xff, 0x29, 0x37, 0xff, 0xff, 0x00, 0x73, 0x3f, 0x7b,
	0x92, 0x7d, 0xff, 0xff, 0x01, 0x00, 0x7b, 0xfe, 0x2f, 0x1b, 0x7f, 0xff,
	0x00, 0x41, 0x37, 0xfe, 0x09, 0x37, 0x7e, 0x7f, 0x00, 0xd2, 0x7f, 0xff,
	0x1b, 0x6f, 0xff, 0xff, 0x00, 0x75, 0x7f, 0x77, 0x27, 0x7f, 0x7b, 0x7f,
	0x01, 0x03, 0x01, 0x09, 0x11, 0x01, 0x41, 0x93, 0x01, 0x75, 0x21, 0x55,
	0x00, 0x51, 0x80, 0xf7, 0x01, 0x41, 0x6b, 0x13, 0x01, 0x00, 0xfb, 0xff,
	0x00, 0x51, 0x01, 0x73, 0x00, 0x41, 0xb7, 0xff, 0x01, 0x00, 0x61, 0x81,
	0x27, 0x09, 0x1e, 0xbf, 0x00, 0x40, 0x01, 0x56, 0x08, 0x00, 0x10, 0x7f,
	0x00, 0x80, 0x21, 0x77, 0x03, 0x01, 0x3f, 0xff, 0x68, 0xd0, 0xf3, 0xb3,
	0x00, 0xd3, 0xfb, 0xff, 0x01, 0x03, 0x37, 0xff, 0x33, 0x37, 0x7f, 0xff,
	0x01, 0x77, 0x7f, 0xff, 0x11, 0x7b, 0xff, 0xff, 0x01, 0xf7, 0x7f, 0xff,
	0x3f, 0xff, 0xfd, 0xff, 0x12, 0xf7, 0xff, 0xff, 0xff, 0xfd, 0xff, 0x7f,
	0x01, 0x12, 0x7d, 0xff, 0x3f, 0x7f, 0xff, 0xff, 0x00, 0x62, 0xff, 0x7f,
	0x3f, 0x3f, 0x7f, 0xff, 0x10, 0xff, 0xf7, 0xff, 0x7f, 0xff, 0x7f, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x23, 0x01, 0x1b,
	0x11, 0x23, 0x77, 0xff, 0x01, 0x75, 0x2b, 0x77, 0x00, 0x41, 0xbe, 0xff,
	0x01, 0xc1, 0x5b, 0x7f, 0x09, 0x33, 0x7d, 0xff, 0x00, 0x51, 0x37, 0xfb,
	0xa9, 0xb1, 0xff, 0xff, 0x01, 0x00, 0x71, 0xb7, 0x21, 0x03, 0x75, 0xff,
	0x00, 0x40, 0x17, 0x6f, 0x00, 0x01, 0x7d, 0xff, 0x00, 0xc1, 0x75, 0xff,
	0x01, 0xab, 0x51, 0xff, 0xe8, 0xd3, 0xff, 0xfb, 0xbb, 0xff, 0xfb, 0xff,
	0x01, 0x23, 0x01, 0x1b, 0x31, 0x01, 0x53, 0x7f, 0x01, 0x75, 0x29, 0x7f,
	0x00, 0x51, 0xb6, 0xff, 0x01, 0xe0, 0x7b, 0xff, 0x0a, 0x3b, 0x7f, 0xff,
	0x00, 0x71, 0x7f, 0xfb, 0x88, 0x75, 0xff, 0x7f, 0x01, 0x00, 0x61, 0xf6,
	0x3f, 0x09, 0x7f, 0xff, 0x00, 0x40, 0x17, 0x7f, 0x08, 0x13, 0x7e, 0x7f,
	0x00, 0x80, 0x77, 0xff, 0x2b, 0x2f, 0x7f, 0x7f, 0x00, 0x71, 0x7f, 0x77,
	0x2b, 0x7f, 0x3b, 0x7f, 0x01, 0x03, 0x01, 0x09, 0x11, 0x01, 0x41, 0x01,
	0x01, 0x75, 0x21, 0x55, 0x00, 0x51, 0x80, 0x53, 0x01, 0x41, 0x49, 0x01,
	0x09, 0x00, 0x01, 0x13, 0x00, 0x51, 0x00, 0x53, 0x80, 0x41, 0x13, 0x7f,
	0x01, 0x00, 0x61, 0x80, 0x21, 0x01, 0x01, 0x13, 0x00, 0x40, 0x00, 0x40,
	0x00, 0x00, 0x00, 0x13, 0x00, 0x80, 0x00, 0x13, 0x01, 0x01, 0x51, 0x7f,
	0x00, 0x50, 0x00, 0x73, 0x01, 0x54, 0x31, 0x77,
};

/* T.82 Tables 19, 20, 21 and 22 in turn, as a DPTABLE field */
const unsigned char jbig_dp_table[1728] = {
	0x2a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x2a, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0x2a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0x0a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x2a, 0xaa, 0xaa, 0xaa,
	0x0a, 0xaa, 0xaa, 0xaa, 0x22, 0x2a, 0xaa, 0xaa, 0x22, 0xaa, 0xaa, 0xaa,
	0x0a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xa9, 0x22, 0x0a, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xa0, 0x00,
	0x2a, 0xaa, 0xaa, 0xaa, 0x0a, 0xaa, 0xa5, 0x55, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0x95, 0x55, 0x2a, 0xaa, 0xa5, 0x55, 0xaa, 0xaa, 0xa5, 0xa9,
	0x2a, 0xaa, 0xaa, 0xaa, 0x2a, 0xaa, 0xaa, 0xaa, 0x0a, 0xaa, 0xaa, 0xa0,
	0x8a, 0xaa, 0xaa, 0xaa, 0x2a, 0xaa, 0xa5, 0x55, 0xaa, 0xaa, 0xaa, 0x92,
	0x5a, 0xaa, 0xaa, 0xa6, 0xaa, 0x0a, 0xaa, 0xa6, 0x8a, 0xaa, 0xaa, 0xaa,
	0x0a, 0x2a, 0xaa, 0xaa, 0x80, 0x2a, 0xaa, 0xaa, 0x00, 0x0a, 0xaa, 0xa9,
	0x8a, 0xaa, 0xaa, 0xaa, 0x5a, 0xaa, 0xaa, 0xa9, 0xaa, 0xaa, 0xaa, 0xa9,
	0xaa, 0x5a, 0xaa, 0xa9, 0x82, 0x0a, 0xaa, 0xaa, 0xa0, 0x0a, 0xaa, 0xaa,
	0x88, 0x82, 0xaa, 0xaa, 0x8a, 0x02, 0xaa, 0xaa, 0xa0, 0x0a, 0xaa, 0xaa,
	0x0a, 0x0a, 0xaa, 0xa9, 0x99, 0xa2, 0xaa, 0xaa, 0xaa, 0x02, 0xaa, 0xaa,
	0xaa, 0xaa, 0x6a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x2a, 0xaa, 0x6a, 0xaa,
	0x2a, 0xaa, 0x5a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x2a, 0xaa, 0x6a, 0xaa,
	0x2a, 0xaa, 0x5a, 0xaa, 0xaa, 0x5a, 0xaa, 0xaa, 0x08, 0xaa, 0x55, 0x55,
	0x08, 0x2a, 0x55, 0x55, 0x0a, 0xaa, 0x96, 0xaa, 0x4a, 0xaa, 0xa5, 0x6a,
	0x2a, 0xaa, 0x5a, 0xaa, 0x0a, 0xaa, 0x9a, 0xaa, 0xaa, 0xaa, 0xa8, 0xa8,
	0xaa, 0x0a, 0xa5, 0xaa, 0x8a, 0xaa, 0x9a, 0xaa, 0x82, 0x0a, 0xaa, 0xaa,
	0x80, 0x2a, 0xaa, 0xa8, 0xa0, 0x0a, 0xaa, 0xa6, 0x8a, 0x2a, 0xa9, 0x55,
	0xa2, 0x2a, 0xa5, 0x9a, 0xa0, 0x0a, 0xa6, 0x9a, 0xa0, 0xaa, 0xaa, 0xaa,
	0x82, 0x0a, 0xaa, 0xa0, 0xa0, 0x0a, 0xaa, 0xa6, 0xa8, 0x8a, 0xaa, 0xaa,
	0x88, 0xa2, 0xaa, 0xa6, 0xa8, 0x8a, 0xaa, 0xa0, 0x00, 0x8a, 0xaa, 0xa6,
	0xaa, 0xa2, 0xaa, 0xa9, 0xa0, 0xa8, 0xa9, 0xa9, 0x2a, 0xaa, 0xa5, 0x5a,
	0x2a, 0xaa, 0x5a, 0xaa, 0xa9, 0x9a, 0xaa, 0x00, 0xa6, 0x9a, 0xa8, 0x80,
	0x00, 0x00, 0xaa, 0xaa, 0x2a, 0xaa, 0xa0, 0x00, 0xa0, 0xaa, 0xaa, 0xaa,
	0xa0, 0x96, 0xaa, 0xaa, 0x8a, 0xaa, 0x9a, 0x5a, 0x8a, 0xaa, 0xa6, 0x6a,
	0xa0, 0x0a, 0xa5, 0x9a, 0x2a, 0xaa, 0xa9, 0xaa, 0x8a, 0xaa, 0xaa, 0x8a,
	0x02, 0xaa, 0x95, 0x6a, 0x20, 0x0a, 0xaa, 0xa0, 0xa0, 0xa6, 0xaa, 0xaa,
	0xa2, 0x2a, 0xa5, 0x5a, 0xaa, 0x8a, 0xaa, 0xa0, 0xaa, 0x8a, 0xaa, 0xa6,
	0xaa, 0xa2, 0xaa, 0xa9, 0xa0, 0x0a, 0xaa, 0xaa, 0x08, 0x6a, 0xaa, 0xa8,
	0xa2, 0xa2, 0xaa, 0xaa, 0xa0, 0xa0, 0xaa, 0xaa, 0xa8, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa2, 0xaa, 0xa9, 0xaa, 0xaa, 0xaa, 0xa9, 0xa8, 0xa8, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0xa0, 0xa2, 0xaa, 0xa9, 0x88, 0xa8, 0xaa, 0xaa,
	0xa0, 0xa8, 0xaa, 0xaa, 0xaa, 0xaa, 0x55, 0x55, 0xaa, 0xaa, 0x55, 0x55,
	0x2a, 0xaa, 0x95, 0x55, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x55, 0x55,
	0x2a, 0xaa, 0xaa, 0xaa, 0x2a, 0xaa, 0x9a, 0xaa, 0xaa, 0xaa, 0xaa, 0x82,
	0x0a, 0xaa, 0x55, 0x55, 0xaa, 0xa2, 0xaa, 0x59, 0x6a, 0xaa, 0xaa, 0xa6,
	0xa0, 0xa2, 0xaa, 0x55, 0x2a, 0xaa, 0x9a, 0xaa, 0xaa, 0xaa, 0xa8, 0x62,
	0xaa, 0x2a, 0xa4, 0x6a, 0xa0, 0x0a, 0xaa, 0xa9, 0x8a, 0xaa, 0xa5, 0x95,
	0x52, 0x2a, 0xa9, 0x55, 0xa8, 0x82, 0xaa, 0xaa, 0x08, 0xaa, 0xaa, 0xa6,
	0xa2, 0x0a, 0xaa, 0xa5, 0xa0, 0x0a, 0xaa, 0xa6, 0xaa, 0x8a, 0xaa, 0xa6,
	0xaa, 0xa2, 0xaa, 0xa6, 0xa0, 0x8a, 0xa9, 0x65, 0xaa, 0xa6, 0xaa, 0xa9,
	0x9a, 0x92, 0xaa, 0xa9, 0xaa, 0xaa, 0xaa, 0x99, 0xa6, 0x5a, 0xaa, 0xaa,
	0xa5, 0x5a, 0xaa, 0x2a, 0xaa, 0xa0, 0xaa, 0xa9, 0xa0, 0x02, 0xaa, 0xa9,
	0x00, 0x00, 0x55, 0x55, 0x2a, 0xaa, 0x9a, 0xaa, 0x8a, 0xaa, 0xa6, 0x55,
	0xaa, 0x2a, 0xa6, 0x5a, 0x0a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x96, 0x6a,
	0x82, 0x2a, 0xa5, 0x55, 0xaa, 0x0a, 0xa9, 0x9a, 0x8a, 0x0a, 0xa5, 0x55,
	0xa2, 0x0a, 0xaa, 0x59, 0xa0, 0x0a, 0xaa, 0x9a, 0xaa, 0x0a, 0xaa, 0xa5,
	0x82, 0x2a, 0xa5, 0x55, 0xa0, 0xaa, 0xa9, 0x5a, 0x56, 0xaa, 0xaa, 0x95,
	0xaa, 0xa2, 0xaa, 0xa4, 0xa8, 0x0a, 0xaa, 0xaa, 0xa6, 0x8a, 0xaa, 0xa6,
	0xaa, 0xa2, 0xaa, 0xa5, 0xaa, 0xa0, 0xaa, 0xa9, 0xaa, 0x8a, 0xaa, 0xaa,
	0xa6, 0x6a, 0xaa, 0xaa, 0xa0, 0x00, 0xaa, 0xa5, 0xa0, 0x00, 0xaa, 0xa5,
	0xaa, 0xa2, 0xaa, 0xa5, 0xaa, 0xa8, 0xaa, 0xa9, 0xaa, 0xa8, 0xaa, 0xa9,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xa0, 0xaa, 0xa9, 0xaa, 0xa8, 0xaa, 0xa9,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xa8, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa2, 0xaa, 0xa6, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0x2a, 0xaa, 0x6a, 0xaa, 0x8a, 0xaa, 0x9a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0x2a, 0xaa, 0x6a, 0xaa, 0xa5, 0x59, 0xa0, 0x08, 0xaa, 0x5a, 0xaa, 0x0a,
	0xaa, 0xaa, 0xaa, 0xaa, 0x80, 0x00, 0x95, 0x55, 0x80, 0x00, 0x95, 0x55,
	0xa0, 0x0a, 0xa5, 0x5a, 0x82, 0xaa, 0x96, 0xaa, 0xaa, 0x6a, 0xaa, 0x2a,
	0xa8, 0x2a, 0xa9, 0x6a, 0xa0, 0xaa, 0xa5, 0xaa, 0x80, 0x00, 0x95, 0x55,
	0xa0, 0x0a, 0xa5, 0x5a, 0xa8, 0x8a, 0xa9, 0x9a, 0x88, 0x88, 0x99, 0x99,
	0xa9, 0x95, 0xa8, 0x80, 0x00, 0x8a, 0x55, 0x9a, 0xaa, 0xa6, 0xaa, 0xa2,
	0xa2, 0xaa, 0xa6, 0xaa, 0x2a, 0xaa, 0x6a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xa2, 0x20, 0xa6, 0x65, 0xa9, 0x65, 0xa8, 0x20, 0x00, 0x00, 0x55, 0x55,
	0x00, 0x00, 0x55, 0x55, 0xa0, 0x00, 0xa5, 0x55, 0xa0, 0xa8, 0xa5, 0xa9,
	0xaa, 0xaa, 0xaa, 0xaa, 0x8a, 0xaa, 0x9a, 0xaa, 0xaa, 0x6a, 0xaa, 0x2a,
	0x22, 0x1a, 0x66, 0x4a, 0x80, 0x00, 0x95, 0x55, 0x2a, 0xaa, 0x6a, 0xaa,
	0x20, 0x10, 0x65, 0x45, 0xa0, 0xa8, 0xa5, 0xa9, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0x95, 0xaa, 0x80, 0xaa, 0x8a, 0xaa, 0x9a, 0xaa, 0xa2, 0xaa, 0xa6,
	0xa0, 0x00, 0xa5, 0x55, 0x08, 0x80, 0x59, 0x95, 0xa2, 0xa0, 0xa6, 0xa5,
	0xa0, 0xa4, 0xa5, 0xa1, 0xa8, 0xa6, 0xa9, 0xa2, 0xaa, 0xa6, 0xaa, 0xa2,
	0xaa, 0xaa, 0xaa, 0xaa, 0xa8, 0xa8, 0xa9, 0xa9, 0xaa, 0xa8, 0xaa, 0xa9,
	0xa0, 0xa2, 0xa5, 0xa6, 0x88, 0xa8, 0x99, 0xa9, 0xa0, 0xa8, 0xa5, 0xa9,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x2a, 0xaa, 0x6a, 0xaa,
	0x2a, 0xaa, 0x6a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x2a, 0xaa, 0x6a, 0xaa,
	0x2a, 0xaa, 0x6a, 0xaa, 0xaa, 0x56, 0xaa, 0x02, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa2, 0xaa, 0xa6, 0xa5, 0x59, 0xa0, 0x08, 0xa5, 0xaa, 0xa0, 0xaa,
	0x2a, 0xaa, 0x6a, 0xaa, 0xa5, 0xa6, 0xa0, 0xa2, 0xa9, 0x9a, 0xa8, 0x8a,
	0xa1, 0x1a, 0xa4, 0x4a, 0x8a, 0x2a, 0x9a, 0x6a, 0xa6, 0xaa, 0xa2, 0xaa,
	0xa9, 0x95, 0xa8, 0x80, 0x08, 0x0a, 0x59, 0x5a, 0xa5, 0x5a, 0xa0, 0x0a,
	0xa0, 0x0a, 0xa5, 0x5a, 0xaa, 0x8a, 0xaa, 0x9a, 0xaa, 0xaa, 0xaa, 0xaa,
	0xa2, 0x8a, 0xa6, 0x9a, 0xaa, 0x0a, 0xaa, 0x5a, 0xaa, 0xa6, 0xaa, 0xa2,
	0xaa, 0x2a, 0xaa, 0x6a, 0xa8, 0xaa, 0xa9, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa2, 0xaa, 0xa6, 0xa5, 0x56, 0xa0, 0x02, 0xaa, 0xaa, 0xaa, 0xaa,
	0x2a, 0xaa, 0x6a, 0xaa, 0x8a, 0xaa, 0x9a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0x00, 0x00, 0x55, 0x55, 0xaa, 0xaa, 0xaa, 0xaa, 0x8a, 0xaa, 0x9a, 0xaa,
	0xa2, 0x2a, 0xa6, 0x6a, 0x8a, 0xaa, 0x9a, 0xaa, 0xa5, 0xa6, 0xa0, 0xa2,
	0xa1, 0x2a, 0xa4, 0x6a, 0xaa, 0x5a, 0xaa, 0x0a, 0x9a, 0xaa, 0x8a, 0xaa,
	0xa2, 0xaa, 0xa6, 0xaa, 0xa8, 0x6a, 0xa9, 0x2a, 0xaa, 0xa8, 0xaa, 0xa9,
	0xa9, 0x55, 0xa8, 0x00, 0xa8, 0x8a, 0xa9, 0x9a, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa2, 0xaa, 0xa6, 0xaa, 0x80, 0xaa, 0x95, 0xaa, 0xa2, 0xaa, 0xa6,
	0xa5, 0x5a, 0xa0, 0x0a, 0xa0, 0x5a, 0xa5, 0x0a, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa2, 0xaa, 0xa6, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa8, 0xaa, 0xa9, 0x2a, 0xaa, 0x6a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0x8a, 0xaa, 0x9a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0x8a, 0xaa, 0x9a, 0xaa, 0xaa, 0x56, 0xaa, 0x02,
	0xa2, 0x00, 0xa6, 0x55, 0xa6, 0x95, 0xa2, 0x80, 0xa6, 0xaa, 0xa2, 0xaa,
	0x21, 0x2a, 0x64, 0x6a, 0x8a, 0xaa, 0x9a, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xa2, 0x1a, 0xa6, 0x4a, 0xa5, 0xaa, 0xa0, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xa9, 0xa5, 0xa8, 0xa0, 0xaa, 0x86, 0xaa, 0x92, 0xaa, 0xa6, 0xaa, 0xa2,
	0xa5, 0x95, 0xa0, 0x80, 0xa8, 0x8a, 0xa9, 0x9a, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0xa8, 0xaa, 0xa9, 0xaa, 0xaa, 0xa2, 0xaa, 0xa6,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x88, 0xaa, 0x99, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa2, 0xaa, 0xa6, 0xaa, 0xa8, 0xaa, 0xa9, 0xaa, 0xaa, 0xaa, 0xaa,
	0xa5, 0x5a, 0xa0, 0x0a, 0x8a, 0xaa, 0x9a, 0xaa, 0xa5, 0xaa, 0xa0, 0xaa,
	0xa2, 0x2a, 0xa6, 0x6a, 0xaa, 0xaa, 0xaa, 0xaa, 0x9a, 0xaa, 0x8a, 0xaa,
	0xaa, 0x00, 0xaa, 0x55, 0xaa, 0x80, 0xaa, 0x95, 0xaa, 0x5a, 0xaa, 0x0a,
	0xa2, 0x2a, 0xa6, 0x6a, 0xaa, 0xaa, 0xaa, 0xaa, 0xa5, 0x5a, 0xa0, 0x0a,
	0xa0, 0x20, 0xa5, 0x65, 0xa8, 0x6a, 0xa9, 0x2a, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa0, 0xaa, 0xa5, 0xa8, 0x8a, 0xa9, 0x9a, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa2, 0xaa, 0xa6, 0xaa, 0xa8, 0xaa, 0xa9, 0xaa, 0xa0, 0xaa, 0xa5,
	0xaa, 0x52, 0xaa, 0x06, 0xaa, 0xa8, 0xaa, 0xa9, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa2, 0xaa, 0xa6, 0xaa, 0xa8, 0xaa, 0xa9, 0xaa, 0xa8, 0xaa, 0xa9,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xa8, 0xaa, 0xa9, 0xaa, 0xa8, 0xaa, 0xa9,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x00, 0x00, 0x55, 0x55,
	0x00, 0x00, 0x55, 0x55, 0x80, 0x00, 0x95, 0x55, 0x2a, 0xaa, 0x6a, 0xaa,
	0x00, 0x00, 0x55, 0x55, 0xaa, 0xaa, 0xaa, 0xaa, 0x9a, 0xaa, 0x8a, 0xaa,
	0xaa, 0x2a, 0xaa, 0x6a, 0xa0, 0x00, 0xa5, 0x55, 0xaa, 0x08, 0xaa, 0x59,
	0x21, 0x00, 0x64, 0x55, 0xaa, 0x08, 0xaa, 0x59, 0x8a, 0xaa, 0x9a, 0xaa,
	0xa2, 0x2a, 0xa6, 0x6a, 0xa6, 0x8a, 0xa2, 0x9a, 0xaa, 0xa8, 0xaa, 0xa9,
	0xa0, 0x00, 0xa5, 0x55, 0x08, 0x80, 0x59, 0x95, 0xaa, 0xa8, 0xaa, 0xa9,
	0xaa, 0x02, 0xaa, 0x56, 0xa8, 0xa0, 0xa9, 0xa5, 0xaa, 0xa2, 0xaa, 0xa6,
	0xaa, 0xa2, 0xaa, 0xa6, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x20, 0xaa, 0x65,
	0xaa, 0x04, 0xaa, 0x51, 0x8a, 0x88, 0x9a, 0x99, 0xaa, 0x08, 0xaa, 0x59,
	0xa5, 0x5a, 0xa0, 0x0a, 0xa5, 0x8a, 0xa0, 0x9a, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa8, 0xaa, 0xa9, 0xaa, 0xaa, 0xaa, 0xaa, 0x9a, 0xaa, 0x8a, 0xaa,
	0xa2, 0x00, 0xa6, 0x55, 0xa6, 0x8a, 0xa2, 0x9a, 0xa0, 0x00, 0xa5, 0x55,
	0x92, 0x2a, 0x86, 0x6a, 0xa8, 0x80, 0xa9, 0x95, 0xa0, 0xaa, 0xa5, 0xaa,
	0xa0, 0xa0, 0xa5, 0xa5, 0xa8, 0xa0, 0xa9, 0xa5, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa0, 0xaa, 0xa5, 0xa8, 0x80, 0xa9, 0x95, 0xaa, 0x0a, 0xaa, 0x5a,
	0x00, 0x00, 0x55, 0x55, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xa0, 0xaa, 0xa5,
	0xa5, 0xa2, 0xa0, 0xa6, 0xaa, 0xa8, 0xaa, 0xa9, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xa0, 0xaa, 0xa5, 0xa6, 0x66, 0xa2, 0x22, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xa8, 0xaa, 0xa9, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
};

namespace {

/* T.82 Table 24: the probability estimation state machine */
struct QmEntry {
	u16 lsz;
	unsigned char nlps, nmps, swtch;
};

const QmEntry qm_table[113] = {
	{0x5a1d,   1,   1, 1}, {0x2586,  14,   2, 0}, {0x1114,  16,   3, 0}, {0x080b,  18,   4, 0},
	{0x03d8,  20,   5, 0}, {0x01da,  23,   6, 0}, {0x00e5,  25,   7, 0}, {0x006f,  28,   8, 0},
	{0x0036,  30,   9, 0}, {0x001a,  33,  10, 0}, {0x000d,  35,  11, 0}, {0x0006,   9,  12, 0},
	{0x0003,  10,  13, 0}, {0x0001,  12,  13, 0}, {0x5a7f,  15,  15, 1}, {0x3f25,  36,  16, 0},
	{0x2cf2,  38,  17, 0}, {0x207c,  39,  18, 0}, {0x17b9,  40,  19, 0}, {0x1182,  42,  20, 0},
	{0x0cef,  43,  21, 0}, {0x09a1,  45,  22, 0}, {0x072f,  46,  23, 0}, {0x055c,  48,  24, 0},
	{0x0406,  49,  25, 0}, {0x0303,  51,  26, 0}, {0x0240,  52,  27, 0}, {0x01b1,  54,  28, 0},
	{0x0144,  56,  29, 0}, {0x00f5,  57,  30, 0}, {0x00b7,  59,  31, 0}, {0x008a,  60,  32, 0},
	{0x0068,  62,  33, 0}, {0x004e,  63,  34, 0}, {0x003b,  32,  35, 0}, {0x002c,  33,   9, 0},
	{0x5ae1,  37,  37, 1}, {0x484c,  64,  38, 0}, {0x3a0d,  65,  39, 0}, {0x2ef1,  67,  40, 0},
	{0x261f,  68,  41, 0}, {0x1f33,  69,  42, 0}, {0x19a8,  70,  43, 0}, {0x1518,  72,  44, 0},
	{0x1177,  73,  45, 0}, {0x0e74,  74,  46, 0}, {0x0bfb,  75,  47, 0}, {0x09f8,  77,  48, 0},
	{0x0861,  78,  49, 0}, {0x0706,  79,  50, 0}, {0x05cd,  48,  51, 0}, {0x04de,  50,  52, 0},
	{0x040f,  50,  53, 0}, {0x0363,  51,  54, 0}, {0x02d4,  52,  55, 0}, {0x025c,  53,  56, 0},
	{0x01f8,  54,  57, 0}, {0x01a4,  55,  58, 0}, {0x0160,  56,  59, 0}, {0x0125,  57,  60, 0},
	{0x00f6,  58,  61, 0}, {0x00cb,  59,  62, 0}, {0x00ab,  61,  63, 0}, {0x008f,  61,  32, 0},
	{0x5b12,  65,  65, 1}, {0x4d04,  80,  66, 0}, {0x412c,  81,  67, 0}, {0x37d8,  82,  68, 0},
	{0x2fe8,  83,  69, 0}, {0x293c,  84,  70, 0}, {0x2379,  86,  71, 0}, {0x1edf,  87,  72, 0},
	{0x1aa9,  87,  73, 0}, {0x174e,  72,  74, 0}, {0x1424,  72,  75, 0}, {0x119c,  74,  76, 0},
	{0x0f6b,  74,  77, 0}, {0x0d51,  75,  78, 0}, {0x0bb6,  77,  79, 0}, {0x0a40,  77,  48, 0},
	{0x5832,  80,  81, 1}, {0x4d1c,  88,  82, 0}, {0x438e,  89,  83, 0}, {0x3bdd,  90,  84, 0},
	{0x34ee,  91,  85, 0}, {0x2eae,  92,  86, 0}, {0x299a,  93,  87, 0}, {0x2516,  86,  71, 0},
	{0x5570,  88,  89, 1}, {0x4ca9,  95,  90, 0}, {0x44d9,  96,  91, 0}, {0x3e22,  97,  92, 0},
	{0x3824,  99,  93, 0}, {0x32b4,  99,  94, 0}, {0x2e17,  93,  86, 0}, {0x56a8,  95,  96, 1},
	{0x4f46, 101,  97, 0}, {0x47e5, 102,  98, 0}, {0x41cf, 103,  99, 0}, {0x3c3d, 104, 100, 0},
	{0x375e,  99,  93, 0}, {0x5231, 105, 102, 0}, {0x4c0f, 106, 103, 0}, {0x4639, 107, 104, 0},
	{0x415e, 103,  99, 0}, {0x5627, 105, 106, 1}, {0x50e7, 108, 107, 0}, {0x4b85, 109, 103, 0},
	{0x5597, 110, 109, 0}, {0x504f, 111, 107, 0}, {0x5a10, 110, 111, 1}, {0x5522, 112, 109, 0},
	{0x59eb, 112, 111, 1},
};

/* marker codes (T.82 Table 2) */
#define J_ESC           0xff
#define J_STUFF         0x00
#define J_SDNORM        0x02
#define J_SDRST         0x03
#define J_ABORT         0x04
#define J_NEWLEN        0x05
#define J_ATMOVE        0x06
#define J_COMMENT       0x07

#define J_CONTEXTS      1024
#define J_BIH           20

/* the contexts of the typical prediction pseudo-pixel (T.82 Figures 11
   and 12) in the bit order of tmpl() */
#define J_TP3           0x0e5
#define J_TP2           0x195

/* room left of a line for the AT pixel's largest lag, and right of it
   for the template */
#define J_LPAD          132
#define J_RPAD          4

/* a big-endian 32 bit number */
u32 get32(const unsigned char *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

/* a big-endian 32 bit number appended */
void put32(std::vector<unsigned char> *out, u32 v)
{
	out->push_back((unsigned char)(v >> 24));
	out->push_back((unsigned char)(v >> 16));
	out->push_back((unsigned char)(v >> 8));
	out->push_back((unsigned char)v);
}

/* three lines (current, one above, two above), one byte a pixel, with
   white borders */
struct Lines {
	std::vector<unsigned char> buf[3];
	unsigned char *r[3];

	/* lines of width pixels, all white */
	void init(u32 width)
	{
		for (int i = 0; i < 3; i++) {
			buf[i].assign(J_LPAD + width + J_RPAD, 0);
			r[i] = buf[i].data() + J_LPAD;
		}
	}

	/* the current line becomes the one above; the new one white */
	void next(u32 width)
	{
		unsigned char *t = r[2];
		r[2] = r[1];
		r[1] = r[0];
		r[0] = t;
		memset(r[0], 0, width);
	}
};

/* pixel x's context; tx: the AT pixel's lag (0: its default place) */
inline u32 tmpl(const Lines &l, u32 x, s16 two, s16 tx)
{
	const unsigned char *r0 = l.r[0] + x, *r1 = l.r[1] + x, *r2 = l.r[2] + x;
	u32 a = tx ? r0[-tx] : r1[2];
	if (two)
		return (u32)r1[-3] << 9 | (u32)r1[-2] << 8 | (u32)r1[-1] << 7 | (u32)r1[0] << 6 |
		       (u32)r1[1] << 5 | a << 4 | (u32)r0[-4] << 3 | (u32)r0[-3] << 2 |
		       (u32)r0[-2] << 1 | r0[-1];
	return (u32)r2[-1] << 9 | (u32)r2[0] << 8 | (u32)r2[1] << 7 | (u32)r1[-2] << 6 |
	       (u32)r1[-1] << 5 | (u32)r1[0] << 4 | (u32)r1[1] << 3 | a << 2 |
	       (u32)r0[-2] << 1 | r0[-1];
}

/* a packed row unpacked into a line */
void unpack(const unsigned char *src, u32 width, unsigned char *line)
{
	for (u32 x = 0; x < width; x++)
		line[x] = (src[x >> 3] >> (7 - (x & 7))) & 1;
}

/* a line packed into a row */
void pack(const unsigned char *line, u32 width, unsigned char *row)
{
	memset(row, 0, (width + 7) / 8);
	for (u32 x = 0; x < width; x++)
		if (line[x])
			row[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
}

} // namespace

/* T.82 INITENC */
void QmEnc::init(std::vector<unsigned char> *out_)
{
	out = out_;
	c = 0;
	a = 0x10000;
	ct = 11;
	sc = 0;
	buffer = -1;
}

/* T.82 ENCODE: CODELPS or CODEMPS, then RENORME with BYTEOUT */
void QmEnc::code(QmState *st, s16 pix)
{
	s16 idx = *st & 0x7f, mps = *st >> 7;
	const QmEntry &e = qm_table[idx];
	a -= e.lsz;
	if (pix != mps) {
		if (a >= e.lsz) {
			c += a;
			a = e.lsz;
		}
		if (e.swtch)
			mps ^= 1;
		idx = e.nlps;
	} else {
		if (a >= 0x8000)
			return;
		if (a < e.lsz) {
			c += a;
			a = e.lsz;
		}
		idx = e.nmps;
	}
	*st = (QmState)(idx | mps << 7);
	do {
		a <<= 1;
		c <<= 1;
		if (--ct == 0) {
			u32 temp = c >> 19;
			if (temp > 0xff) {
				if (buffer >= 0)
					out->push_back((unsigned char)(buffer + 1));
				for (; sc; sc--)
					out->push_back(0x00);
				buffer = (s16)(temp & 0xff);
			} else if (temp == 0xff) {
				sc++;
			} else {
				if (buffer >= 0)
					out->push_back((unsigned char)buffer);
				for (; sc; sc--)
					out->push_back(0xff);
				buffer = (s16)temp;
			}
			c &= 0x7ffff;
			ct = 8;
		}
	} while (a < 0x8000);
}

/* T.82 FLUSH: CLEARBITS, then FINALWRITES (where the figure's carry
   free branch writes BUFFER, not BUFFER + 1) */
void QmEnc::flush()
{
	u32 temp = (a - 1 + c) & 0xffff0000;
	c = temp < c ? temp + 0x8000 : temp;
	c <<= ct;
	if (c > 0x7ffffff) {
		if (buffer >= 0)
			out->push_back((unsigned char)(buffer + 1));
		for (; sc; sc--)
			out->push_back(0x00);
	} else {
		if (buffer >= 0)
			out->push_back((unsigned char)buffer);
		for (; sc; sc--)
			out->push_back(0xff);
	}
	out->push_back((unsigned char)(c >> 19));
	out->push_back((unsigned char)(c >> 11));
}

/* T.82 INITDEC: three bytes into C */
void QmDec::init(const unsigned char *data_, size_t len_)
{
	data = data_;
	len = len_;
	pos = 0;
	c = 0;
	for (int i = 0; i < 3; i++) {
		c <<= 8;
		c += (u32)(pos < len ? data[pos++] : 0) << 8;
	}
	ct = 8;
	a = 0x10000;
}

/* T.82 DECODE, with LPS_EXCHANGE, MPS_EXCHANGE, RENORMD and BYTEIN */
s16 QmDec::decode(QmState *st)
{
	s16 idx = *st & 0x7f, mps = *st >> 7, pix;
	const QmEntry &e = qm_table[idx];
	a -= e.lsz;
	if ((c >> 16) < a) {
		if (a >= 0x8000)
			return mps;
		if (a < e.lsz) {
			pix = !mps;
			if (e.swtch)
				mps ^= 1;
			idx = e.nlps;
		} else {
			pix = mps;
			idx = e.nmps;
		}
	} else {
		c -= a << 16;
		if (a < e.lsz) {
			pix = mps;
			idx = e.nmps;
		} else {
			pix = !mps;
			if (e.swtch)
				mps ^= 1;
			idx = e.nlps;
		}
		a = e.lsz;
	}
	*st = (QmState)(idx | mps << 7);
	do {
		if (ct == 0) {
			c += (u32)(pos < len ? data[pos++] : 0) << 8;
			ct = 8;
		}
		a <<= 1;
		c <<= 1;
		ct--;
	} while (a < 0x8000);
	if (ct == 0) {
		c += (u32)(pos < len ? data[pos++] : 0) << 8;
		ct = 8;
	}
	return pix;
}

namespace {

/* the context of the differential-layer TP pseudo-pixel LNTP (T.82
   Figure 8) in the bit order of diff_cx(): phase 3, the six high
   resolution pixels foreground, the four low resolution pixels
   background */
#define J_TPD           0xff0

/* the DPTABLE field after the BIH (T.82 6.6.3) */
#define J_DPTABLE       1728

/* one plane of one resolution layer, a byte a pixel (0 or 1), each
   line after the other */
struct Bitmap {
	u32 w = 0, h = 0;
	std::vector<unsigned char> px;

	/* w by h pixels, all background */
	void init(u32 w_, u32 h_)
	{
		w = w_;
		h = h_;
		px.assign((size_t)w * h, 0);
	}

	/* the start of line y */
	unsigned char *row(u32 y) { return px.data() + (size_t)y * w; }
};

/* a bitmap read with the edge rules of T.82 6.1.2: background left,
   right and above line top; the line last replicated below it */
struct View {
	const Bitmap *b;
	s64 top, last;

	/* pixel (x, y) */
	unsigned char operator()(s64 x, s64 y) const
	{
		if (x < 0 || x >= (s64)b->w || y < top)
			return 0;
		if (y > last)
			y = last;
		return y < 0 ? 0 : b->px[(size_t)y * b->w + (size_t)x];
	}
};

/* bit idx of the packed Table 17: the low resolution pixel */
inline s16 rr_bit(u32 idx)
{
	return (jbig_rr_table[idx >> 3] >> (7 - (idx & 7))) & 1;
}

/* entry e of a DPTABLE: 0, 1, or 2 (no prediction; 3 is read as 2) */
inline s16 dp_entry(const unsigned char *t, u32 e)
{
	s16 v = (t[e >> 2] >> (6 - 2 * (e & 3))) & 3;
	return v > 2 ? 2 : v;
}

/* the low resolution image of hi (T.82 6.3, Figure 4 and Table 17) */
void reduce(const Bitmap &hi, Bitmap *lo)
{
	lo->init((hi.w + 1) / 2, (hi.h + 1) / 2);
	View h = {&hi, 0, (s64)hi.h - 1}, l = {lo, 0, (s64)lo->h - 1};
	for (u32 j = 0; j < lo->h; j++) {
		s64 y = 2 * (s64)j;
		for (u32 i = 0; i < lo->w; i++) {
			s64 x = 2 * (s64)i;
			u32 idx = (u32)h(x + 1, y + 1) | (u32)h(x, y + 1) << 1 | (u32)h(x - 1, y + 1) << 2 |
				  (u32)h(x + 1, y) << 3 | (u32)h(x, y) << 4 | (u32)h(x - 1, y) << 5 |
				  (u32)h(x + 1, y - 1) << 6 | (u32)h(x, y - 1) << 7 | (u32)h(x - 1, y - 1) << 8 |
				  (u32)l(i - 1, j) << 9 | (u32)l(i, (s64)j - 1) << 10 |
				  (u32)l((s64)i - 1, (s64)j - 1) << 11;
			lo->px[(size_t)j * lo->w + i] = (unsigned char)rr_bit(idx);
		}
	}
}

/* the context of a lowest-layer pixel, as tmpl() (Figures 14 and 15) */
inline u32 low_cx(const View &v, s64 x, s64 y, s16 two, s16 tx)
{
	u32 a = tx ? v(x - tx, y) : v(x + 2, y - 1);
	if (two)
		return (u32)v(x - 3, y - 1) << 9 | (u32)v(x - 2, y - 1) << 8 | (u32)v(x - 1, y - 1) << 7 |
		       (u32)v(x, y - 1) << 6 | (u32)v(x + 1, y - 1) << 5 | a << 4 | (u32)v(x - 4, y) << 3 |
		       (u32)v(x - 3, y) << 2 | (u32)v(x - 2, y) << 1 | v(x - 1, y);
	return (u32)v(x - 1, y - 2) << 9 | (u32)v(x, y - 2) << 8 | (u32)v(x + 1, y - 2) << 7 |
	       (u32)v(x - 2, y - 1) << 6 | (u32)v(x - 1, y - 1) << 5 | (u32)v(x, y - 1) << 4 |
	       (u32)v(x + 1, y - 1) << 3 | a << 2 | (u32)v(x - 2, y) << 1 | v(x - 1, y);
}

/* the context of a differential-layer pixel (Figure 16): the phase in
   bits 10 and 11, six high resolution pixels (the AT pixel in bit 8),
   then four low resolution pixels */
inline u32 diff_cx(const View &h, const View &l, s64 x, s64 y, s16 tx)
{
	s64 i = x >> 1, j = y >> 1, i0 = (x & 1) ? i : i - 1;
	u32 a = tx ? h(x - tx, y) : h(x - 1, y - 1);
	return (u32)((y & 1) << 1 | (x & 1)) << 10 | (u32)h(x, y - 2) << 9 | a << 8 |
	       (u32)h(x, y - 1) << 7 | (u32)h(x + 1, y - 1) << 6 | (u32)h(x - 2, y) << 5 |
	       (u32)h(x - 1, y) << 4 | (u32)l(i0, j) << 3 | (u32)l(i0 + 1, j) << 2 |
	       (u32)l(i0, j + 1) << 1 | l(i0 + 1, j + 1);
}

/* DPVALUE for pixel (x, y) (6.6): the table entry that Figure 13's
   reference pixels select for its phase */
inline s16 dp_value(const unsigned char *t, const View &h, const View &l, s64 x, s64 y)
{
	static const u32 base[4] = {0, 256, 768, 2816};
	static const s16 bits[4] = {8, 9, 11, 12};
	s64 i = x >> 1, j = y >> 1, hx = 2 * i, hy = 2 * j;
	u32 p = (u32)l(i - 1, j - 1) | (u32)l(i, j - 1) << 1 | (u32)l(i - 1, j) << 2 | (u32)l(i, j) << 3 |
		(u32)h(hx - 1, hy - 1) << 4 | (u32)h(hx, hy - 1) << 5 | (u32)h(hx + 1, hy - 1) << 6 |
		(u32)h(hx - 1, hy) << 7 | (u32)h(hx, hy) << 8 | (u32)h(hx + 1, hy) << 9 |
		(u32)h(hx - 1, hy + 1) << 10 | (u32)h(hx, hy + 1) << 11;
	s16 ph = (s16)((y & 1) << 1 | (x & 1));
	return dp_entry(t, base[ph] + (p & ((1u << bits[ph]) - 1)));
}

/* the colour of low resolution pixel (i, j) when it and its 8
   neighbours (Figure 5) have one colour, else 2 */
inline s16 tp_colour(const View &l, s64 i, s64 j)
{
	unsigned char c = l(i, j);
	for (s64 dy = -1; dy <= 1; dy++)
		for (s64 dx = -1; dx <= 1; dx++)
			if (l(i + dx, j + dy) != c)
				return 2;
	return c;
}

/* the arithmetic coder of an encoder or of a decoder */
struct Codec {
	QmEnc *enc;
	QmDec *dec;

	/* pix coded in the context's state, or the decoded pixel */
	s16 operator()(QmState *st, s16 pix)
	{
		if (enc) {
			enc->code(st, pix);
			return pix;
		}
		return dec->decode(st);
	}
};

/* a move of the AT pixel at line y of a layer */
struct LayerMove {
	u32 y;
	s16 tx;
};

/* the state of one bit-plane of one resolution layer: its coder's
   probabilities, AT pixel and TP state, from one stripe to the next */
struct LayerCoder {
	std::vector<QmState> st;
	s16 tx = 0, lntp_prev = 1;
	bool reset = false;             /* SDRST: the next stripe starts
					   as the top of the image */

	/* the state at the top of the image */
	void init(size_t contexts)
	{
		st.assign(contexts, 0);
		tx = 0;
		lntp_prev = 1;
		reset = false;
	}
};

/* the parameters of a progressive BIE that do not change */
struct ProgParams {
	s16 two, tpb, tpd, dp;
	const unsigned char *dp_table;
	u32 l0;
};

/* lines y0 to y1 - 1 of layer d (lo: layer d - 1, when d > 0) coded
   with c, or decoded into hi; moves: the AT moves in the stripe; ly0,
   ly1: the low resolution lines of the stripe */
void code_stripe(const ProgParams &pp, LayerCoder &lc, Codec &c, Bitmap *hi, const Bitmap *lo,
		 u32 y0, u32 y1, u32 ly0, u32 ly1, const std::vector<LayerMove> &moves,
		 JbigLayerStats *stats)
{
	s64 top = lc.reset ? (s64)y0 : 0;
	View h = {hi, top, (s64)hi->h - 1};
	View l = {lo, lc.reset ? (s64)ly0 : 0, (s64)ly1 - 1};
	lc.reset = false;
	bool enc = c.enc != nullptr;
	u32 w = hi->w;
	std::vector<s16> tpv;
	s16 lntp = 1;
	for (u32 y = y0; y < y1; y++) {
		for (const LayerMove &m : moves)
			if (m.y == y)
				lc.tx = m.tx;
		unsigned char *r = hi->row(y);
		if (!lo) {
			/* the lowest layer: TPB (6.5), then the pixels */
			if (pp.tpb) {
				s16 nt = 0;
				if (enc)
					for (u32 x = 0; x < w && !nt; x++)
						nt = r[x] != h(x, (s64)y - 1);
				s16 sl = c(&lc.st[pp.two ? J_TP2 : J_TP3], !(nt ^ lc.lntp_prev));
				nt = !(sl ^ lc.lntp_prev);
				lc.lntp_prev = nt;
				if (!nt) {
					for (u32 x = 0; x < w; x++)
						r[x] = h(x, (s64)y - 1);
					if (stats) {
						stats->tp_lines++;
						stats->tp_pixels += w;
					}
					continue;
				}
			}
			for (u32 x = 0; x < w; x++)
				r[x] = (unsigned char)c(&lc.st[low_cx(h, x, y, pp.two, lc.tx)], r[x]);
			if (stats)
				stats->coded_pixels += w;
			continue;
		}

		/* a differential layer: LNTP at the start of each line pair
		   (6.4), then TP, DP and the coder for each pixel */
		s64 j = y >> 1;
		if (pp.tpd && !(y & 1)) {
			tpv.assign(lo->w, 2);
			long ex = 0;
			for (u32 i = 0; i < lo->w; i++)
				tpv[i] = tp_colour(l, i, j);
			if (enc) {
				View all = {hi, top, (s64)hi->h - 1};
				for (u32 i = 0; i < lo->w; i++) {
					if (tpv[i] == 2)
						continue;
					s64 x = 2 * (s64)i;
					ex += (all(x, 2 * j) != tpv[i]) + (all(x + 1, 2 * j) != tpv[i]) +
					      (all(x, 2 * j + 1) != tpv[i]) + (all(x + 1, 2 * j + 1) != tpv[i]);
				}
				lntp = ex != 0;
			}
			lntp = c(&lc.st[J_TPD], lntp);
			if (stats) {
				stats->tp_exceptions += ex;
				stats->tp_lines += !lntp;
			}
		}
		h.last = y;
		for (u32 x = 0; x < w; x++) {
			if (pp.tpd && !lntp && tpv[x >> 1] != 2) {
				r[x] = (unsigned char)tpv[x >> 1];
				if (stats)
					stats->tp_pixels++;
				continue;
			}
			if (pp.dp) {
				s16 v = dp_value(pp.dp_table, h, l, x, y);
				if (v != 2) {
					r[x] = (unsigned char)v;
					if (stats)
						stats->dp_pixels++;
					continue;
				}
			}
			r[x] = (unsigned char)c(&lc.st[diff_cx(h, l, x, y, lc.tx)], r[x]);
			if (stats)
				stats->coded_pixels++;
		}
		h.last = (s64)hi->h - 1;
	}
}

/* the order of the SDEs (T.82 Table 11), its loops run as the data
   comes: s, d and p, the loop over s to the stripes there are now (a
   NEWLEN can make them fewer) */
struct SdeOrder {
	s16 loop[3];                    /* outer to inner: 0 s, 1 d, 2 p */
	u32 idx[3] = {0, 0, 0};
	u32 lim_d, lim_p;
	bool started = false, done = false;

	/* the loops of Table 11 for the order byte; false when it is one
	   of the two combinations not allowed */
	bool init(s16 order, u32 layers, u32 planes)
	{
		static const s16 t[8][3] = {
			{2, 1, 0}, {-1, 0, 0}, {1, 2, 0}, {1, 0, 2},
			{0, 2, 1}, {2, 0, 1}, {0, 1, 2}, {-1, 0, 0},
		};
		const s16 *r = t[order & 7];
		if (r[0] < 0)
			return false;
		for (int k = 0; k < 3; k++)
			loop[k] = r[k];
		lim_d = layers;
		lim_p = planes;
		return true;
	}

	/* the limit of loop k with S stripes */
	u32 lim(int k, u32 stripes) const
	{
		return loop[k] == 0 ? stripes : loop[k] == 1 ? lim_d : lim_p;
	}

	/* the indexes valid with S stripes */
	bool valid(u32 stripes) const
	{
		for (int k = 0; k < 3; k++)
			if (idx[k] >= lim(k, stripes))
				return false;
		return true;
	}

	/* the next SDE with S stripes; false after the last */
	bool next(u32 stripes)
	{
		if (done)
			return false;
		if (started)
			idx[2]++;
		started = true;
		for (;;) {
			int k = 0;
			while (k < 3 && idx[k] < lim(k, stripes))
				k++;
			if (k == 3)
				return true;
			if (k == 0) {
				done = true;
				return false;
			}
			for (int m = k; m < 3; m++)
				idx[m] = 0;
			idx[k - 1]++;
		}
	}

	/* the index of the loop over s (0), d (1) or p (2) */
	u32 get(s16 v) const
	{
		for (int k = 0; k < 3; k++)
			if (loop[k] == v)
				return idx[k];
		return 0;
	}
};

/* the lines of the layers below layer D: Y(d-1) = ceil(Y(d) / 2) */
std::vector<u32> layer_sizes(u32 top, s16 layers)
{
	std::vector<u32> v((size_t)layers + 1);
	v[layers] = top;
	for (s16 d = layers; d > 0; d--)
		v[d - 1] = (u32)(((u64)v[d] + 1) / 2);
	return v;
}

/* S, the stripes of the lowest layer, Y0 / L0 rounded up */
u32 stripe_count(u32 y0, u32 l0)
{
	return (u32)(((u64)y0 + l0 - 1) / l0);
}

/* the first line of stripe s in a layer of stripes of ld lines, at
   most h */
u32 stripe_line(u32 s, u64 ld, u32 h)
{
	u64 y = (u64)s * ld;
	return y < h ? (u32)y : h;
}

/* the escaped SCD and its end marker appended */
void put_sde(std::vector<unsigned char> *bie, const std::vector<unsigned char> &scd)
{
	for (unsigned char b : scd) {
		bie->push_back(b);
		if (b == J_ESC)
			bie->push_back(J_STUFF);
	}
	bie->push_back(J_ESC);
	bie->push_back(J_SDNORM);
}

/* T.82 progressive coding (opt.layers differential layers): the
   layers made by resolution reduction, then the SDEs in opt.order */
void encode_progressive(const JbigImage &img, const JbigOptions &opt, std::vector<unsigned char> *bie)
{
	s16 layers = std::min<s16>(opt.layers, 255);
	s16 planes = std::max<s16>(img.planes, 1);
	u32 l0 = opt.l0 ? opt.l0 : 128;
	s16 order = opt.order & (JBG_HITOLO | JBG_SEQ | JBG_ILEAVE | JBG_SMID);
	SdeOrder ord;
	if (!ord.init(order, (u32)layers + 1, (u32)planes)) {
		order &= JBG_HITOLO;
		ord.init(order, (u32)layers + 1, (u32)planes);
	}
	s16 options = opt.options & (JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON);
	ProgParams pp = {(s16)((options & JBG_LRLTWO) != 0), (s16)((options & JBG_TPBON) != 0),
			 (s16)((options & JBG_TPDON) != 0), (s16)((options & JBG_DPON) != 0),
			 jbig_dp_table, l0};

	bie->clear();
	bie->push_back(0);                      /* DL */
	bie->push_back((unsigned char)layers);  /* D */
	bie->push_back((unsigned char)planes);  /* P */
	bie->push_back(0);
	put32(bie, img.width);
	put32(bie, img.height);
	put32(bie, l0);
	bie->push_back((unsigned char)opt.mx);
	bie->push_back(0);                      /* MY */
	bie->push_back((unsigned char)order);
	bie->push_back((unsigned char)options);

	/* each plane at each resolution, the lowest first */
	std::vector<u32> ys = layer_sizes(img.height, layers);
	std::vector<std::vector<Bitmap>> bm(planes, std::vector<Bitmap>((size_t)layers + 1));
	std::vector<std::vector<LayerCoder>> lc(planes, std::vector<LayerCoder>((size_t)layers + 1));
	size_t stride = img.stride();
	for (s16 i = 0; i < planes; i++) {
		Bitmap &top = bm[i][layers];
		top.init(img.width, img.height);
		if (i < (s16)img.bits.size())
			for (u32 y = 0; y < img.height; y++)
				unpack(img.bits[i].data() + y * stride, img.width, top.row(y));
		for (s16 d = layers; d > 0; d--)
			reduce(bm[i][d], &bm[i][d - 1]);
		for (s16 d = 0; d <= layers; d++)
			lc[i][d].init(d ? 4096 : J_CONTEXTS);
	}
	if (opt.stats)
		for (s16 d = 0; d <= layers; d++)
			opt.stats[d] = JbigLayerStats();

	u32 stripes = stripe_count(ys[0], l0);
	std::vector<unsigned char> scd;
	std::vector<LayerMove> moves;
	QmEnc enc;
	Codec c = {&enc, nullptr};
	while (ord.next(stripes)) {
		u32 s = ord.get(0), dn = ord.get(1);
		s16 d = (s16)((order & JBG_HITOLO) ? layers - (s16)dn : (s16)dn);
		s16 i = (s16)ord.get(2);
		u64 ld = (u64)l0 << d;
		u32 y0 = stripe_line(s, ld, ys[d]), y1 = stripe_line(s + 1, ld, ys[d]);
		moves.clear();
		for (size_t m = 0; m < opt.n_moves; m++) {
			const JbigAtMove &mv = opt.moves[m];
			if (mv.d != d || mv.y < y0 || mv.y >= y1)
				continue;
			bie->push_back(J_ESC);
			bie->push_back(J_ATMOVE);
			put32(bie, mv.y - y0);
			bie->push_back((unsigned char)mv.tx);
			bie->push_back(0);
			moves.push_back({mv.y, mv.tx});
		}
		scd.clear();
		enc.init(&scd);
		const Bitmap *lo = d ? &bm[i][d - 1] : nullptr;
		u32 ly0 = 0, ly1 = 0;
		if (d) {
			ly0 = stripe_line(s, ld / 2, ys[d - 1]);
			ly1 = stripe_line(s + 1, ld / 2, ys[d - 1]);
		}
		code_stripe(pp, lc[i][d], c, &bm[i][d], lo, y0, y1, ly0, ly1, moves,
			    opt.stats ? &opt.stats[d] : nullptr);
		enc.flush();
		while (!scd.empty() && scd.back() == 0x00)
			scd.pop_back();
		if (opt.stats)
			opt.stats[d].scd_bytes += (long)scd.size();
		put_sde(bie, scd);
	}
}

/* one SDE of a progressive BIE, as it came */
struct Sde {
	std::vector<unsigned char> scd;
	std::vector<LayerMove> moves;   /* the lines from the stripe's top */
	s16 end = 0;                    /* J_SDNORM, J_SDRST, or 0: cut */
	bool have = false;
};

/* a progressive BIE (D more than 0) after its header (hdr octets): all
   its SDEs collected, then each plane decoded from the lowest layer
   up; as jbig_decode_image() */
s16 decode_progressive(const unsigned char *data, size_t len, size_t hdr, u32 max_rows,
		       const unsigned char *dp_table, JbigImage *img, size_t *used)
{
	s16 layers = data[1], planes = data[2];
	u32 xd = get32(data + 4), yd = get32(data + 8), l0 = get32(data + 12);
	s16 mx = data[16], order = data[18] & 0x0f, options = data[19];
	ProgParams pp = {(s16)((options & JBG_LRLTWO) != 0), (s16)((options & JBG_TPBON) != 0),
			 (s16)((options & JBG_TPDON) != 0), (s16)((options & JBG_DPON) != 0), dp_table,
			 l0};
	SdeOrder ord;
	if (!ord.init(order, (u32)layers + 1, (u32)planes))
		return -1;

	/* the SDEs by plane, layer and stripe */
	std::vector<std::vector<std::vector<Sde>>> sdes(planes,
		std::vector<std::vector<Sde>>((size_t)layers + 1));
	std::vector<LayerMove> pending;
	size_t pos = hdr;
	s16 r = 0;

	/* marker segments up to anything else: 0, 1 at the data's end or
	   ABORT, -1 when bad; trail: after the last SDE, where an unknown
	   marker ends them (0) */
	auto markers = [&](bool trail) -> s16 {
		for (;;) {
			if (pos >= len)
				return 1;
			if (data[pos] != J_ESC || pos + 1 >= len || data[pos + 1] == J_STUFF ||
			    data[pos + 1] == J_SDNORM || data[pos + 1] == J_SDRST)
				return 0;
			switch (data[pos + 1]) {
			case J_ATMOVE:
				if (pos + 8 > len)
					return 1;
				if (data[pos + 7] != 0 || data[pos + 6] > mx || data[pos + 6] > 127 ||
				    (data[pos + 6] && data[pos + 6] < 3))
					return -1;
				pending.push_back({get32(data + pos + 2), data[pos + 6]});
				pos += 8;
				break;
			case J_NEWLEN: {
				if (pos + 6 > len)
					return 1;
				u32 n = get32(data + pos + 2);
				if (n < yd)
					yd = n;
				pos += 6;
				break;
			}
			case J_COMMENT: {
				if (pos + 6 > len)
					return 1;
				u32 n = get32(data + pos + 2);
				if (n > len - pos - 6)
					return 1;
				pos += 6 + n;
				break;
			}
			case J_ABORT:
				return 1;
			default:
				return trail ? 0 : -1;
			}
		}
	};

	/* the stripes there are now */
	auto stripes = [&]() { return stripe_count(layer_sizes(yd, layers)[0], l0); };

	while (!r && ord.next(stripes())) {
		r = markers(false);
		if (r)
			break;
		if (!ord.valid(stripes()))
			continue;       /* (a NEWLEN before it made it go) */
		s16 dn = (s16)ord.get(1);
		std::vector<Sde> &v = sdes[ord.get(2)][(order & JBG_HITOLO) ? layers - dn : dn];
		if (v.size() <= ord.get(0))
			v.resize((size_t)ord.get(0) + 1);
		Sde &e = v[ord.get(0)];
		e.moves.swap(pending);
		pending.clear();
		e.have = true;
		while (pos < len) {
			unsigned char b = data[pos++];
			if (b != J_ESC) {
				e.scd.push_back(b);
				continue;
			}
			if (pos >= len)
				break;
			b = data[pos++];
			if (b == J_STUFF) {
				e.scd.push_back(J_ESC);
				continue;
			}
			if (b != J_SDNORM && b != J_SDRST)
				return -1;
			e.end = b;
			break;
		}
		if (!e.end)
			r = 1;
	}
	if (r < 0)
		return -1;
	if (!r && markers(true) == 1 && pos < len)
		r = 1;          /* (ABORT after the last SDE) */
	if (used)
		*used = pos;

	/* the stripes that the lines asked for need, in each layer */
	std::vector<u32> ys = layer_sizes(yd, layers);
	u32 want = std::min(yd, max_rows);
	u64 ltop = (u64)l0 << layers;
	u32 need = (u32)std::min<u64>(((u64)want + ltop - 1) / ltop, stripes());
	size_t stride = (xd + 7) / 8;
	u32 h = want;
	std::vector<u32> xs((size_t)layers + 1);
	xs[layers] = xd;
	for (s16 d = layers; d > 0; d--)
		xs[d - 1] = (xs[d] + 1) / 2;
	img->bits.assign(planes, std::vector<unsigned char>());
	std::vector<unsigned char> row(stride);
	for (s16 i = 0; i < planes; i++) {
		std::vector<Bitmap> bm((size_t)layers + 1);
		u32 have = need;        /* stripes decoded in the layer below */
		for (s16 d = 0; d <= layers; d++) {
			u64 ld = (u64)l0 << d;
			bm[d].init(xs[d], stripe_line(have, ld, ys[d]));
			LayerCoder lc;
			lc.init(d ? 4096 : J_CONTEXTS);
			QmDec dec;
			Codec c = {nullptr, &dec};
			u32 s = 0;
			for (; s < have; s++) {
				if (s >= sdes[i][d].size() || !sdes[i][d][s].have)
					break;
				Sde &e = sdes[i][d][s];
				u32 y0 = stripe_line(s, ld, ys[d]), y1 = stripe_line(s + 1, ld, ys[d]);
				std::vector<LayerMove> mv;
				for (const LayerMove &m : e.moves)
					mv.push_back({y0 + m.y, m.tx});
				u32 ly0 = 0, ly1 = 0;
				if (d) {
					ly0 = stripe_line(s, ld / 2, ys[d - 1]);
					ly1 = stripe_line(s + 1, ld / 2, ys[d - 1]);
				}
				dec.init(e.scd.data(), e.scd.size());
				code_stripe(pp, lc, c, &bm[d], d ? &bm[d - 1] : nullptr, y0, y1, ly0, ly1,
					    mv, nullptr);
				if (e.end == J_SDRST) {
					lc.init(lc.st.size());
					lc.reset = true;
				}
				if (!e.end) {
					s++;
					break;
				}
			}
			if (s < have)
				r = r ? r : 1;
			have = s;
			if (d) {
				/* (the layer below no longer needed) */
				Bitmap().px.swap(bm[d - 1].px);
			}
		}
		u32 rows = std::min(want, stripe_line(have, ltop, ys[layers]));
		h = std::min(h, rows);
		img->bits[i].resize((size_t)rows * stride);
		for (u32 y = 0; y < rows; y++) {
			pack(bm[layers].row(y), xd, row.data());
			memcpy(img->bits[i].data() + (size_t)y * stride, row.data(), stride);
		}
	}
	img->height = h;
	for (auto &b : img->bits)
		b.resize((size_t)h * stride);
	return r;
}

} // namespace

/* each plane reduced to half its resolution */
void jbig_reduce(const JbigImage &hi, JbigImage *lo)
{
	lo->width = (hi.width + 1) / 2;
	lo->height = (hi.height + 1) / 2;
	lo->planes = hi.planes;
	lo->bits.assign(hi.bits.size(), std::vector<unsigned char>());
	size_t hs = hi.stride(), ls = lo->stride();
	for (size_t i = 0; i < hi.bits.size(); i++) {
		Bitmap a, b;
		a.init(hi.width, hi.height);
		for (u32 y = 0; y < hi.height; y++)
			unpack(hi.bits[i].data() + y * hs, hi.width, a.row(y));
		reduce(a, &b);
		lo->bits[i].assign(ls * lo->height, 0);
		for (u32 y = 0; y < lo->height; y++)
			pack(b.row(y), lo->width, lo->bits[i].data() + y * ls);
	}
}

/* the order of the SDEs, T.82 Table 11 for one layer: true when the
   stripe is the outer loop (each stripe in every plane, then the next),
   false when the plane is; -1 for the two combinations not allowed */
static s16 stripe_outer(s16 order)
{
	s16 seq = (order & JBG_SEQ) != 0, il = (order & JBG_ILEAVE) != 0,
	    smid = (order & JBG_SMID) != 0;
	if (smid && seq == il)
		return -1;
	return seq ? !smid : il && smid;
}

/* the image's planes in stripes of opt.l0 lines, each stripe of each
   plane its own SCD */
void jbig_encode_image(const JbigImage &img, const JbigOptions &opt, std::vector<unsigned char> *bie)
{
	if (opt.layers > 0) {
		encode_progressive(img, opt, bie);
		return;
	}
	u32 width = img.width, rows = img.height;
	u32 l0 = opt.l0 ? opt.l0 : 128;
	s16 planes = std::max<s16>(img.planes, 1);
	s16 two = (opt.options & JBG_LRLTWO) != 0, tp = (opt.options & JBG_TPBON) != 0;
	s16 order = opt.order & (JBG_SEQ | JBG_ILEAVE | JBG_SMID);
	if (stripe_outer(order) < 0)
		order = 0;
	size_t stride = img.stride();
	bie->clear();
	bie->push_back(0);                      /* DL */
	bie->push_back(0);                      /* D */
	bie->push_back((unsigned char)planes);  /* P */
	bie->push_back(0);
	put32(bie, width);
	put32(bie, rows);
	put32(bie, l0);
	bie->push_back((unsigned char)opt.mx);
	bie->push_back(0);                      /* MY */
	bie->push_back((unsigned char)order);   /* HITOLO 0 */
	bie->push_back((unsigned char)(opt.options & (JBG_LRLTWO | JBG_TPBON | JBG_VLENGTH)));

	struct Plane {
		std::vector<QmState> st;
		Lines l;
		s16 lntp_prev = 1, tx = 0;
	};
	std::vector<Plane> pl(planes);
	for (Plane &p : pl) {
		p.st.assign(J_CONTEXTS, 0);
		p.l.init(width);
	}
	if (opt.stats)
		*opt.stats = JbigLayerStats();
	std::vector<unsigned char> scd;
	QmEnc enc;
	u32 stripes = (rows + l0 - 1) / l0;
	s16 outer = stripe_outer(order);
	for (u32 k = 0; k < stripes * (u32)planes; k++) {
		u32 s = outer ? k / planes : k % stripes;
		s16 i = (s16)(outer ? k % planes : k / stripes);
		Plane &p = pl[i];
		const unsigned char *bits = i < (s16)img.bits.size() ? img.bits[i].data() : nullptr;
		u32 y0 = s * l0, y1 = std::min(rows, y0 + l0);
		for (size_t m = 0; m < opt.n_moves; m++) {
			const JbigAtMove &mv = opt.moves[m];
			if (mv.y < y0 || mv.y >= y1)
				continue;
			bie->push_back(J_ESC);
			bie->push_back(J_ATMOVE);
			put32(bie, mv.y - y0);  /* (from the stripe's top) */
			bie->push_back((unsigned char)mv.tx);
			bie->push_back(0);
		}
		scd.clear();
		enc.init(&scd);
		for (u32 y = y0; y < y1; y++) {
			for (size_t m = 0; m < opt.n_moves; m++)
				if (opt.moves[m].y == y)
					p.tx = opt.moves[m].tx;
			p.l.next(width);
			if (bits)
				unpack(bits + y * stride, width, p.l.r[0]);
			if (tp) {
				s16 lntp = memcmp(p.l.r[0], p.l.r[1], width) != 0;
				enc.code(&p.st[two ? J_TP2 : J_TP3], !(lntp ^ p.lntp_prev));
				p.lntp_prev = lntp;
				if (!lntp) {
					if (opt.stats) {
						opt.stats->tp_lines++;
						opt.stats->tp_pixels += width;
					}
					continue;
				}
			}
			for (u32 x = 0; x < width; x++)
				enc.code(&p.st[tmpl(p.l, x, two, p.tx)], p.l.r[0][x]);
			if (opt.stats)
				opt.stats->coded_pixels += width;
		}
		enc.flush();
		while (!scd.empty() && scd.back() == 0x00)
			scd.pop_back();
		if (opt.stats)
			opt.stats->scd_bytes += (long)scd.size();
		for (unsigned char b : scd) {
			bie->push_back(b);
			if (b == J_ESC)
				bie->push_back(J_STUFF);
		}
		bie->push_back(J_ESC);
		bie->push_back(J_SDNORM);
	}
}

/* the header checked, then marker segments and stripes in the order
   the header gives */
s16 jbig_decode_image(const unsigned char *data, size_t len, u32 max_rows, s16 max_planes,
		      JbigImage *img, size_t *used)
{
	img->width = img->height = 0;
	img->planes = 0;
	img->bits.clear();
	if (used)
		*used = 0;
	if (len < J_BIH || data[0] != 0 || data[2] == 0 || data[2] > max_planes)
		return -1;
	s16 planes = data[2];
	u32 xd = get32(data + 4), yd = get32(data + 8), l0 = get32(data + 12);
	s16 mx = data[16], options = data[19], outer = stripe_outer(data[18] & 7);
	if (xd == 0 || xd > 8192 || l0 == 0 || data[17] != 0 || outer < 0)
		return -1;

	/* a private DP table follows the header (DPLAST 0), or is the one
	   of an earlier BIE (DPLAST 1: not kept here, so used only when DP
	   is not) */
	size_t hdr = J_BIH;
	const unsigned char *dpt = jbig_dp_table;
	if ((options & JBG_DPON) && (options & JBG_DPPRIV)) {
		if (options & JBG_DPLAST) {
			if (data[1])
				return -1;
		} else {
			if (len < J_BIH + J_DPTABLE)
				return -1;
			dpt = data + J_BIH;
			hdr += J_DPTABLE;
		}
	}
	if (data[1]) {
		img->width = xd;
		img->planes = planes;
		return decode_progressive(data, len, hdr, max_rows, dpt, img, used);
	}
	s16 two = (options & JBG_LRLTWO) != 0, tp = (options & JBG_TPBON) != 0;
	u32 cap = max_rows;
	size_t stride = (xd + 7) / 8;
	img->width = xd;
	img->planes = planes;
	img->bits.resize(planes);

	struct Move {
		u32 y;
		s16 tx;
	};
	struct Plane {
		std::vector<QmState> st;
		Lines l;
		std::vector<Move> moves;
		s16 lntp_prev = 1, tx = 0;
		u32 rows = 0;
	};
	std::vector<Plane> pl(planes);
	for (Plane &p : pl) {
		p.st.assign(J_CONTEXTS, 0);
		p.l.init(xd);
	}
	std::vector<Move> pending;              /* ATMOVEs for the next SDE */
	std::vector<unsigned char> scd;
	QmDec dec;
	size_t pos = hdr;

	/* marker segments up to an SDE; 0 at an SDE, 2 when none may follow
	   (the page's lines all there: from NEWLEN, or done), 1 at the data's
	   end or ABORT, -1 when bad; trail: after the last stripe, up to
	   anything else (0) */
	auto markers = [&](u32 y0, bool trail) -> s16 {
		for (;;) {
			if (!trail && y0 >= yd)
				return 2;
			if (pos >= len)
				return 1;
			if (data[pos] != J_ESC || pos + 1 >= len || data[pos + 1] == J_STUFF ||
			    data[pos + 1] == J_SDNORM || data[pos + 1] == J_SDRST)
				return 0;
			switch (data[pos + 1]) {
			case J_ATMOVE:
				if (pos + 8 > len)
					return 1;
				if (data[pos + 7] != 0 || data[pos + 6] > mx || data[pos + 6] > 127 ||
				    (data[pos + 6] && data[pos + 6] < 3))
					return -1;
				/* yAT counts from the top of the stripe that
				   follows */
				pending.push_back({get32(data + pos + 2), data[pos + 6]});
				pos += 8;
				break;
			case J_NEWLEN: {
				if (pos + 6 > len)
					return 1;
				u32 n = get32(data + pos + 2);
				if (n < yd)
					yd = n;
				pos += 6;
				break;
			}
			case J_COMMENT: {
				if (pos + 6 > len)
					return 1;
				u32 n = get32(data + pos + 2);
				if (n > len - pos - 6)
					return 1;
				pos += 6 + n;
				break;
			}
			case J_ABORT:
				return 1;
			default:
				return trail ? 0 : -1;
			}
		}
	};

	/* plane i's stripe s; as markers() */
	auto stripe = [&](s16 i, u32 s) -> s16 {
		u32 y0 = s * l0;
		s16 r = markers(y0, false);
		if (r)
			return r;
		Plane &p = pl[i];
		for (const Move &m : pending)
			p.moves.push_back({y0 + m.y, m.tx});
		pending.clear();

		/* an SDE: unescape up to its end */
		scd.clear();
		s16 end = 0;
		while (pos < len) {
			unsigned char b = data[pos++];
			if (b != J_ESC) {
				scd.push_back(b);
				continue;
			}
			if (pos >= len)
				break;
			b = data[pos++];
			if (b == J_STUFF) {
				scd.push_back(J_ESC);
				continue;
			}
			if (b != J_SDNORM && b != J_SDRST)
				return -1;
			end = b;
			break;
		}

		dec.init(scd.data(), scd.size());
		u32 y1 = std::min(std::min(yd, cap), y0 + l0);
		if (y1 > y0 && y0 == p.rows) {
			img->bits[i].resize((size_t)y1 * stride);
			for (u32 y = y0; y < y1; y++) {
				for (const Move &m : p.moves)
					if (m.y == y)
						p.tx = m.tx;
				p.l.next(xd);
				s16 copy = 0;
				if (tp) {
					s16 lntp = !(dec.decode(&p.st[two ? J_TP2 : J_TP3]) ^ p.lntp_prev);
					p.lntp_prev = lntp;
					copy = !lntp;
				}
				if (copy)
					memcpy(p.l.r[0], p.l.r[1], xd);
				else
					for (u32 x = 0; x < xd; x++)
						p.l.r[0][x] = (unsigned char)dec.decode(&p.st[tmpl(p.l, x, two, p.tx)]);
				pack(p.l.r[0], xd, img->bits[i].data() + (size_t)y * stride);
				p.rows = y + 1;
			}
		}
		if (!end)
			return 1;
		if (end == J_SDRST) {
			/* the next stripe starts as the top of the image does
			   (6.2.5): the lines above it background too */
			std::fill(p.st.begin(), p.st.end(), 0);
			p.tx = 0;
			p.lntp_prev = 1;
			p.l.init(xd);
		}
		return 0;
	};

	s16 r = 0;
	if (outer) {
		for (u32 s = 0; !r && s * l0 < std::min(yd, cap); s++)
			for (s16 i = 0; !r && i < planes; i++)
				r = stripe(i, s);
	} else {
		/* (the last plane's stripes past the cap not read) */
		for (s16 i = 0; !r && i < planes; i++) {
			for (u32 s = 0; !r && s * l0 < (i == planes - 1 ? std::min(yd, cap) : yd); s++)
				r = stripe(i, s);
			if (r == 2)
				r = 0;
		}
	}
	if (r == 2)
		r = 0;
	if (!r && markers(0, true) == 1 && pos < len)
		r = 1;          /* (ABORT after the last stripe) */
	u32 h = std::min(yd, cap);
	for (const Plane &p : pl)
		h = std::min(h, p.rows);
	img->height = h;
	for (auto &b : img->bits)
		b.resize((size_t)h * stride);
	if (used)
		*used = pos;
	return r;
}

/* the page as one plane */
void jbig_encode(const T4Page *page, const JbigOptions &opt, std::vector<unsigned char> *bie)
{
	JbigImage img;
	img.width = (u32)std::max<s16>(page->width, 0);
	img.height = (u32)std::max<s32>(page->rows, 0);
	img.planes = 1;
	img.bits.emplace_back(page->bits.begin(),
			      page->bits.begin() + (long)img.height * T4_STRIDE(page->width));
	JbigOptions o = opt;
	o.order = 0;
	jbig_encode_image(img, o, bie);
}

/* one plane's image into the page's rows */
s16 jbig_decode(const unsigned char *data, size_t len, s32 max_rows, T4Page *page)
{
	if (len >= J_BIH && (data[1] != 0 || (data[19] & JBG_DPON)))
		return -1;      /* (not in T.85) */
	JbigImage img;
	s16 r = jbig_decode_image(data, len, max_rows > 0 ? (u32)max_rows : 0, 1, &img, nullptr);
	if (r < 0)
		return r;
	s16 fine = page->fine;
	page->release();
	page->init((s16)img.width, fine);
	for (u32 y = 0; y < img.height; y++)
		if (!page->add_row(img.bits[0].data() + (size_t)y * img.stride()))
			return 1;
	return r;
}

} // namespace dsp
