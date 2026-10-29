/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    v13.cpp - V.13's simulated carrier control (v13.h)
 */
#include "v13.h"

namespace dcore {
namespace v13 {

u8 SimCarrier::tx(u8 link_bit)
{
	link_bit &= 1;
	if (on_ || !wait_.empty() || !link_bit)
		wait_.push_back(link_bit);
	if (!on_) {
		/* 105 ON with the data (after the idle pattern's least) */
		if (!wait_.empty() && idle_sent_ >= MIN_IDLE) {
			on_ = true;
			on_left_ = ON_BITS;
			ones_ = 0;
		} else {
			if (scr_.s == 0x7f)
				scr_.s = 0x7e;  /* (note 6) */
			idle_sent_++;
			return scr_.bit(1);
		}
	}
	if (on_left_ > 0) {
		on_left_--;
		return scr_.bit(0);
	}
	u8 o = 1;
	if (!wait_.empty()) {
		o = wait_.front();
		wait_.pop_front();
	}
	/* 105 OFF once the link has been idle a while (what waits is
	   its 1s) */
	ones_ = link_bit ? ones_ + 1 : 0;
	if (ones_ >= OFF_IDLE &&
	    static_cast<s32>(wait_.size()) <= OFF_IDLE) {
		wait_.clear();
		on_ = false;
		idle_sent_ = 0;
	}
	return o;
}

u8 SimCarrier::rx(u8 b)
{
	u8 d = d_.bit(b & 1);
	idle_ = d ? idle_ + 1 : 0;
	if (idle_ >= DETECT_IDLE)
		no_idle_ = 0;
	if (c109_) {
		if (idle_ >= DETECT_IDLE) {
			/* (what waits is the idle pattern, or 1s before
			   it: none of it reaches the link) */
			c109_ = false;
			zeros_ = 0;
			delay_.assign(DELAY, 1);
			return 1;
		}
		delay_.push_back(b & 1);
		u8 o = delay_.front();
		delay_.pop_front();
		return o;
	}
	/* OFF: the ON pattern after the idle pattern turns it ON */
	zeros_ = d ? 0 : zeros_ + 1;
	if (zeros_ >= ON_BITS || ++no_idle_ >= LOST_IDLE) {
		c109_ = true;
		zeros_ = 0;
		no_idle_ = 0;
	}
	return 1;
}

}  // namespace v13
}  // namespace dcore
