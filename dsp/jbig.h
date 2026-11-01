/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    jbig.h - ITU-T T.82 (JBIG1) bi-level image coding as T.85 profiles
 *             it for fax: one resolution layer, one bit-plane,
 *             sequential stripes, the QM arithmetic coder
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
#define JBG_TPDON       0x10
#define JBG_TPBON       0x08    /* typical prediction */
#define JBG_DPON        0x04
#define JBG_DPPRIV      0x02
#define JBG_DPLAST      0x01

/* a moved adaptive template pixel, for the encoder's tests */
struct JbigAtMove {
	u32 y;                          /* the first line it applies to (in
					   the page) */
	s16 tx;                         /* its lag to the left; 0 default */
};

/* how a page is coded */
struct JbigOptions {
	u32 l0 = 128;                   /* lines in a stripe (T.85's basic
					   value; others need DIS bit 79) */
	s16 options = JBG_TPBON;        /* JBG_LRLTWO, JBG_TPBON */
	s16 mx = 0;                     /* the AT pixel's largest lag */
	const JbigAtMove *moves = nullptr;
	size_t n_moves = 0;
};

/* a page as a bi-level image entity (BIE): the 20 byte header and one
   stripe data entity for each l0 lines */
void jbig_encode(const T4Page *page, const JbigOptions &opt, std::vector<unsigned char> *bie);

/* a BIE to a page of its own width, at most max_rows lines; 0 when
   whole, 1 when the data ended (or ABORT came) early with the lines
   decoded so far, -1 when it is no T.85 BIE */
s16 jbig_decode(const unsigned char *data, size_t len, s32 max_rows, T4Page *page);

} // namespace dsp

#endif /* __JBIG_H__ */
