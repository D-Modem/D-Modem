/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    backlog.h - the received characters an error-correcting protocol
 *                (LAPM, X.75 / V.120, MNP) could not hand the DTE: held
 *                until it takes them, while the protocol tells the
 *                remote it is busy (its own way: RNR, no credit)
 *
 */

#ifndef DLINK_BACKLOG_H
#define DLINK_BACKLOG_H

#include <cstring>

#include "dm_types.h"

namespace dlink {

/* up to N characters held.  L is the Link (link.h), which has
   write_chars() and push_pending */
template <s16 N>
class DteBacklog {
public:
	/* nothing held */
	void clear() { count_ = head_ = 0; }
	/* (see clear()) */
	bool empty() const { return !count_; }

	/* n characters of d to the DTE through link: those it does not
	   take held, and the link told to push them (push()) later.
	   Whether some are held */
	template <class L>
	bool deliver(L *link, const u8 *d, s16 n)
	{
		s16 taken = link->write_chars(d, n);
		if (taken < 0)
			taken = 0;
		if (taken >= n)
			return false;
		head_ = 0;
		count_ = static_cast<s16>(n - taken);
		memcpy(buf_, d + taken, count_);
		link->push_pending = true;
		return true;
	}

	/* the held characters to the DTE again: whether they have all
	   gone now (the link no longer to push them) */
	template <class L>
	bool push(L *link)
	{
		s16 taken = link->write_chars(buf_ + head_, count_);
		if (taken > 0) {
			head_ = static_cast<s16>(head_ + taken);
			count_ = static_cast<s16>(count_ - taken);
		}
		if (count_)
			return false;
		head_ = 0;
		link->push_pending = false;
		return true;
	}

private:
	s16 count_ = 0, head_ = 0;
	u8 buf_[N];
};

} // namespace dlink

#endif
