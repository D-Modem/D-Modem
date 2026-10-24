/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    run_loop.cpp - a modem program's main loop (run_loop.h)
 *
 */

#include <cerrno>

#include <dcore.h>

#include "dm_types.h"
#include "run_loop.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace dmd {

volatile sig_atomic_t RunLoop::running_ = 1;

/* see run_loop.h */
void RunLoop::catch_signals()
{
	signal(SIGINT, [](int) { stop(); });
	signal(SIGTERM, [](int) { stop(); });
#ifdef _WIN32
	/* (Windows: Ctrl+C, Ctrl+Break, the console closed, the user
	   logging off or the system shutting down stop it too) */
	SetConsoleCtrlHandler([](DWORD) -> BOOL {
		stop();
		return TRUE;
	}, TRUE);
#endif
}

/* see run_loop.h */
s32 RunLoop::poll(dcore::Modem *const *m, s16 n)
{
	s32 next = 1000;
	for (s16 i = 0; i < n; i++) {
		s64 t;
		m[i]->poll();
		t = m[i]->next_timer_ms();
		if (t >= 0 && t < next)
			next = static_cast<s32>(t);
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

/* see run_loop.h (Windows' FD_SET takes a SOCKET: portable.h's sockets
   are ints) */
void RunLoop::watch(int fd)
{
	FD_SET(DMODEM_FD_SOCKET(fd), &set_);
	if (fd > max_)
		max_ = fd;
}

/* see run_loop.h */
void RunLoop::watch_write(int fd)
{
	FD_SET(DMODEM_FD_SOCKET(fd), &wset_);
	if (fd > max_)
		max_ = fd;
}

/* see run_loop.h */
int RunLoop::wait(s32 ms)
{
	struct timeval tmo;
	int ret;

	tmo.tv_sec = ms / 1000;
	tmo.tv_usec = ms % 1000 * 1000;
#ifdef _WIN32
	/* (Winsock's select(): an error if no socket is watched; a sleep
	   then) */
	if (set_.fd_count == 0 && wset_.fd_count == 0) {
		Sleep((DWORD)ms);
		return 0;
	}
	ret = select(0, &set_, &wset_, nullptr, &tmo);
	if (ret == SOCKET_ERROR) {
		errno = WSAGetLastError() == WSAEINTR ? EINTR : EIO;
		return errno == EINTR ? 0 : -1;
	}
	return ret;
#else
	ret = select(max_ + 1, &set_, &wset_, nullptr, &tmo);
	if (ret < 0 && errno == EINTR)
		return 0;
	return ret;
#endif
}

} // namespace dmd
