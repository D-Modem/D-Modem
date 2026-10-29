/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    env.h - settings from the environment: the tests' DSP_* switches
 *            and the front ends' own.  A number is read as atoi() reads
 *            it (its leading digits; 0 when there are none)
 *
 */

#ifndef DSP_ENV_H
#define DSP_ENV_H

#include <cstdlib>
#include <cstring>

#include "dm_types.h"

namespace dsp {

/* var's value, nullptr when it is not set */
inline const char *env(const char *var)
{
	return std::getenv(var);
}

/* var as a number, def when it is not set */
inline long env_long(const char *var, long def = 0)
{
	const char *e = env(var);
	return e ? std::strtol(e, nullptr, 10) : def;
}

/* (see env_long()) */
inline s32 env_int(const char *var, s32 def = 0)
{
	return static_cast<s32>(env_long(var, def));
}

/* var as a real number, def when it is not set */
inline double env_double(const char *var, double def = 0)
{
	const char *e = env(var);
	return e ? std::strtod(e, nullptr) : def;
}

/* var is set to 0: a switch, on by default, turned off */
inline bool env_off(const char *var)
{
	const char *e = env(var);
	return e && std::strtol(e, nullptr, 10) == 0;
}

/* var is not set to 0 (see env_off()) */
inline bool env_on(const char *var)
{
	return !env_off(var);
}

/* var is set to a non-zero number: a switch, off by default, turned
   on */
inline bool env_set_on(const char *var)
{
	return env_long(var) != 0;
}

/* var is set to exactly s */
inline bool env_is(const char *var, const char *s)
{
	const char *e = env(var);
	return e && std::strcmp(e, s) == 0;
}

} // namespace dsp

#endif
