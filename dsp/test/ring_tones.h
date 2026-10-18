/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    ring_tones.h - the countries' ringing for the tests' networks: the
 *                   ringing tone the caller hears (its frequency, or two,
 *                   or one modulated), its cadence, which the ring signal
 *                   to the called line keeps too, and that signal's
 *                   frequency (ring_tones.cpp, from the ITU's list of
 *                   the tones of national networks, after E.180)
 *
 */
#ifndef DMODEM_TEST_RING_TONES_H
#define DMODEM_TEST_RING_TONES_H

namespace ring {

struct Country {
	const char *name;       /* upper case, words joined by '_' */
	int f1, f2;             /* Hz; f2 0: one tone */
	float mod;              /* f1 modulated at this (Hz), or 0 */
	int ring_hz;            /* the ring signal's */
	int n;                  /* the cadence: n seconds, on and off in
				   turn from on, round and round */
	float cadence[8];
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

/* whether the cadence is on at t seconds from the ringing's start */
inline bool on_at(const Country &c, double t)
{
	double period = 0;
	for (int i = 0; i < c.n; i++)
		period += c.cadence[i];
	double x = t - period * (double)(long)(t / period);
	for (int i = 0; i < c.n; i++) {
		if (x < c.cadence[i])
			return i % 2 == 0;
		x -= c.cadence[i];
	}
	return false;
}

} // namespace ring

#endif
