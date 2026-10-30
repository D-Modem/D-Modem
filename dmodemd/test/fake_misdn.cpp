/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fake_misdn.cpp - an mISDN card for misdn_test.py, in front of the C
 *                     library (LD_PRELOAD): dmodemd's PF_ISDN sockets
 *
 *    The base socket (ISDN_P_BASE) answers the device list's ioctls
 *    with one device, 0, an S0 terminal interface with two B-channels,
 *    named $FAKE_MISDN_NAME (fake-hfc.1).  Every other PF_ISDN socket is
 *    a SOCK_SEQPACKET connection to the test's network at the Unix
 *    socket $FAKE_MISDN, each of its messages one of mISDN's (struct
 *    mISDNhead and the data), both ways, as they are; bind() sends the
 *    network a message of its own first: the primitive 0xFFFF0000, the
 *    id the protocol << 16 | the device << 8 | the channel, then the
 *    terminal's name ($FAKE_MISDN_TERMINAL).
 *
 */

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

const int PF_MISDN = 34;
const int ISDN_P_BASE = 0;
const int MAX_FD = 4096;
const uint32_t BIND_PRIM = 0xFFFF0000u;
/* (_IOR('I', 67, int), _IOR('I', 68, int)) */
const unsigned long IMGETCOUNT = 0x80044943ul;
const unsigned long IMGETDEVINFO = 0x80044944ul;

struct DevInfo {
	unsigned int id;
	unsigned int Dprotocols;
	unsigned int Bprotocols;
	unsigned int protocol;
	unsigned char channelmap[16];
	unsigned int nrbchan;
	char name[20];
};

/* each descriptor's: 0 not ours, -1 a base socket, else 1 + the
   protocol */
int kind[MAX_FD];

template <class F>
F real(const char *name)
{
	return reinterpret_cast<F>(dlsym(RTLD_NEXT, name));
}

bool ours(int fd)
{
	return fd >= 0 && fd < MAX_FD && kind[fd] != 0;
}

} // namespace

extern "C" {

int socket(int domain, int type, int protocol)
{
	static auto next = real<int (*)(int, int, int)>("socket");
	if (domain != PF_MISDN)
		return next(domain, type, protocol);
	const int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
	int fd;
	if (protocol == ISDN_P_BASE) {
		int sv[2];
		if (socketpair(AF_UNIX, SOCK_STREAM | flags, 0, sv) < 0)
			return -1;
		close(sv[1]);
		fd = sv[0];
	} else {
		const char *path = getenv("FAKE_MISDN");
		if (!path) {
			errno = EAFNOSUPPORT;
			return -1;
		}
		fd = next(AF_UNIX, SOCK_SEQPACKET | flags, 0);
		if (fd < 0)
			return -1;
		struct sockaddr_un a;
		memset(&a, 0, sizeof(a));
		a.sun_family = AF_UNIX;
		strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
		if (connect(fd, reinterpret_cast<struct sockaddr *>(&a),
			    sizeof(a)) < 0) {
			int e = errno;
			close(fd);
			errno = e;
			return -1;
		}
	}
	if (fd >= MAX_FD) {
		close(fd);
		errno = EMFILE;
		return -1;
	}
	kind[fd] = protocol == ISDN_P_BASE ? -1 : 1 + protocol;
	return fd;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
	static auto next = real<int (*)(int, const struct sockaddr *,
				       socklen_t)>("bind");
	if (!ours(fd))
		return next(fd, addr, len);
	if (kind[fd] < 0 || len < 6) {
		errno = EINVAL;
		return -1;
	}
	const unsigned char *a = reinterpret_cast<const unsigned char *>(addr);
	const uint32_t id = static_cast<uint32_t>(kind[fd] - 1) << 16 |
		static_cast<uint32_t>(a[2]) << 8 | a[3];
	const char *term = getenv("FAKE_MISDN_TERMINAL");
	if (!term)
		term = "";
	unsigned char msg[8 + 64];
	size_t n = strlen(term) < 64 ? strlen(term) : 64;
	memcpy(msg, &BIND_PRIM, 4);
	memcpy(msg + 4, &id, 4);
	memcpy(msg + 8, term, n);
	return send(fd, msg, 8 + n, 0) < 0 ? -1 : 0;
}

#ifdef __GLIBC__
int ioctl(int fd, unsigned long req, ...)
#else
int ioctl(int fd, int req, ...)
#endif
{
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (!ours(fd) || kind[fd] > 0) {
#ifdef __GLIBC__
		static auto next = real<int (*)(int, unsigned long, ...)>(
			"ioctl");
#else
		static auto next = real<int (*)(int, int, ...)>("ioctl");
#endif
		return next(fd, req, arg);
	}
	const unsigned long r = static_cast<unsigned long>(req) & 0xFFFFFFFFul;
	if (r == IMGETCOUNT) {
		*static_cast<int *>(arg) = 1;
		return 0;
	}
	if (r == IMGETDEVINFO) {
		DevInfo *d = static_cast<DevInfo *>(arg);
		if (d->id != 0) {
			errno = ENODEV;
			return -1;
		}
		memset(d, 0, sizeof(*d));
		d->Dprotocols = 1u << 1;        /* ISDN_P_TE_S0 */
		d->Bprotocols = 1u << 1 | 1u << 2;
		d->channelmap[0] = 0x06;
		d->nrbchan = 2;
		const char *name = getenv("FAKE_MISDN_NAME");
		strncpy(d->name, name ? name : "fake-hfc.1",
			sizeof(d->name) - 1);
		return 0;
	}
	errno = EINVAL;
	return -1;
}

int close(int fd)
{
	static auto next = real<int (*)(int)>("close");
	if (fd >= 0 && fd < MAX_FD)
		kind[fd] = 0;
	return next(fd);
}

} // extern "C"
