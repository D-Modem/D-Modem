/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    osmo_v110_peer.h - libosmocore's V.110 for v110_osmo_test.cpp
 *    (osmo_v110_peer.c)
 *
 */

#ifndef OSMO_V110_PEER_H
#define OSMO_V110_PEER_H

#ifdef __cplusplus
extern "C" {
#endif

int osmo_peer_rates(void);
int osmo_peer_rate(int i, int *ir, int *chunk);
int osmo_peer_encode(int i, const unsigned char *user, int chunk,
		     unsigned char bits[80]);
int osmo_peer_decode(int i, const unsigned char bits[80],
		     unsigned char *user, int room, int *sx_on);

#ifdef __cplusplus
}
#endif

#endif
