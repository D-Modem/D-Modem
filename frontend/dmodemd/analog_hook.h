/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    analog_hook.h - what an analogue line's Host (AlsaLine, UsbLine)
 *                    shares, a mixin on the host H (CRTP): the hook's
 *                    state, the clock of the line's samples, and hook
 *                    flashes and pulse dialling timed on that clock.
 *
 *    H gives its log lines' prefix, static const char LOG_TAG[], and
 *    its hook switch:
 *      void hw_hook(bool off)        the switch off hook or on
 *      s16 hw_pulse(bool broken)     pulse dialling's break (broken) or
 *                                    make; 0, or -1 if it cannot
 *    and may have:
 *      bool hw_flash(u16 ms)         a flash it makes itself (or cannot
 *                                    make): true, nothing timed here
 *
 */
#ifndef DMODEMD_ANALOG_HOOK_H
#define DMODEMD_ANALOG_HOOK_H

#include <dcore.h>
#include <log.h>

#include "dm_types.h"

namespace dmd {

template <class H>
class AnalogHook : public dcore::Host {
public:
	/* see dcore.h: on hook for ms, then off again (if it still is) */
	void hook_flash(dcore::Modem &, u16 ms) override
	{
		if (!off_hook())
			return;
		dcore::log_printf(1, "%s: hook flash, %u ms\n", H::LOG_TAG,
				  ms);
		if (self().hw_flash(ms))
			return;
		self().hw_hook(false);
		flash_end_ = samples_ + static_cast<u64>(ms) * rate_ / 1000;
	}
	/* see dcore.h: only off hook */
	s16 pulse_hook(dcore::Modem &, bool broken) override
	{
		if (!off_hook())
			return -1;
		flash_end_ = 0;
		return self().hw_pulse(broken);
	}
	/* the clock: the line's samples so far, in ms */
	u64 now_ms() override { return samples_ * 1000 / rate_; }

protected:
	~AnalogHook() = default;

	/* off hook, as the core has it */
	bool off_hook() const { return hook_ == MODEM_HOOK_OFF; }
	/* the core's hook state (set_hook()): a flash under way forgotten */
	void hook_state(s16 state)
	{
		hook_ = state;
		flash_end_ = 0;
	}
	/* a flash over by now: off hook again (before the frame at
	   samples_) */
	void flash_expire()
	{
		if (flash_end_ && samples_ >= flash_end_) {
			flash_end_ = 0;
			if (off_hook())
				self().hw_hook(true);
		}
	}
	/* (H's) the flash timed here */
	bool hw_flash(u16 ms)
	{
		(void)ms;
		return false;
	}

	s16 hook_ = MODEM_HOOK_ON;
	u64 samples_ = 0;               /* the line's, so far */
	u32 rate_ = 9600;

private:
	/* the host */
	H &self() { return static_cast<H &>(*this); }

	u64 flash_end_ = 0;             /* samples_ the flash ends at, or 0 */
};

} // namespace dmd

#endif
