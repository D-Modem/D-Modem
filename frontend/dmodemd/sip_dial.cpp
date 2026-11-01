/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    sip_dial.cpp - the SIP user a dial string calls, and the caller of
 *                   a call that rings (sip.h)
 *
 */

#include <algorithm>
#include <cctype>
#include <cstring>

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

namespace {

/* s in lower case */
std::string lower(std::string s)
{
	for (char &c : s)
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return s;
}

/* s with its %XX escapes decoded (RFC 3986) */
std::string unescape(const std::string &s)
{
	std::string r;

	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size() &&
		    isxdigit(static_cast<unsigned char>(s[i + 1])) &&
		    isxdigit(static_cast<unsigned char>(s[i + 2]))) {
			r += static_cast<char>(
				std::stoi(s.substr(i + 1, 2), nullptr, 16));
			i += 2;
		} else {
			r += s[i];
		}
	}
	return r;
}

/* s with control characters (tabs, line ends) as spaces, and the ends'
   spaces gone */
std::string clean(const std::string &s)
{
	std::string r;

	for (char c : s)
		r += (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) ? ' ' :
									     c;
	size_t a = r.find_first_not_of(' '), b = r.find_last_not_of(' ');
	return a == std::string::npos ? std::string() : r.substr(a, b - a + 1);
}

} // namespace

/* see sip.h: the number is the URI's user part (a tel: URI's number),
   without its parameters; the name is the display name without its
   quotes and escapes */
Caller caller_of(const std::string &uri, const std::string &dname,
		 const std::string &privacy)
{
	Caller c;
	std::string u = uri, scheme, host, name;
	size_t i;

	if (!u.empty() && u.front() == '<')
		u.erase(0, 1);
	if ((i = u.find('>')) != std::string::npos)
		u.erase(i);
	if ((i = u.find(':')) != std::string::npos) {
		scheme = lower(u.substr(0, i));
		u.erase(0, i + 1);
	}
	if (scheme != "tel") {
		/* a sip: URI without a user is a host's: no number */
		i = u.find('@');
		host = i == std::string::npos ? lower(u) : lower(u.substr(i + 1));
		u = i == std::string::npos ? std::string() : u.substr(0, i);
	}
	if ((i = u.find(';')) != std::string::npos)
		u.erase(i);
	c.number = clean(unescape(u));
	c.number.erase(std::remove(c.number.begin(), c.number.end(), ' '),
		       c.number.end());

	for (i = 0; i < dname.size(); i++) {
		if (dname[i] == '\\' && i + 1 < dname.size())
			name += dname[++i];
		else if (dname[i] != '"')
			name += dname[i];
	}
	c.name = clean(name);

	std::string p = lower(privacy);
	bool all = p.find("id") != std::string::npos ||
		   p.find("user") != std::string::npos ||
		   p.find("header") != std::string::npos;
	bool anon = lower(c.number) == "anonymous" ||
		    host.compare(0, 17, "anonymous.invalid") == 0;
	if (all || anon) {
		c.number_withheld = c.name_withheld = true;
		c.number.clear();
		c.name.clear();
	}
	if (lower(c.name) == "anonymous") {
		c.name_withheld = true;
		c.name.clear();
	}
	return c;
}

} // namespace sip
