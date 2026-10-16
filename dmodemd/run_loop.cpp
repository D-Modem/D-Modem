/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    run_loop.cpp - a modem program's main loop (run_loop.h)
 *
 */

#include <errno.h>

#include <dcore.h>

#include "run_loop.h"

namespace dmd {

volatile sig_atomic_t RunLoop::running_ = 1;

/* see run_loop.h */
void RunLoop::catch_signals()
{
	signal(SIGINT, [](int) { stop(); });
	signal(SIGTERM, [](int) { stop(); });
}

/* see run_loop.h */
long RunLoop::poll(dcore::Modem *const *m, int n)
{
	long next = 1000;
	for (int i = 0; i < n; i++) {
		long t;
		m[i]->poll();
		t = m[i]->next_timer_ms();
		if (t >= 0 && t < next)
			next = t;
	}
	return next;
}

/* see run_loop.h */
void RunLoop::clear()
{
	FD_ZERO(&set_);
	FD_ZERO(&wset_);
	max_ = -1;
}

/* see run_loop.h */
void RunLoop::watch(int fd)
{
	FD_SET(fd, &set_);
	if (fd > max_)
		max_ = fd;
}

/* see run_loop.h */
void RunLoop::watch_write(int fd)
{
	FD_SET(fd, &wset_);
	if (fd > max_)
		max_ = fd;
}

/* see run_loop.h */
int RunLoop::wait(long ms)
{
	struct timeval tmo;
	int ret;

	tmo.tv_sec = ms / 1000;
	tmo.tv_usec = ms % 1000 * 1000;
	ret = select(max_ + 1, &set_, &wset_, NULL, &tmo);
	if (ret < 0 && errno == EINTR)
		return 0;
	return ret;
}

} // namespace dmd
