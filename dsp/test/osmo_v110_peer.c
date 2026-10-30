/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    osmo_v110_peer.c - libosmocore's V.110 (src/isdn/v110.c, built
 *    from its source tree into this test alone) behind a few plain
 *    functions, for v110_osmo_test.cpp: its RA1 at each user rate it has,
 *    its frame coding.  The independent end of D-Modem's V.110.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <osmocom/isdn/v110.h>

#include "osmo_v110_peer.h"

/* (libosmocore's OSMO_ASSERT) */
void osmo_panic(const char *fmt, ...)
{
	fprintf(stderr, "osmo_panic: %s\n", fmt);
	abort();
}

/* how many user rates libosmocore has */
int osmo_peer_rates(void)
{
	return _NUM_OSMO_V110_SYNC_RA1;
}

/* rate i's user rate, intermediate rate, and user bits a frame */
int osmo_peer_rate(int i, int *ir, int *chunk)
{
	*ir = osmo_v110_sync_ra1_get_intermediate_rate(i);
	*chunk = osmo_v110_sync_ra1_get_user_data_chunk_bitlen(i);
	return osmo_v110_sync_ra1_get_user_data_rate(i);
}

/* a frame's 80 bits at rate i from chunk user bits, S and X ON (0),
   E4-E7 ones; 0, or -1 */
int osmo_peer_encode(int i, const unsigned char *user, int chunk,
		     unsigned char bits[80])
{
	struct osmo_v110_decoded_frame f;
	memset(&f, 0, sizeof(f));
	memset(f.e_bits, 1, sizeof(f.e_bits));
	if (osmo_v110_sync_ra1_user_to_ir(i, &f, user, chunk) < 0)
		return -1;
	return osmo_v110_encode_frame(bits, 80, &f) == 80 ? 0 : -1;
}

/* a frame's user bits at rate i (its E1-E3 checked by libosmocore), and
   whether its S and X bits are all ON; how many bits, or -1 */
int osmo_peer_decode(int i, const unsigned char bits[80],
		     unsigned char *user, int room, int *sx_on)
{
	struct osmo_v110_decoded_frame f;
	static const int S[6] = { 0, 2, 3, 5, 7, 8 };   /* S1 .. S9 */
	int k;
	memset(&f, 0, sizeof(f));
	if (osmo_v110_decode_frame(&f, bits, 80) < 0)
		return -1;
	*sx_on = !f.x_bits[0] && !f.x_bits[1];
	for (k = 0; k < 6; k++)
		if (f.s_bits[S[k]])
			*sx_on = 0;
	return osmo_v110_sync_ra1_ir_to_user(i, user, room, &f);
}
