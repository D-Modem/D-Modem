/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    capi_msg.cpp - CAPI 2.0's messages (capi_msg.h)
 *
 */

#include <cstdio>

#include "capi_msg.h"

namespace capi {

/* see capi_msg.h */
std::vector<u8> encode(const Msg &m, size_t len_min)
{
	std::vector<u8> out;
	size_t len = 8 + m.params.size();
	if (len < len_min)
		len = len_min;
	Writer(out).w(static_cast<u16>(len)).w(m.appl).b(m.cmd).b(m.sub)
		.w(m.num);
	out.insert(out.end(), m.params.begin(), m.params.end());
	out.resize(len, 0);
	out.insert(out.end(), m.data.begin(), m.data.end());
	return out;
}

/* see capi_msg.h: a DATA_B3_REQ's data its length word's (at 16) after
   the message's length */
bool decode(const u8 *p, size_t n, Msg &m)
{
	if (n < 8)
		return false;
	Reader r(p, n);
	size_t len = r.w();
	m.appl = r.w();
	m.cmd = r.b();
	m.sub = r.b();
	m.num = r.w();
	/* (RCAPI's own commands have no length of their own) */
	if (len == 0 || len > n)
		len = n;
	if (len < 8)
		return false;
	m.params.assign(p + 8, p + len);
	m.data.clear();
	if (m.cmd == DATA_B3 && m.sub == REQ && len >= 18) {
		size_t dlen = static_cast<size_t>(p[16] | p[17] << 8);
		if (len + dlen > n)
			return false;
		m.data.assign(p + len, p + len + dlen);
	}
	return true;
}

/* see capi_msg.h */
Writer &Writer::s(const u8 *p, size_t n)
{
	if (n < 255) {
		b(static_cast<u8>(n));
	} else {
		b(0xFF);
		w(static_cast<u16>(n));
	}
	out_.insert(out_.end(), p, p + n);
	return *this;
}

/* see capi_msg.h */
u8 Reader::b()
{
	if (p_ >= end_) {
		ok_ = false;
		return 0;
	}
	return *p_++;
}

/* see capi_msg.h */
std::vector<u8> Reader::s()
{
	size_t n = b();
	if (n == 0xFF)
		n = w();
	if (n > left()) {
		ok_ = false;
		p_ = end_;
		return {};
	}
	std::vector<u8> v(p_, p_ + n);
	p_ += n;
	return v;
}

/* see capi_msg.h */
std::string name(u8 cmd, u8 sub)
{
	const char *c;
	switch (cmd) {
	case ALERT: c = "ALERT"; break;
	case CONNECT: c = "CONNECT"; break;
	case CONNECT_ACTIVE: c = "CONNECT_ACTIVE"; break;
	case DISCONNECT: c = "DISCONNECT"; break;
	case LISTEN: c = "LISTEN"; break;
	case INFO: c = "INFO"; break;
	case SELECT_B_PROTOCOL: c = "SELECT_B_PROTOCOL"; break;
	case FACILITY: c = "FACILITY"; break;
	case CONNECT_B3: c = "CONNECT_B3"; break;
	case CONNECT_B3_ACTIVE: c = "CONNECT_B3_ACTIVE"; break;
	case DISCONNECT_B3: c = "DISCONNECT_B3"; break;
	case DATA_B3: c = "DATA_B3"; break;
	case RESET_B3: c = "RESET_B3"; break;
	case CONNECT_B3_T90_ACTIVE: c = "CONNECT_B3_T90_ACTIVE"; break;
	case MANUFACTURER: c = "MANUFACTURER"; break;
	default: {
		char b[16];
		snprintf(b, sizeof(b), "0x%02X", cmd);
		return std::string(b) + "_" + std::to_string(sub);
	}
	}
	const char *s = sub == REQ ? "_REQ" : sub == CONF ? "_CONF" :
		sub == IND ? "_IND" : sub == RESP ? "_RESP" : "_?";
	return std::string(c) + s;
}

} // namespace capi
