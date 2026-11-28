/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t45.cpp - ITU-T T.45 run-length colour encoding
 *
 * The data is a 6-octet header (NCOMP: the components of a value,
 * COMPLEN: the octets of a component, NVALS: the values, 4 octets) and
 * then runs: RUNLEN (1 octet 1 to 255, or 0x00 and 2 octets) and the
 * value (CVAL: NCOMP components of COMPLEN octets).  The runs end when
 * their RUNLENs add up to NVALS.  All numbers are big-endian.
 *
 */

#include <string.h>

#include "t45.h"

namespace dsp {

namespace {

/* a big-endian number of n octets (1, 2 or 4) appended */
void put_be(std::vector<unsigned char> *out, u32 v, s16 n)
{
	for (s16 k = n - 1; k >= 0; k--)
		out->push_back((unsigned char)(v >> (8 * k)));
}

/* a big-endian number of n octets at p */
u32 get_be(const unsigned char *p, s16 n)
{
	u32 v = 0;
	for (s16 k = 0; k < n; k++)
		v = v << 8 | p[k];
	return v;
}

/* the value i of v is the same as value j */
bool same(const T45Values &v, size_t i, size_t j)
{
	const size_t n = (size_t)v.ncomp;
	return memcmp(&v.vals[i * n], &v.vals[j * n], n * sizeof(u32)) == 0;
}

} // namespace

/* (t45.h) the header, then a run for each group of equal values that
   follow each other (at most 65535 in a run) */
void t45_encode(const T45Values &v, std::vector<unsigned char> *out)
{
	const s16 nc = v.ncomp > 0 ? v.ncomp : 1;
	const s16 cl = v.complen == 2 || v.complen == 4 ? v.complen : 1;
	const size_t n = v.ncomp > 0 ? v.vals.size() / (size_t)nc : 0;
	out->push_back((unsigned char)nc);
	out->push_back((unsigned char)cl);
	put_be(out, (u32)n, 4);
	for (size_t i = 0; i < n;) {
		size_t run = 1;
		while (i + run < n && run < 65535 && same(v, i, i + run))
			run++;
		if (run <= 255) {
			out->push_back((unsigned char)run);
		} else {
			out->push_back(0x00);
			put_be(out, (u32)run, 2);
		}
		for (s16 c = 0; c < nc; c++)
			put_be(out, v.vals[i * nc + c], cl);
		i += run;
	}
}

/* (t45.h) the header, then the runs until NVALS values */
s16 t45_decode(const unsigned char *data, size_t len, T45Values *v, size_t *used, size_t max_vals)
{
	v->vals.clear();
	if (len < 6)
		return -1;
	const s16 nc = data[0], cl = data[1];
	const u32 nvals = get_be(data + 2, 4);
	if (nc < 1 || (cl != 1 && cl != 2 && cl != 4) || nvals > max_vals ||
	    (u64)nvals * (u64)nc > (u64)max_vals * 4)
		return -1;
	v->ncomp = nc;
	v->complen = cl;
	const size_t cval = (size_t)nc * cl;
	size_t pos = 6, have = 0;
	while (have < nvals) {
		if (pos >= len)
			return -1;
		u32 run = data[pos++];
		if (run == 0) {
			if (pos + 2 > len)
				return -1;
			run = get_be(data + pos, 2);
			pos += 2;
		}
		if (pos + cval > len || run > nvals - have)
			return -1;
		u32 c[255];
		for (s16 k = 0; k < nc; k++)
			c[k] = get_be(data + pos + (size_t)k * cl, cl);
		pos += cval;
		for (u32 r = 0; r < run; r++)
			v->vals.insert(v->vals.end(), c, c + nc);
		have += run;
	}
	if (used)
		*used = pos;
	return 0;
}

} // namespace dsp
