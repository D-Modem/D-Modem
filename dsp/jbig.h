/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    jbig.h - ITU-T T.82 (JBIG1) bi-level image coding as T.85 profiles
 *             it for fax (one resolution layer, one bit-plane,
 *             sequential stripes, the QM arithmetic coder), with more
 *             bit-planes as T.43 uses it, and progressive coding
 *             (differential layers, TPDON, DPON)
 *
 */

#ifndef __JBIG_H__
#define __JBIG_H__

#include <cstddef>
#include <vector>

#include "dm_types.h"
#include "t4.h"

namespace dsp {

/* one context's adaptive probability state: the index into T.82's
   Table 24 in the low 7 bits, the more probable symbol in bit 7 */
typedef unsigned char QmState;

/* the QM arithmetic encoder (T.82 6.8.2); its bytes, the stripe coded
   data (SCD), are appended to out */
struct QmEnc {
	u32 c, a, sc;
	s16 ct;
	s16 buffer;                     /* the byte held back; -1: none yet */
	std::vector<unsigned char> *out;

	void init(std::vector<unsigned char> *out_);
	/* one pixel in a context's state */
	void code(QmState *st, s16 pix);
	/* the last bytes (trailing 0x00 bytes may be left out: the
	   decoder reads 0x00 past the end) */
	void flush();
};

/* the QM arithmetic decoder (T.82 6.8.3) on one SCD */
struct QmDec {
	const unsigned char *data;
	size_t len, pos;
	u32 c, a;
	s16 ct;

	void init(const unsigned char *data_, size_t len_);
	/* one pixel in a context's state */
	s16 decode(QmState *st);
};

/* the BIH's options byte (T.82 Table 8) */
#define JBG_LRLTWO      0x40    /* the two line template */
#define JBG_VLENGTH     0x20    /* NEWLEN may follow */
#define JBG_TPDON       0x10    /* typical prediction, differential
					   layers */
#define JBG_TPBON       0x08    /* typical prediction, lowest layer */
#define JBG_DPON        0x04    /* deterministic prediction */
#define JBG_DPPRIV      0x02    /* a private DP table */
#define JBG_DPLAST      0x01    /* ... the one last used */

/* T.82 Table 17, the resolution reduction: bit 7 - (i & 7) of byte
   i >> 3 is the low resolution pixel for index i */
extern const unsigned char jbig_rr_table[512];

/* T.82 Tables 19 to 22, the default DP tables, packed as the DPTABLE
   field (6.6.3): two bits an entry, the first in the top bits; phase
   0 at entry 0, 1 at 256, 2 at 768, 3 at 2816 */
extern const unsigned char jbig_dp_table[1728];

/* the BIH's order byte (T.82 Table 7) */
#define JBG_HITOLO      0x08    /* the differential layers high to low */
#define JBG_SEQ         0x04
#define JBG_ILEAVE      0x02    /* the bit-planes interleaved */
#define JBG_SMID        0x01    /* ... a stripe of each in turn */

/* a moved adaptive template pixel, for the encoder's tests */
struct JbigAtMove {
	u32 y;                          /* the first line it applies to (in
					   the image of its layer) */
	s16 tx;                         /* its lag to the left; 0 default */
	s16 d = 0;                      /* its resolution layer (0: the
					   lowest) */
};

/* what the encoder did in one resolution layer (all its planes), the
   columns of T.82 Table 30 */
struct JbigLayerStats {
	long tp_lines = 0;              /* lines (line pairs in a
					   differential layer) with LNTP 0 */
	long tp_exceptions = 0;         /* high resolution pixels not the
					   colour of a low resolution pixel
					   and its 8 neighbours */
	long tp_pixels = 0;             /* pixels that TP predicts */
	long dp_pixels = 0;             /* pixels that DP predicts */
	long coded_pixels = 0;          /* pixels the coder codes (not
					   the pseudo-pixels) */
	long scd_bytes = 0;             /* the SCD of all its stripes */
};

/* how a page is coded */
struct JbigOptions {
	u32 l0 = 128;                   /* lines in a stripe (T.85's basic
					   value; others need DIS bit 79) */
	s16 options = JBG_TPBON;        /* JBG_LRLTWO, JBG_TPBON */
	s16 mx = 0;                     /* the AT pixel's largest lag */
	const JbigAtMove *moves = nullptr;
	size_t n_moves = 0;
	s16 order = 0;                  /* T.82 Table 7: JBG_HITOLO,
					   JBG_SEQ, JBG_ILEAVE, JBG_SMID */
	s16 layers = 0;                 /* D: the differential layers (0:
					   sequential coding) */
	JbigLayerStats *stats = nullptr; /* if given: layers + 1 entries,
					   the lowest layer first */
};

/* an image of bit-planes, the first the most significant: each row
   after another, packed, the leftmost pixel in a byte's top bit, 1
   black */
struct JbigImage {
	u32 width = 0, height = 0;
	s16 planes = 0;
	std::vector<std::vector<unsigned char>> bits;

	/* bytes in a row */
	size_t stride() const { return (width + 7) / 8; }
};

/* an image at half the resolution, as T.82 6.3 reduces it: each plane
   reduced with Table 17 */
void jbig_reduce(const JbigImage &hi, JbigImage *lo);

/* an image as a BIE of its planes: the header, then the stripe data
   entities in opt.order; with opt.layers, the lowest resolution layer
   and the differential layers (TPDON and DPON as opt.options give) */
void jbig_encode_image(const JbigImage &img, const JbigOptions &opt, std::vector<unsigned char> *bie);

/* a BIE (DL 0, any D) of at most max_planes planes to an image of at
   most max_rows lines at its highest resolution; *used (if given): the octets up to its end, and
   marker segments after its last stripe.  0 when whole, 1 when the data
   ended (or ABORT came) early, with the lines that every plane has so
   far, -1 when it is none */
s16 jbig_decode_image(const unsigned char *data, size_t len, u32 max_rows, s16 max_planes,
		      JbigImage *img, size_t *used);

/* a page as a bi-level image entity (BIE): the 20 byte header and one
   stripe data entity for each l0 lines */
void jbig_encode(const T4Page *page, const JbigOptions &opt, std::vector<unsigned char> *bie);

/* a BIE to a page of its own width, at most max_rows lines; 0 when
   whole, 1 when the data ended (or ABORT came) early with the lines
   decoded so far, -1 when it is no T.85 BIE (T.85 fixes D, TPDON and
   DPON to 0) */
s16 jbig_decode(const unsigned char *data, size_t len, s32 max_rows, T4Page *page);

} // namespace dsp

#endif /* __JBIG_H__ */
