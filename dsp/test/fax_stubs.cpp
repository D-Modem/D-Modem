/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    fax_stubs.cpp - the modem core the FSK pump in dp_fsk.cpp links
 *    against, for the fax tests without a core (the DSP's log is
 *    test_util.cpp's)
 */

#include <stdio.h>

#include "../fax.h"

using namespace dsp;

/* the modem core's calls: no registry, S registers and parameters 0,
   every bit asked for given and every bit given taken (n) */
int modem_dp_register(enum DP_ID, struct dp_operations *) { return 0; }
/* nothing to deregister */
void modem_dp_deregister(enum DP_ID, struct dp_operations *) {}
/* S register: 0 */
int modem_get_sreg(struct modem *, unsigned int) { return 0; }
/* a parameter set: nothing to set; 0 */
long modem_set_param(struct modem *, unsigned, int) { return 0; }
/* a parameter: 0 */
intptr_t modem_get_param(struct modem *, unsigned) { return 0; }
/* the bits asked for: all n given */
int modem_get_bits(struct modem *, int, u8 *, int n) { return n; }
/* the bits given: all n taken */
int modem_put_bits(struct modem *, int, u8 *, int n) { return n; }
/* no V.8 for the FSK pump's low speed caller without a core */
s16 dsp::dp_v8_first(struct modem *, enum DP_ID) { return 0; }
