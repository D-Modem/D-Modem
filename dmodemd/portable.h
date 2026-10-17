/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    portable.h - what the modem's programs and libraries need of the
 *                 system that Linux has its own way and Apple's systems
 *                 (macOS, iOS: an app with the modem in it) another:
 *                 descriptors closed on exec from the start, sends that
 *                 don't raise SIGPIPE, a sleep until a monotonic time,
 *                 a peer's hang-up seen without reading
 *
 *    DMODEM_PORTABLE_FALLBACKS: the other way on Linux too (its tests).
 *
 */
#ifndef DMODEMD_PORTABLE_H
#define DMODEMD_PORTABLE_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(DMODEM_PORTABLE_FALLBACKS) || \
	!defined(SOCK_CLOEXEC) || !defined(MSG_NOSIGNAL)
#define DMODEM_PORTABLE_FALLBACK 1
#endif

namespace dmd {

#ifdef DMODEM_PORTABLE_FALLBACK
/* (SIGPIPE: the sockets made here don't raise it, SO_NOSIGPIPE, where
   there is that; a program ignores it where there isn't) */
const int SEND_NOSIGNAL = 0;

/* fd closed on exec and, where it can be, its sends without SIGPIPE */
inline int cloexec(int fd)
{
	if (fd >= 0) {
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
		int on = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
	}
	return fd;
}
#else
const int SEND_NOSIGNAL = MSG_NOSIGNAL;
#endif

/* socket(2), closed on exec */
inline int socket_cloexec(int domain, int type, int protocol)
{
#ifdef DMODEM_PORTABLE_FALLBACK
	return cloexec(socket(domain, type, protocol));
#else
	return socket(domain, type | SOCK_CLOEXEC, protocol);
#endif
}

/* socketpair(2), both ends closed on exec */
inline int socketpair_cloexec(int domain, int type, int protocol, int sv[2])
{
#ifdef DMODEM_PORTABLE_FALLBACK
	if (socketpair(domain, type, protocol, sv) < 0)
		return -1;
	cloexec(sv[0]);
	cloexec(sv[1]);
	return 0;
#else
	return socketpair(domain, type | SOCK_CLOEXEC, protocol, sv);
#endif
}

/* asleep until CLOCK_MONOTONIC reads *at (EINTR: again) */
inline void sleep_until(const struct timespec *at)
{
#ifdef DMODEM_PORTABLE_FALLBACK
	for (;;) {
		struct timespec now, left;
		clock_gettime(CLOCK_MONOTONIC, &now);
		left.tv_sec = at->tv_sec - now.tv_sec;
		left.tv_nsec = at->tv_nsec - now.tv_nsec;
		if (left.tv_nsec < 0) {
			left.tv_nsec += 1000000000L;
			left.tv_sec--;
		}
		if (left.tv_sec < 0)
			return;
		if (nanosleep(&left, nullptr) == 0)
			return;
		if (errno != EINTR)
			return;
	}
#else
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, at,
			       nullptr) == EINTR)
		;
#endif
}

/* the other end of the stream socket fd has closed it (seen without
   taking anything from it) */
inline bool peer_closed(int fd)
{
#ifdef DMODEM_PORTABLE_FALLBACK
	struct pollfd p = { fd, POLLIN, 0 };
	char c;
	if (poll(&p, 1, 0) <= 0)
		return false;
	if (p.revents & (POLLHUP | POLLERR | POLLNVAL))
		return true;
	/* (readable: its end, or data, which stays) */
	return recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
#else
	struct pollfd p = { fd, POLLRDHUP, 0 };
	return poll(&p, 1, 0) > 0 &&
		(p.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL));
#endif
}

} // namespace dmd

#endif
