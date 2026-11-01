/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fake_net.h - the fake USB modems' connections to line_exchange
 *                 (fake_st7554.cpp, fake_pico_line.cpp), on POSIX and on
 *                 Windows: the exchange's socket of the line by its path
 *                 (a Unix socket), or "tcp:<host>:<port>" (Windows, whose
 *                 Winsock under Wine has no Unix sockets: the tests relay
 *                 TCP to the exchange's socket)
 *
 */
#ifndef DMODEMD_TEST_FAKE_NET_H
#define DMODEMD_TEST_FAKE_NET_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <string>

/* (no MSG_NOSIGNAL: SO_NOSIGPIPE on each socket instead, no_sigpipe()) */
#if !defined(_WIN32) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

namespace fake_net {

/* fd's sends without SIGPIPE where the system has SO_NOSIGPIPE (Apple's,
   which may have no MSG_NOSIGNAL) */
inline void no_sigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
	(void)fd;
#endif
}

/* a TCP connection to "<host>:<port>"; -1 */
inline int connect_tcp(const char *where)
{
	std::string s = where;
	size_t colon = s.rfind(':');
	struct addrinfo hints = {}, *ai = nullptr;
	int fd = -1;

	if (colon == std::string::npos)
		return -1;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(s.substr(0, colon).c_str(), s.c_str() + colon + 1,
			&hints, &ai) || !ai)
		return -1;
	fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (fd >= 0 && connect(fd, ai->ai_addr, (int)ai->ai_addrlen) != 0) {
#ifdef _WIN32
		closesocket(fd);
#else
		close(fd);
#endif
		fd = -1;
	}
	freeaddrinfo(ai);
	if (fd >= 0) {
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one,
			   sizeof(one));
		no_sigpipe(fd);
	}
	return fd;
}

/* fd closed */
inline void close_fd(int fd)
{
	if (fd < 0)
		return;
#ifdef _WIN32
	closesocket(fd);
#else
	close(fd);
#endif
}

/* n octets of p to fd, all of them: false if not */
inline bool send_all(int fd, const void *p, size_t n)
{
	const char *b = (const char *)p;
	while (n > 0) {
#ifdef _WIN32
		int k = send(fd, b, (int)n, 0);
#else
		ssize_t k = send(fd, b, n, MSG_NOSIGNAL);
#endif
		if (k <= 0)
			return false;
		b += k;
		n -= (size_t)k;
	}
	return true;
}

/* what fd has now, without waiting: > 0 octets, 0 at its end, -1 if
   nothing is there (or an error) */
inline long recv_now(int fd, void *buf, size_t n)
{
#ifdef _WIN32
	fd_set r;
	struct timeval zero = { 0, 0 };
	FD_ZERO(&r);
	FD_SET((SOCKET)fd, &r);
	if (select(0, &r, nullptr, nullptr, &zero) != 1)
		return -1;
	int k = recv(fd, (char *)buf, (int)n, 0);
	return k < 0 ? -1 : k;
#else
	ssize_t k = recv(fd, buf, n, MSG_DONTWAIT);
	return k < 0 ? -1 : (long)k;
#endif
}

/* a connection to the exchange at where (a Unix socket's path, or
   tcp:<host>:<port>), its first octet kind; -1 */
inline int connect_exchange(const char *where, char kind)
{
	int fd;
#ifdef _WIN32
	static bool started;
	if (!started) {
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
		started = true;
	}
#endif
	if (!strncmp(where, "tcp:", 4)) {
		fd = connect_tcp(where + 4);
	} else {
#ifdef _WIN32
		return -1;
#else
		struct sockaddr_un a = {};
		/* (closed on exec: fcntl(), as Haiku has no SOCK_CLOEXEC) */
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
			return -1;
		no_sigpipe(fd);
		a.sun_family = AF_UNIX;
		strncpy(a.sun_path, where, sizeof(a.sun_path) - 1);
		if (connect(fd, reinterpret_cast<struct sockaddr *>(&a),
			    sizeof(a)) < 0) {
			close(fd);
			return -1;
		}
#endif
	}
	if (fd < 0)
		return -1;
	if (!send_all(fd, &kind, 1)) {
		close_fd(fd);
		return -1;
	}
	return fd;
}

} // namespace fake_net

#endif
