/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    rtp_red.cpp - RFC 2198's redundant audio (rtp_red.h)
 *
 */
#include "rtp_red.h"

#include <cctype>
#include <cstring>

namespace dmd {
namespace red {

bool decode(const u8 *p, size_t n, u32 ts, std::vector<Block> *out)
{
	size_t at = 0, data = 0;

	out->clear();
	/* the headers: four octets each while F is set, then one */
	for (;;) {
		if (at >= n)
			goto bad;
		if (!(p[at] & 0x80)) {
			out->push_back({ static_cast<u8>(p[at] & 0x7f), ts,
					 nullptr, 0 });
			at++;
			break;
		}
		if (at + 4 > n)
			goto bad;
		u32 off = static_cast<u32>(p[at + 1]) << 6 | p[at + 2] >> 2;
		size_t len = static_cast<size_t>(p[at + 2] & 3) << 8 |
			     p[at + 3];
		out->push_back({ static_cast<u8>(p[at] & 0x7f), ts - off,
				 nullptr, len });
		data += len;
		at += 4;
	}
	/* the data: the redundant blocks' lengths, the rest the primary's */
	if (data > n - at)
		goto bad;
	out->back().n = n - at - data;
	for (Block &b : *out) {
		b.data = p + at;
		at += b.n;
	}
	return true;
bad:
	out->clear();
	return false;
}

size_t encode(const Block *red, size_t nred, const Block &primary, u8 *out,
	      size_t max)
{
	size_t hdr = 1, data = primary.n, at = 0;
	std::vector<const Block *> v;

	for (size_t i = 0; i < nred; i++) {
		u32 off = primary.ts - red[i].ts;
		if (!off || off > MAX_OFFSET || red[i].n > MAX_LEN)
			continue;
		v.push_back(&red[i]);
		hdr += 4;
		data += red[i].n;
	}
	if (hdr + data > max)
		return 0;
	for (const Block *b : v) {
		u32 off = primary.ts - b->ts;
		out[at++] = static_cast<u8>(0x80 | (b->pt & 0x7f));
		out[at++] = static_cast<u8>(off >> 6);
		out[at++] = static_cast<u8>((off & 0x3f) << 2 | b->n >> 8);
		out[at++] = static_cast<u8>(b->n);
	}
	out[at++] = static_cast<u8>(primary.pt & 0x7f);
	for (const Block *b : v) {
		if (b->n)
			memcpy(out + at, b->data, b->n);
		at += b->n;
	}
	if (primary.n)
		memcpy(out + at, primary.data, primary.n);
	return at + primary.n;
}

std::vector<s16> fmtp_types(const std::string &v)
{
	std::vector<s16> t;
	size_t i = 0;

	while (i < v.size() && isspace(static_cast<unsigned char>(v[i])))
		i++;
	for (;;) {
		s32 x = 0;
		size_t start = i;
		while (i < v.size() && isdigit(static_cast<unsigned char>(v[i])))
			if ((x = x * 10 + (v[i++] - '0')) > 127)
				return {};
		if (i == start)
			return {};
		t.push_back(static_cast<s16>(x));
		if (i < v.size() && v[i] == '/') {
			i++;
			continue;
		}
		break;
	}
	while (i < v.size() && isspace(static_cast<unsigned char>(v[i])))
		i++;
	if (i != v.size())
		return {};
	return t;
}

std::string fmtp(s16 pt, s16 level)
{
	std::string p = std::to_string(pt), s = p;
	for (s16 i = 0; i < level; i++)
		s += "/" + p;
	return s;
}

} // namespace red
} // namespace dmd
