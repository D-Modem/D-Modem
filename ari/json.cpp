/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    json.cpp - a JSON reader (see json.h)
 *
 */
#include <stdlib.h>
#include <string.h>

#include "json.h"

namespace ari {

namespace {

const Json none;
const std::string empty;

/* a recursive descent over text from p */
struct Reader {
	const char *p, *end;
	int depth = 0;

	void space()
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' ||
				   *p == '\r'))
			p++;
	}
	bool literal(const char *word)
	{
		size_t n = strlen(word);
		if ((size_t)(end - p) < n || memcmp(p, word, n))
			return false;
		p += n;
		return true;
	}
	/* four hex digits of a \u escape */
	bool hex4(unsigned &v)
	{
		v = 0;
		for (int i = 0; i < 4; i++, p++) {
			if (p >= end)
				return false;
			char c = *p;
			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= c - '0';
			else if (c >= 'a' && c <= 'f')
				v |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				v |= c - 'A' + 10;
			else
				return false;
		}
		return true;
	}
	/* code point u as UTF-8 */
	static void utf8(std::string &s, unsigned u)
	{
		if (u < 0x80) {
			s += (char)u;
		} else if (u < 0x800) {
			s += (char)(0xc0 | u >> 6);
			s += (char)(0x80 | (u & 0x3f));
		} else if (u < 0x10000) {
			s += (char)(0xe0 | u >> 12);
			s += (char)(0x80 | (u >> 6 & 0x3f));
			s += (char)(0x80 | (u & 0x3f));
		} else {
			s += (char)(0xf0 | u >> 18);
			s += (char)(0x80 | (u >> 12 & 0x3f));
			s += (char)(0x80 | (u >> 6 & 0x3f));
			s += (char)(0x80 | (u & 0x3f));
		}
	}
	/* a string, p at its opening quote */
	bool string(std::string &s)
	{
		p++;
		while (p < end && *p != '"') {
			if ((unsigned char)*p < 0x20)
				return false;
			if (*p != '\\') {
				s += *p++;
				continue;
			}
			if (++p >= end)
				return false;
			char c = *p++;
			unsigned u, lo;
			switch (c) {
			case '"': case '\\': case '/': s += c; break;
			case 'b': s += '\b'; break;
			case 'f': s += '\f'; break;
			case 'n': s += '\n'; break;
			case 'r': s += '\r'; break;
			case 't': s += '\t'; break;
			case 'u':
				if (!hex4(u))
					return false;
				/* a surrogate pair */
				if (u >= 0xd800 && u < 0xdc00 && end - p >= 6 &&
				    p[0] == '\\' && p[1] == 'u') {
					p += 2;
					if (!hex4(lo) || lo < 0xdc00 || lo > 0xdfff)
						return false;
					u = 0x10000 + ((u - 0xd800) << 10) +
						(lo - 0xdc00);
				}
				utf8(s, u);
				break;
			default:
				return false;
			}
		}
		if (p >= end)
			return false;
		p++;
		return true;
	}
	bool value(Json &v)
	{
		space();
		if (p >= end || ++depth > 64)
			return false;
		bool ok = true;
		switch (*p) {
		case '{':
			v.type = Json::OBJECT;
			p++;
			space();
			if (p < end && *p == '}') {
				p++;
				break;
			}
			for (;;) {
				std::string key;
				Json member;
				space();
				if (p >= end || *p != '"' || !string(key))
					return false;
				space();
				if (p >= end || *p++ != ':' || !value(member))
					return false;
				v.o.emplace_back(std::move(key),
						 std::move(member));
				space();
				if (p < end && *p == ',') {
					p++;
					continue;
				}
				if (p < end && *p == '}') {
					p++;
					break;
				}
				return false;
			}
			break;
		case '[':
			v.type = Json::ARRAY;
			p++;
			space();
			if (p < end && *p == ']') {
				p++;
				break;
			}
			for (;;) {
				Json e;
				if (!value(e))
					return false;
				v.a.push_back(std::move(e));
				space();
				if (p < end && *p == ',') {
					p++;
					continue;
				}
				if (p < end && *p == ']') {
					p++;
					break;
				}
				return false;
			}
			break;
		case '"':
			v.type = Json::STRING;
			ok = string(v.s);
			break;
		case 't':
			v.type = Json::BOOL;
			v.b = true;
			ok = literal("true");
			break;
		case 'f':
			v.type = Json::BOOL;
			ok = literal("false");
			break;
		case 'n':
			ok = literal("null");
			break;
		default: {
			/* a number: strtod on a copy (the text is not ended) */
			const char *q = p;
			while (q < end && strchr("+-0123456789.eE", *q))
				q++;
			std::string num(p, q);
			char *e;
			if (num.empty())
				return false;
			v.type = Json::NUMBER;
			v.n = strtod(num.c_str(), &e);
			ok = *e == 0;
			p = q;
		}
		}
		depth--;
		return ok;
	}
};

} // namespace

/* see json.h */
const Json &Json::operator[](const char *key) const
{
	if (type == OBJECT)
		for (const auto &m : o)
			if (m.first == key)
				return m.second;
	return none;
}

/* see json.h */
const Json &Json::operator[](size_t i) const
{
	return type == ARRAY && i < a.size() ? a[i] : none;
}

/* see json.h */
const std::string &Json::str() const
{
	return type == STRING ? s : empty;
}

/* see json.h */
bool Json::parse(const std::string &text, Json &out)
{
	Reader r{ text.data(), text.data() + text.size() };
	out = Json();
	if (!r.value(out))
		return false;
	r.space();
	return r.p == r.end;
}

} // namespace ari
