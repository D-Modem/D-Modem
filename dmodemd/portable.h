/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    portable.h - what the modem's programs and libraries need of the
 *                 system that Linux has its own way and Apple's systems
 *                 (macOS, iOS: an app with the modem in it) another:
 *                 descriptors closed on exec from the start, sends that
 *                 don't raise SIGPIPE, a sleep until a monotonic time,
 *                 a peer's hang-up seen without reading; and on
 *                 Windows (MinGW-w64) the DTE's stream socket: read,
 *                 written and made non-blocking with Winsock's calls
 *                 (there a socket is no file descriptor), a connected
 *                 pair of them for socketpair(2)
 *
 *    DMODEM_PORTABLE_FALLBACKS: the other way on Linux too (its tests).
 *    On Windows a descriptor (int) here is a SOCKET: Winsock's handles
 *    fit in an int.
 *
 */
#ifndef DMODEMD_PORTABLE_H
#define DMODEMD_PORTABLE_H

#include <errno.h>
#include <fcntl.h>
#include <time.h>

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace dmd {

/* Winsock started, once (the process's first use of a socket) */
inline bool net_init()
{
	static const bool ok = [] {
		WSADATA w;
		return WSAStartup(MAKEWORD(2, 2), &w) == 0;
	}();
	return ok;
}

/* the last call's error as errno has it (EAGAIN for a socket that
   would block) */
inline int sock_errno()
{
	int e = WSAGetLastError();
	return e == WSAEWOULDBLOCK ? EAGAIN : e == WSAEINTR ? EINTR :
		e == WSAECONNRESET ? ECONNRESET : EIO;
}

/* up to n bytes from the stream socket fd: as read(2) */
inline int fd_read(int fd, void *buf, int n)
{
	int r = recv((SOCKET)fd, (char *)buf, n, 0);
	if (r == SOCKET_ERROR) {
		errno = sock_errno();
		return -1;
	}
	return r;
}

/* n bytes to the stream socket fd: as write(2) */
inline int fd_write(int fd, const void *buf, int n)
{
	int r = send((SOCKET)fd, (const char *)buf, n, 0);
	if (r == SOCKET_ERROR) {
		errno = sock_errno();
		return -1;
	}
	return r;
}

/* fd's reads and writes no longer wait */
inline int set_nonblocking(int fd)
{
	u_long on = 1;
	return ioctlsocket((SOCKET)fd, FIONBIO, &on) == 0 ? 0 : -1;
}

/* the socket fd closed */
inline void fd_close(int fd)
{
	closesocket((SOCKET)fd);
}

#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif

/* the local time of t into *tm (localtime_r) */
inline void local_time(time_t t, struct tm *tm)
{
	localtime_s(tm, &t);
}

/* (sends never raise a signal on Windows; MSG_DONTWAIT: the sockets it
   is used on are made non-blocking instead, set_nonblocking()) */
const int SEND_NOSIGNAL = 0;
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

/* socket(2), not inherited by child processes */
inline int socket_cloexec(int domain, int type, int protocol)
{
	if (!net_init())
		return -1;
	SOCKET s = WSASocketW(domain, type, protocol, nullptr, 0,
			      WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
	if (s == INVALID_SOCKET) {
		errno = sock_errno();
		return -1;
	}
	return (int)s;
}

/* another descriptor for the socket fd (F_DUPFD_CLOEXEC) */
inline int fd_dup(int fd)
{
	WSAPROTOCOL_INFOW pi;
	if (WSADuplicateSocketW((SOCKET)fd, GetCurrentProcessId(), &pi))
		return -1;
	SOCKET s = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
			      FROM_PROTOCOL_INFO, &pi, 0,
			      WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
	return s == INVALID_SOCKET ? -1 : (int)s;
}

/* asleep until CLOCK_MONOTONIC reads *at */
inline void sleep_until(const struct timespec *at)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long long ms = (at->tv_sec - now.tv_sec) * 1000LL +
		(at->tv_nsec - now.tv_nsec) / 1000000;
	if (ms > 0)
		Sleep((DWORD)ms);
}

/* the other end of the stream socket fd has closed it (seen without
   taking anything from it) */
inline bool peer_closed(int fd)
{
	fd_set r;
	struct timeval t = { 0, 0 };
	char c;
	FD_ZERO(&r);
	FD_SET((SOCKET)fd, &r);
	if (select(0, &r, nullptr, nullptr, &t) <= 0)
		return false;
	/* (readable: its end, an error, or data, which stays) */
	return recv((SOCKET)fd, &c, 1, MSG_PEEK) <= 0 &&
		WSAGetLastError() != WSAEWOULDBLOCK;
}

/* two connected stream sockets, for socketpair(2): a listener on the
   loopback address takes the one connection it waits for; Nagle off,
   so that a byte goes when it is written */
inline int stream_pair(int sv[2])
{
	if (!net_init())
		return -1;
	SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	SOCKET a = INVALID_SOCKET, b = INVALID_SOCKET;
	struct sockaddr_in in = {};
	int len = sizeof(in), on = 1;
	if (l == INVALID_SOCKET)
		return -1;
	in.sin_family = AF_INET;
	in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(l, (struct sockaddr *)&in, sizeof(in)) ||
	    getsockname(l, (struct sockaddr *)&in, &len) || listen(l, 1))
		goto fail;
	a = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (a == INVALID_SOCKET ||
	    connect(a, (struct sockaddr *)&in, sizeof(in)))
		goto fail;
	b = accept(l, nullptr, nullptr);
	if (b == INVALID_SOCKET)
		goto fail;
	closesocket(l);
	setsockopt(a, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
	setsockopt(b, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
	sv[0] = (int)a;
	sv[1] = (int)b;
	return 0;
fail:
	closesocket(l);
	if (a != INVALID_SOCKET)
		closesocket(a);
	return -1;
}

/* socketpair(2) (stream sockets alone: domain and type are AF_UNIX
   and SOCK_STREAM wherever this is called on Windows) */
inline int socketpair_cloexec(int, int, int, int sv[2])
{
	return stream_pair(sv);
}

} // namespace dmd

#else /* !_WIN32 */

#include <poll.h>
#include <sys/socket.h>
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

/* as the Windows functions above, by POSIX's own calls */
inline bool net_init() { return true; }
inline int fd_read(int fd, void *buf, int n) { return (int)read(fd, buf, n); }
inline int fd_write(int fd, const void *buf, int n)
{
	return (int)write(fd, buf, n);
}
inline int set_nonblocking(int fd)
{
	return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}
inline void fd_close(int fd) { close(fd); }
inline int stream_pair(int sv[2])
{
	return socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sv);
}
inline int fd_dup(int fd) { return fcntl(fd, F_DUPFD_CLOEXEC, 0); }
inline void local_time(time_t t, struct tm *tm) { localtime_r(&t, tm); }

} // namespace dmd

#endif /* _WIN32 */

#endif
