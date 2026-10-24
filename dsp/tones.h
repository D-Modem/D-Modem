/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    tones.h - the countries' tones, from the ITU's list of the tones
 *              of national networks (after E.180; tones.cpp): the ringing
 *              tone the caller hears (its frequency, or two, or one
 *              modulated, and its cadence, which the ring signal to the
 *              called line keeps too), that signal's frequency, the dial
 *              tone, the busy tone and the congestion tone.  The call
 *              progress pump takes the country's busy and congestion
 *              tones for busy (dp_callprog.cpp); the tests' network,
 *              netsim, plays them all
 *
 */
#ifndef DMODEM_TONES_H
#define DMODEM_TONES_H


#include "dm_types.h"

namespace tones {

/* a tone and its cadence */
struct Tone {
	int f1, f2;             /* Hz; f2 0: one tone */
	float mod;              /* f1 modulated at this (Hz), or 0 */
	int n;                  /* the cadence: n seconds, on and off in
				   turn from on, round and round; 0:
				   continuous */
	float cadence[8];
};

struct Country {
	const char *name;       /* upper case, words joined by '_' */
	Tone ringing;           /* (n > 0) */
	int ring_hz;            /* the ring signal's */
	Tone dial;
	Tone busy;              /* the called line busy */
	Tone congestion;        /* the network's lines or switches busy */
};

/* the countries, up to an entry whose name is null */
extern const Country COUNTRIES[];

/* other names of them (the core's, USA, UK, ...), up to a null one */
struct Alias {
	const char *alias, *name;
};
extern const Alias ALIASES[];

/* a country by its name or an alias (case, spaces, '-' and '.' as '_'),
   or null */
const Country *find(const char *name);

/* whether the tone's cadence is on at t seconds from its start */
inline bool on_at(const Tone &c, double t)
{
	if (c.n <= 0)
		return true;
	double period = 0;
	for (int i = 0; i < c.n; i++)
		period += c.cadence[i];
	double x = t - period * static_cast<double>(
				       static_cast<s64>(t / period));
	for (int i = 0; i < c.n; i++) {
		if (x < c.cadence[i])
			return i % 2 == 0;
		x -= c.cadence[i];
	}
	return false;
}

/* the cadence's period (s), 0 if continuous */
inline double period(const Tone &c)
{
	double p = 0;
	for (int i = 0; i < c.n; i++)
		p += c.cadence[i];
	return p;
}

} // namespace tones

#endif
