/*
 *
 *    sip_dial.cpp - the SIP user a dial string calls (sip.h)
 *
 */

#include <string.h>

#include "sip.h"

namespace sip {

/* see sip.h */
std::string dial_user(const char *dial)
{
	/* V.250's dial string: the digits and what DTMF has (dialed), its
	   modifiers (T, P, W, comma, @, !, ;) and the characters that only
	   format a number */
	static const char dialed[] = "0123456789*#+ABCDabcd";
	static const char other[] = "TtPpWw,@!; -().";
	std::string user;

	if (!dial)
		return user;
	for (const char *p = dial; *p; p++) {
		if (strchr(dialed, *p))
			user += *p;
		else if (!strchr(other, *p))
			return dial;    /* a name: as it is */
	}
	return user;
}

} // namespace sip
