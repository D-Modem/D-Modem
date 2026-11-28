/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * compat.h - libre on the classic Mac OS (the MacOS9 system of
 * D-Modem/gui's cmake/macos9.cmake), over the C library of
 * clang-retro/mac-sdk and its sockets on MacTCP. cmake/libre.cmake
 * includes this file before each source of libre (-include).
 *
 * The C library has the types of IPv6 but no IPv6 network, and no
 * multicast. This file gives these names, so that libre compiles:
 *
 * - the IN6_IS_ADDR_* tests and IN_MULTICAST, as in the BSD headers.
 * - struct ip_mreq and struct ipv6_mreq, and the socket options of
 *   multicast. setsockopt() fails with them, so a multicast join fails.
 * - pthread_setname_np(): it does nothing. The threads are the
 *   cooperative threads of the C library and have no names.
 * - getrlimit(): the C library declares it but does not have it. The
 *   limit of RLIMIT_NOFILE is FD_SETSIZE, the limit of select().
 */
#ifndef DMODEM_LIBRE_MACOS9_COMPAT_H
#define DMODEM_LIBRE_MACOS9_COMPAT_H

#include <sys/types.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <errno.h>

#define IN6_IS_ADDR_UNSPECIFIED(a) \
	((a)->__in6_u.__s6_addr32[0] == 0 && (a)->__in6_u.__s6_addr32[1] == 0 && \
	 (a)->__in6_u.__s6_addr32[2] == 0 && (a)->__in6_u.__s6_addr32[3] == 0)
#define IN6_IS_ADDR_LOOPBACK(a) \
	((a)->__in6_u.__s6_addr32[0] == 0 && (a)->__in6_u.__s6_addr32[1] == 0 && \
	 (a)->__in6_u.__s6_addr32[2] == 0 && (a)->__in6_u.__s6_addr32[3] == htonl(1))
#define IN6_IS_ADDR_V4MAPPED(a) \
	((a)->__in6_u.__s6_addr32[0] == 0 && (a)->__in6_u.__s6_addr32[1] == 0 && \
	 (a)->__in6_u.__s6_addr32[2] == htonl(0xffff))
#define IN6_IS_ADDR_MULTICAST(a) ((a)->s6_addr[0] == 0xff)
#define IN6_IS_ADDR_LINKLOCAL(a) \
	((a)->s6_addr[0] == 0xfe && ((a)->s6_addr[1] & 0xc0) == 0x80)
#define IN6_IS_ADDR_SITELOCAL(a) \
	((a)->s6_addr[0] == 0xfe && ((a)->s6_addr[1] & 0xc0) == 0xc0)
#define IN_MULTICAST(a) (((in_addr_t)(a) & 0xf0000000) == 0xe0000000)

/* A multicast group of IPv4: the group and the interface */
struct ip_mreq {
	struct in_addr imr_multiaddr;
	struct in_addr imr_interface;
};

/* A multicast group of IPv6: the group and the interface index */
struct ipv6_mreq {
	struct in6_addr ipv6mr_multiaddr;
	unsigned int ipv6mr_interface;
};

#define IP_ADD_MEMBERSHIP 12
#define IP_DROP_MEMBERSHIP 13
#define IPV6_JOIN_GROUP 20
#define IPV6_LEAVE_GROUP 21

/* Gives a name to a thread: nothing on the classic Mac OS */
#define pthread_setname_np(thread, name) ((void)(thread), (void)(name), 0)

/* Gets a limit of the program: RLIMIT_NOFILE only, as FD_SETSIZE. 0,
   or -1 with errno EINVAL for the other limits. */
static inline int dmodem_macos9_getrlimit(int resource, struct rlimit *rl)
{
	if (resource != RLIMIT_NOFILE) {
		errno = EINVAL;
		return -1;
	}
	rl->rlim_cur = FD_SETSIZE;
	rl->rlim_max = FD_SETSIZE;
	return 0;
}
#define getrlimit(resource, rl) dmodem_macos9_getrlimit(resource, rl)

#endif
