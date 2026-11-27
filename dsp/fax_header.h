/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fax_header.h - the header line of a sent fax page: a line of text
 *                   (the date, the sender's name and number, the page
 *                   number) in black on white, in rows put above the
 *                   page's own rows, as fax machines print it
 *
 *    The font is our own 5 by 7 font of capitals, digits and the usual
 *    signs; a small letter prints as its capital, a character the font
 *    does not have as '?'.
 *
 */

#ifndef __FAX_HEADER_H__
#define __FAX_HEADER_H__

#include "dm_types.h"
#include "t4.h"

namespace dsp {

/* the rows that the header line takes: at the page's resolution */
s32 fax_header_rows(s16 fine);

/* the header line of text in new rows above the rows of p (a black and
   white page; a colour or grey page, or a file, stays as it is): 0, or
   -1 when out of memory (p as it was) */
s16 fax_header_add(T4Page *p, const char *text);

} // namespace dsp

#endif /* __FAX_HEADER_H__ */
