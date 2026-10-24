/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    run_loop.h - the main loop of a modem program (dmodemd, the
 *                 modem_test pairs): until SIGINT, SIGTERM or stop(),
 *                 each turn the modems' work due, then select() on the
 *                 descriptors the turn watches until their next timer
 *
 */
#ifndef DMODEMD_RUN_LOOP_H
#define DMODEMD_RUN_LOOP_H

#include <csignal>

#include "dm_types.h"

#ifdef _WIN32
/* (Windows: select() on sockets alone, a descriptor here a SOCKET) */
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

/* a descriptor as fd_set's macros take it: Windows' a SOCKET (its
   descriptors are sockets, ints here: portable.h) */
#ifdef _WIN32
#define DMODEM_FD_SOCKET(fd) ((SOCKET)(fd))
#else
#define DMODEM_FD_SOCKET(fd) (fd)
#endif

namespace dcore { class Modem; }

namespace dmd {

class RunLoop {
public:
	/* SIGINT and SIGTERM stop() it */
	static void catch_signals();
	/* the loop ends at its next turn (safe in a signal handler) */
	static void stop() { running_ = 0; }
	/* the loop goes on (no stop() yet) */
	static bool running() { return running_; }

	/* the n modems' work due (poll()): the ms to the first of their
	   next timers, 1000 at most */
	static s32 poll(dcore::Modem *const *m, s16 n);

	/* this turn's descriptors: none, then each to watch (to read
	   from, or to write to) */
	void clear();
	void watch(int fd);
	void watch_write(int fd);
	/* fd is readable, writable (after wait()) */
	bool ready(int fd) const {
		return fd >= 0 && FD_ISSET(DMODEM_FD_SOCKET(fd), (fd_set *)&set_);
	}
	/* (writable) */
	bool ready_write(int fd) const {
		return fd >= 0 && FD_ISSET(DMODEM_FD_SOCKET(fd), (fd_set *)&wset_);
	}
	/* select() on them for ms: > 0 some are ready, 0 none (or a
	   signal), < 0 an error (errno) */
	int wait(s32 ms);

private:
	static volatile sig_atomic_t running_;
	fd_set set_, wset_;
	int max_ = -1;
};

} // namespace dmd

#endif
