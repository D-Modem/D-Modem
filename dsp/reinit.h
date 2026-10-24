/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    reinit.h - an object back to its value-initialised state, in place;
 *               a buffer allocated without an exception
 *
 */

#ifndef DSP_REINIT_H
#define DSP_REINIT_H

#include <cstddef>
#include <new>

namespace dsp {

/* o back to a value-initialised T, in place: its plain fields 0 (as a
   memset() of it), its containers empty, its default member values
   set.  No copy of it goes on the stack, which a large one (V90Rx is
   450 kB) would overflow.  o must be a whole object of type T, not
   the base of another */
template <class T>
inline void reinit(T &o)
{
	o.~T();
	::new (static_cast<void *>(&o)) T();
}

/* v as n value-initialised elements (zeros): false when out of memory,
   for the init functions that report it rather than throw through the
   C interface */
template <class V>
inline bool alloc_zeroed(V &v, std::size_t n) noexcept
{
	try {
		v.assign(n, typename V::value_type());
		return true;
	} catch (const std::bad_alloc &) {
		return false;
	}
}

/* v resized to n, its new elements value-initialised (zeros) and the
   old ones kept: false when out of memory (v as it was) */
template <class V>
inline bool resize_zeroed(V &v, std::size_t n) noexcept
{
	try {
		v.resize(n);
		return true;
	} catch (const std::bad_alloc &) {
		return false;
	}
}

/* v empty, its memory given back (an assignment of {} keeps it) */
template <class V>
inline void free_buffer(V &v) noexcept
{
	V().swap(v);
}

} // namespace dsp

#endif
