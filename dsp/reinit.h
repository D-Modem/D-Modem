/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    reinit.h - an object back to its value-initialised state, in place
 *
 */

#ifndef DSP_REINIT_H
#define DSP_REINIT_H

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

} // namespace dsp

#endif
