/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    libc_compat.h - the test programs' C library the same everywhere:
 *                    on Windows the few POSIX calls they make that its
 *                    C library has under other names or not at all
 *                    (setenv, unsetenv, mkdir with a mode); and with
 *                    any C library but glibc (Windows', musl, Apple's,
 *                    Android's) glibc's rand() (its TYPE_3 additive
 *                    generator, RAND_MAX 2^31 - 1), so that the tests'
 *                    noise and random documents are the same draws as
 *                    with glibc, where the tests' thresholds were set.
 *                    The build includes it ahead of each test source
 *                    here; with glibc on Linux it is empty.
 */
#ifndef DMODEM_LIBC_COMPAT_H
#define DMODEM_LIBC_COMPAT_H

/* (any C library header: glibc's defines __GLIBC__) */
#include <stdlib.h>

#ifdef _WIN32
#include <direct.h>

/* name=value in the environment (overwrite 0: kept if it is there) */
static inline int setenv(const char *name, const char *value, int overwrite)
{
	if (!overwrite && getenv(name))
		return 0;
	return _putenv_s(name, value);
}

/* name out of the environment */
static inline int unsetenv(const char *name)
{
	return _putenv_s(name, "");
}

#ifdef __cplusplus
/* a directory (Windows has no mode for it) */
static inline int mkdir(const char *path, int)
{
	return _mkdir(path);
}
#endif
#endif /* _WIN32 */

#ifndef __GLIBC__
#include <stdint.h>

/* glibc's random(3) state (TYPE_3: x(i) = x(i-3) + x(i-31)), the last 34
   words in a ring */
struct dmodem_glibc_rand_state {
	int32_t r[34];
	unsigned k;
	int seeded;
};

/* the one random(3) state */
static inline struct dmodem_glibc_rand_state *dmodem_glibc_rand_st(void)
{
	static struct dmodem_glibc_rand_state s;
	return &s;
}

/* srand(seed) as glibc: the words from the seed by Park and Miller's
   16807 generator, then 310 outputs thrown away */
static inline void dmodem_glibc_srand(unsigned seed)
{
	struct dmodem_glibc_rand_state *s = dmodem_glibc_rand_st();
	int32_t r[34];
	int i;
	r[0] = (int32_t)(seed ? seed : 1);
	for (i = 1; i < 31; i++) {
		int32_t hi = r[i - 1] / 127773, lo = r[i - 1] % 127773;
		int32_t w = 16807 * lo - 2836 * hi;
		r[i] = w < 0 ? w + 2147483647 : w;
	}
	for (i = 31; i < 34; i++)
		r[i] = r[i - 31];
	for (i = 0; i < 34; i++)
		s->r[i] = r[i];
	s->k = 34;
	s->seeded = 1;
	for (i = 34; i < 344; i++) {
		unsigned k = s->k++;
		s->r[k % 34] = (int32_t)((uint32_t)s->r[(k - 31) % 34] +
					 (uint32_t)s->r[(k - 3) % 34]);
	}
}

/* rand() as glibc: 0 to 2^31 - 1 */
static inline int dmodem_glibc_rand(void)
{
	struct dmodem_glibc_rand_state *s = dmodem_glibc_rand_st();
	if (!s->seeded)
		dmodem_glibc_srand(1);
	unsigned k = s->k++;
	uint32_t v = (uint32_t)s->r[(k - 31) % 34] +
		(uint32_t)s->r[(k - 3) % 34];
	s->r[k % 34] = (int32_t)v;
	return (int)(v >> 1);
}

#ifdef __cplusplus
/* (std::rand in the C++ library's headers, after the macros below) */
namespace std {
using ::dmodem_glibc_rand;
using ::dmodem_glibc_srand;
}
#endif

#undef RAND_MAX
#define RAND_MAX 2147483647
#define rand() dmodem_glibc_rand()
#define srand(seed) dmodem_glibc_srand(seed)
#endif /* !__GLIBC__ */

#endif
