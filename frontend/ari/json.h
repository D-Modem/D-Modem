/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    json.h - the JSON of ARI's events and replies, read into a tree
 *
 *    Enough of RFC 8259 for what Asterisk sends: objects, arrays,
 *    strings (with \u escapes, as UTF-8), numbers, true, false, null.
 *    A missing member or element reads as null, so lookups chain:
 *    event["channel"]["caller"]["number"].str().
 *
 */
#ifndef DMODEM_ARI_JSON_H
#define DMODEM_ARI_JSON_H

#include <string>
#include <utility>
#include <vector>

namespace ari {

struct Json {
	enum Type { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

	Type type = NUL;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<Json> a;
	std::vector<std::pair<std::string, Json>> o;

	/* member key of an object (null if none, or not an object) */
	const Json &operator[](const char *key) const;
	/* element i of an array (null if none) */
	const Json &operator[](size_t i) const;
	/* element i of an array, a negative i none (null) */
	const Json &operator[](int i) const
	{
		return i < 0 ? (*this)[static_cast<size_t>(-1)] :
			       (*this)[static_cast<size_t>(i)];
	}
	/* the string, else "" */
	const std::string &str() const;
	/* whether it is null (also a missing member or element) */
	bool null() const { return type == NUL; }

	/* text into out; false if it is not one JSON value */
	static bool parse(const std::string &text, Json &out);
};

} // namespace ari

#endif
