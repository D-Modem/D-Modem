/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    reinit.h - an object back to its value-initialised state, in place;
 *               a buffer allocated without an exception; plain arrays
 *               and records zeroed and copied with their types checked
 *
 */

#ifndef DSP_REINIT_H
#define DSP_REINIT_H

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <new>
#include <type_traits>

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

/* o all zeros, as a memset() of it: o is a plain record or number (no
   constructor, no default member values, not a pointer) */
template <class T>
inline void zero(T &o)
{
	static_assert(std::is_trivially_default_constructible<T>::value &&
			      std::is_trivially_copyable<T>::value &&
			      !std::is_pointer<T>::value,
		      "zero() is for plain records and numbers");
	o = T();
}

/* every element of a all zeros (see zero(T &)), arrays of arrays too */
template <class T, std::size_t N>
inline void zero(T (&a)[N])
{
	if constexpr (std::is_array<T>::value) {
		for (auto &x : a)
			zero(x);
	} else {
		static_assert(
			std::is_trivially_default_constructible<T>::value &&
				std::is_trivially_copyable<T>::value,
			"zero() is for arrays of plain records and numbers");
		std::fill(std::begin(a), std::end(a), T());
	}
}

/* d a copy of s: arrays of one type and size, arrays of arrays too */
template <class T, std::size_t N>
inline void copy_array(T (&d)[N], const T (&s)[N])
{
	if constexpr (std::is_array<T>::value) {
		for (std::size_t i = 0; i < N; i++)
			copy_array(d[i], s[i]);
	} else {
		std::copy(std::begin(s), std::end(s), d);
	}
}

} // namespace dsp

#endif
