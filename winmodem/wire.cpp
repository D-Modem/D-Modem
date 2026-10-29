/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    wire.cpp - the driver's and dmodemd's frames (wire.h)
 *
 */

#include "dm_types.h"
#include "wire.h"

#include <cstring>

namespace vmodem {

/* see wire.h */
void wire_put(std::string &out, FrameType t, const void *p, size_t n)
{
	out.push_back(static_cast<char>(t));
	out.push_back(static_cast<char>(n & 0xff));
	out.push_back(static_cast<char>(n >> 8));
	out.append(static_cast<const char *>(p), n);
}

/* see wire.h */
void wire_hello(std::string &out, const std::string &port)
{
	std::string p;
	p.push_back(static_cast<char>(WIRE_VERSION & 0xff));
	p.push_back(static_cast<char>(WIRE_VERSION >> 8));
	p += port.substr(0, 255);
	wire_put(out, FRAME_HELLO, p.data(), p.size());
}

/* see wire.h */
void wire_lines(std::string &out, u8 lines)
{
	wire_put(out, FRAME_LINES, &lines, 1);
}

/* see wire.h */
void wire_state(std::string &out, u8 state, u32 baud)
{
	u8 p[5] = { state, static_cast<u8>(baud),
			 static_cast<u8>(baud >> 8),
			 static_cast<u8>(baud >> 16),
			 static_cast<u8>(baud >> 24) };
	wire_put(out, FRAME_STATE, p, sizeof(p));
}

/* see wire.h */
void wire_data(std::string &out, const void *p, size_t n)
{
	const u8 *b = static_cast<const u8 *>(p);
	while (n > 0) {
		size_t k = n < WIRE_MAX_DATA ? n : WIRE_MAX_DATA;
		wire_put(out, FRAME_DATA, b, k);
		b += k;
		n -= k;
	}
}

/* see wire.h: what has been taken dropped first, now and then */
void WireReader::feed(const void *p, size_t n)
{
	if (pos_ > 0 && pos_ >= buf_.size() / 2) {
		buf_.erase(0, pos_);
		pos_ = 0;
	}
	buf_.append(static_cast<const char *>(p), n);
}

/* see wire.h */
bool WireReader::next(Frame &f)
{
	if (bad_ || buf_.size() - pos_ < WIRE_HEAD)
		return false;
	const u8 *h =
		reinterpret_cast<const u8 *>(buf_.data()) + pos_;
	size_t n = h[1] | static_cast<size_t>(h[2]) << 8;
	switch (h[0]) {
	case FRAME_DATA:
		if (n > WIRE_MAX_DATA) {
			bad_ = true;
			return false;
		}
		break;
	case FRAME_HELLO:
	case FRAME_LINES:
	case FRAME_STATE:
		if (n > 512) {
			bad_ = true;
			return false;
		}
		break;
	default:
		bad_ = true;
		return false;
	}
	if (buf_.size() - pos_ < WIRE_HEAD + n)
		return false;
	f.type = static_cast<FrameType>(h[0]);
	f.p = h + WIRE_HEAD;
	f.n = n;
	pos_ += WIRE_HEAD + n;
	return true;
}

/* see wire.h */
bool WireReader::hello(const Frame &f, u16 *version, std::string *port)
{
	if (f.type != FRAME_HELLO || f.n < 2)
		return false;
	*version = static_cast<u16>(f.p[0] | u16{f.p[1]} << 8);
	if (port)
		port->assign(reinterpret_cast<const char *>(f.p) + 2, f.n - 2);
	return true;
}

/* see wire.h */
bool WireReader::lines(const Frame &f, u8 *lines)
{
	if (f.type != FRAME_LINES || f.n < 1)
		return false;
	*lines = f.p[0];
	return true;
}

/* see wire.h */
bool WireReader::state(const Frame &f, u8 *state, u32 *baud)
{
	if (f.type != FRAME_STATE || f.n < 5)
		return false;
	*state = f.p[0];
	*baud = f.p[1] | static_cast<u32>(f.p[2]) << 8 |
		static_cast<u32>(f.p[3]) << 16 |
		static_cast<u32>(f.p[4]) << 24;
	return true;
}

} // namespace vmodem
