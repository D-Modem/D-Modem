/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t45.h - ITU-T T.45 run-length colour encoding: a list of colour
 *            values (or palette indices) as runs, for the colour tags
 *            of T.44 mode 4 (one colour for each symbol of a JBIG2
 *            mask)
 *
 */

#ifndef __T45_H__
#define __T45_H__

#include <cstddef>
#include <vector>

#include "dm_types.h"

namespace dsp {

/* a list of colour values: ncomp components each, of complen octets
   (1, 2 or 4); vals has ncomp numbers for each value, a value after
   another */
struct T45Values {
	s16 ncomp = 3;
	s16 complen = 1;
	std::vector<u32> vals;

	/* the number of colour values (NVALS) */
	size_t count() const { return ncomp > 0 ? vals.size() / (size_t)ncomp : 0; }
};

/* the header (NCOMP, COMPLEN, NVALS) and the runs of v appended to out.
   A run has at most 65535 values; a run of 1 to 255 has a 1-octet
   RUNLEN, a longer run the 3-octet form */
void t45_encode(const T45Values &v, std::vector<unsigned char> *out);

/* the T.45 data at data to v, at most max_vals values; *used (if
   given): its octets.  0 when whole, -1 when it is not valid (a bad
   header, a run past NVALS, more than max_vals values or 4 * max_vals
   components, the data cut short) */
s16 t45_decode(const unsigned char *data, size_t len, T45Values *v, size_t *used = nullptr,
	       size_t max_vals = (size_t)1 << 24);

} // namespace dsp

#endif /* __T45_H__ */
