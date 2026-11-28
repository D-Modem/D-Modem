/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * net/if.h - the network interfaces for libre on the classic Mac OS
 * (cmake/libre-macos9/compat.h tells more). The C library of
 * clang-retro/mac-sdk has no net/if.h and no interface ioctls. This file
 * gives the names that libre's src/net/posix/pif.c uses. The ioctls fail,
 * so libre lists no interfaces.
 */
#ifndef DMODEM_LIBRE_MACOS9_NET_IF_H
#define DMODEM_LIBRE_MACOS9_NET_IF_H

#include <sys/types.h>
#include <sys/socket.h>

#define IFNAMSIZ 16
#define IFF_UP 0x1
#define IFF_LOOPBACK 0x8

/* One interface: its name, and its address or its flags */
struct ifreq {
	char ifr_name[IFNAMSIZ];
	union {
		struct sockaddr ifru_addr;
		short ifru_flags;
	} ifr_ifru;
};
#define ifr_addr ifr_ifru.ifru_addr
#define ifr_flags ifr_ifru.ifru_flags

/* The list of the interfaces (SIOCGIFCONF) */
struct ifconf {
	int ifc_len;
	union {
		char *ifcu_buf;
		struct ifreq *ifcu_req;
	} ifc_ifcu;
};
#define ifc_buf ifc_ifcu.ifcu_buf
#define ifc_req ifc_ifcu.ifcu_req

#define SIOCGIFCONF 0x8912
#define SIOCGIFFLAGS 0x8913
#define SIOCGIFADDR 0x8915

#endif
