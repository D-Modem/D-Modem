/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    str.h - text helpers in place of the C library's: numbers to
 *            text (dec(), hex(), fixed()), numbers from text
 *            (to_ulong(), parse_long(), leading_long()), and
 *            comparisons (starts_with(), and same_nocase() and
 *            starts_nocase(), which ignore case)
 *
 */

#ifndef DCORE_STR_H
#define DCORE_STR_H

#include <charconv>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace dcore {

/* v in decimal, at least width digits (zeros after a minus sign) */
template <class T>
inline std::string dec(T v, int width = 0)
{
	static_assert(std::is_integral<T>::value, "dec() takes integers");
	std::string s = std::to_string(v);
	std::size_t digits = s.size() - (v < 0 ? 1 : 0);
	if (static_cast<int>(digits) < width)
		s.insert(v < 0 ? 1 : 0, width - digits, '0');
	return s;
}

/* v in hexadecimal, lower case (upper with upper_case), at least width
   digits */
inline std::string hex(unsigned long v, int width = 0, bool upper_case = false)
{
	char buf[2 * sizeof(v)];
	auto r = std::to_chars(buf, buf + sizeof(buf), v, 16);
	std::string s(buf, r.ptr);
	if (upper_case)
		for (char &c : s)
			if (c >= 'a' && c <= 'f')
				c = static_cast<char>(c - 'a' + 'A');
	if (static_cast<int>(s.size()) < width)
		s.insert(0, width - s.size(), '0');
	return s;
}

/* the unsigned number in base at the start of [p, last), *end after
   its digits (p and 0 when there are none); too large a number is
   ULONG_MAX, as strtoul() has it */
inline unsigned long to_ulong(const char *p, const char *last,
			      const char **end, int base = 10)
{
	unsigned long v = 0;
	auto r = std::from_chars(p, last, v, base);
	*end = r.ptr;
	if (r.ec == std::errc::result_out_of_range) {
		/* past the digits that did not fit */
		const char *q = p;
		unsigned long d;
		while (q < last && std::from_chars(q, q + 1, d, base).ec ==
		       std::errc())
			q++;
		*end = q;
		return std::numeric_limits<unsigned long>::max();
	}
	return v;
}

/* the whole of s as a number in base (0: as C's strtol() has it, 0x
   hex, 0 octal, else decimal), a sign allowed: false if s is anything
   else, or too large */
inline bool parse_long(std::string_view s, long &v, int base = 10)
{
	bool neg = false;
	if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
		neg = s[0] == '-';
		s.remove_prefix(1);
	}
	if (base == 0) {
		if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			base = 16;
			s.remove_prefix(2);
		} else {
			base = s.size() > 1 && s[0] == '0' ? 8 : 10;
		}
	}
	if (s.empty())
		return false;
	unsigned long u;
	auto r = std::from_chars(s.data(), s.data() + s.size(), u, base);
	if (r.ec != std::errc() || r.ptr != s.data() + s.size() ||
	    u > static_cast<unsigned long>(std::numeric_limits<long>::max()))
		return false;
	v = neg ? -static_cast<long>(u) : static_cast<long>(u);
	return true;
}

/* the decimal number at the start of s, after blanks and a sign, as
   C's atoi() reads it: 0 if there is none */
inline long leading_long(std::string_view s)
{
	std::size_t i = 0;
	bool neg = false;
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
		i++;
	if (i < s.size() && (s[i] == '+' || s[i] == '-'))
		neg = s[i++] == '-';
	long v = 0;
	std::from_chars(s.data() + i, s.data() + s.size(), v);
	return neg ? -v : v;
}

/* v with prec digits after the point ("%.*f" in the C locale) */
inline std::string fixed(double v, int prec)
{
	std::ostringstream o;
	o.imbue(std::locale::classic());
	o << std::fixed << std::setprecision(prec) << v;
	return o.str();
}

/* s begins with prefix */
inline bool starts_with(std::string_view s, std::string_view prefix)
{
	return s.substr(0, prefix.size()) == prefix;
}

/* c in upper case (ASCII) */
inline char upper_ascii(char c)
{
	return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

/* a and b the same, case aside (ASCII) */
inline bool same_nocase(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (std::size_t i = 0; i < a.size(); i++)
		if (upper_ascii(a[i]) != upper_ascii(b[i]))
			return false;
	return true;
}

/* s begins with prefix, case aside (ASCII) */
inline bool starts_nocase(std::string_view s, std::string_view prefix)
{
	return s.size() >= prefix.size() &&
		same_nocase(s.substr(0, prefix.size()), prefix);
}

} // namespace dcore

#endif
