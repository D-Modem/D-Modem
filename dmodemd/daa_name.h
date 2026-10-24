/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    daa_name.h - a country's name as the DAA tables (si3054.cpp,
 *                 si3056.cpp) have it
 *
 */
#ifndef DMODEMD_DAA_NAME_H
#define DMODEMD_DAA_NAME_H

#include <cctype>

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

} // namespace dmd

#endif
