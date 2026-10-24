/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    modem_dp.h - a data pump as the modem core knows it: the instance
 *                 (struct dp), the table of functions a library registers
 *                 for each of its ids (struct dp_operations), the
 *                 registration, and the bits a pump exchanges with the
 *                 core
 *
 *    The library calls modem_dp_register() from prop_dp_init() for each
 *    id it serves (one table may serve several); the core creates a pump
 *    from the table when a call needs that modulation, runs process() on
 *    each block of samples, and destroys it at the end of the call or
 *    when the pump hands over to another one (DPSTAT_CHANGEDP).
 *
 *    Neither structure may change: Smart Link's i386 library has tables
 *    of exactly these six words, and its pump objects begin with a struct
 *    dp that its own state follows directly.
 *
 */

#ifndef DSP_MODEM_DP_H
#define DSP_MODEM_DP_H

#include "modem_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct modem;
struct dp;

/* one pump's functions */
struct dp_operations {
	/* for the log */
	const char *name;
	/* 0, not used */
	int use_count;
	/* a new pump on modem m for modulation id, on the calling side if
	   caller is not 0, at srate samples/s, process() never to be given
	   more than max_frag samples; op is this table.  Its id, modem and
	   op are set, status 0.  NULL if it cannot run */
	struct dp *(*create)(struct modem *m, enum DP_ID id, int caller,
			     s32 srate, int max_frag,
			     struct dp_operations *op);
	/* frees the pump */
	int (*destroy)(struct dp *d);
	/* cnt received samples (s16) in, cnt samples to send written to
	   out: a DPSTAT_* */
	int (*process)(struct dp *d, void *in, void *out, int cnt);
	/* the local side hangs up; may be NULL */
	int (*hangup)(struct dp *d);
};

/* a running pump: the start of each pump's object */
struct dp {
	enum DP_ID id;                  /* the modulation it was created for */
	struct modem *modem;
	unsigned status;                /* not used by the core */
	struct dp_operations *op;       /* the table it was created from */
	void *dp_data;                  /* the pump's own */
};

/*
 *    From the core.  A unit of bits is one byte with nbits bits in its
 *    low bits (the open pumps take one bit per byte).
 */

/* n units for the pump's transmitter, from the link layer (ones when it
   has nothing to send) into buf; returns n */
int modem_get_bits(struct modem *m, int nbits, u8 *buf, int n);
/* n units the pump's receiver got, from buf to the link layer; returns
   n */
int modem_put_bits(struct modem *m, int nbits, u8 *buf, int n);
/* op serves id from now on: 0, or -1 if the core has no such id or
   another table already serves it.  op must stay valid while
   registered */
int modem_dp_register(enum DP_ID id, struct dp_operations *op);
/* no table serves id any more (whatever op is) */
void modem_dp_deregister(enum DP_ID id, struct dp_operations *op);

#ifdef __cplusplus
}
#endif

#endif /* DSP_MODEM_DP_H */
