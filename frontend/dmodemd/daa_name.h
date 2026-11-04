/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    daa_name.h - a country's name as the DAA tables (si3054.cpp,
 *                 si3056.cpp, cx_country.cpp) have it, and the lookup
 *                 of a row by it
 *
 */
#ifndef DMODEMD_DAA_NAME_H
#define DMODEMD_DAA_NAME_H

#include <cctype>
#include <cstddef>

namespace dmd {

/* a and b the same country's name, case and '_' (or ' ') ignored
   (NEW_ZEALAND, "New Zealand") */
inline bool daa_same_name(const char *a, const char *b)
{
	for (;;) {
		while (*a == '_' || *a == ' ')
			a++;
		while (*b == '_' || *b == ' ')
			b++;
		if (!*a || !*b)
			return !*a && !*b;
		if (toupper(static_cast<unsigned char>(*a++)) !=
		    toupper(static_cast<unsigned char>(*b++)))
			return false;
	}
}

/* the row of table whose name is name (daa_same_name()), or nullptr:
   none, or name nullptr */
template <class Row, std::size_t N>
inline const Row *daa_find(const Row (&table)[N], const char *name)
{
	if (!name)
		return nullptr;
	for (const Row &r : table)
		if (daa_same_name(r.name, name))
			return &r;
	return nullptr;
}

} // namespace dmd

#endif
