/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    capi_msg.h - COMMON-ISDN-API 2.0's messages (CAPI 2.0, ETSI's
 *                 ES 201 915): their header, their parameters (words and
 *                 dwords little endian; a struct its length, one octet,
 *                 or 0xFF and a word, then its octets), the commands and
 *                 the info values dmodem-capid uses
 *
 *    A message: its total length (a word, the parameters' end; a
 *    DATA_B3's data is not counted), the application's id, the command,
 *    the subcommand, the message number, then the parameters.
 *
 */

#ifndef DMODEM_CAPI_MSG_H
#define DMODEM_CAPI_MSG_H

#include <string>
#include <vector>

#include "dm_types.h"

namespace capi {

/* commands */
enum : u8 {
	ALERT = 0x01, CONNECT = 0x02, CONNECT_ACTIVE = 0x03,
	DISCONNECT = 0x04, LISTEN = 0x05, INFO = 0x08,
	SELECT_B_PROTOCOL = 0x41, FACILITY = 0x80, CONNECT_B3 = 0x82,
	CONNECT_B3_ACTIVE = 0x83, DISCONNECT_B3 = 0x84, DATA_B3 = 0x86,
	RESET_B3 = 0x87, CONNECT_B3_T90_ACTIVE = 0x88, MANUFACTURER = 0xFF,
};
/* subcommands */
enum : u8 { REQ = 0x80, CONF = 0x81, IND = 0x82, RESP = 0x83 };

/* info and reason values */
enum : u16 {
	NO_ERROR = 0x0000,
	ILLEGAL_CONTROLLER = 0x2002,
	ILLEGAL_IDENT = 0x2002 + 0,     /* (PLCI or NCCI: 0x2002 too) */
	OUT_OF_PLCI = 0x2003,
	OUT_OF_NCCI = 0x2004,
	ILLEGAL_MESSAGE = 0x2007,
	MESSAGE_NOT_SUPPORTED = 0x2001,
	B1_NOT_SUPPORTED = 0x3001,
	B2_NOT_SUPPORTED = 0x3002,
	B3_NOT_SUPPORTED = 0x3003,
	B1_PARAM_NOT_SUPPORTED = 0x3004,
	CIP_NOT_SUPPORTED = 0x300A,
	FACILITY_NOT_SUPPORTED = 0x300B,
	/* a reason: the line's (layer 1) */
	NO_LAYER1 = 0x3301,
	/* reasons: Q.850's cause, 0x3480 | cause */
	CAUSE_NORMAL = 0x3490, CAUSE_BUSY = 0x3491,
	CAUSE_NO_RESPONSE = 0x3492, CAUSE_NO_ANSWER = 0x3493,
	CAUSE_REJECTED = 0x3495, CAUSE_UNSPECIFIED = 0x349F,
};

/* CIP values (the call's service) */
enum : u16 { CIP_ANY = 0, CIP_SPEECH = 1, CIP_UDI = 2, CIP_AUDIO = 4 };

/* the RCAPI commands of libcapi20's remote CAPI (rcapid's): command
   and subcommand */
enum : u16 {
	RCAPI_REGISTER_REQ = 0xF2FF, RCAPI_REGISTER_CONF = 0xF3FF,
	RCAPI_GET_MANUFACTURER_REQ = 0xFAFF,
	RCAPI_GET_MANUFACTURER_CONF = 0xFBFF,
	RCAPI_GET_SERIAL_NUMBER_REQ = 0xFEFF,
	RCAPI_GET_SERIAL_NUMBER_CONF = 0xFFFF,
	RCAPI_GET_VERSION_REQ = 0xFCFF, RCAPI_GET_VERSION_CONF = 0xFDFF,
	RCAPI_GET_PROFILE_REQ = 0xE0FF, RCAPI_GET_PROFILE_CONF = 0xE1FF,
	RCAPI_AUTH_USER_REQ = 0xFF00, RCAPI_AUTH_USER_CONF = 0xFF01,
};

/* a message, its header taken apart */
struct Msg {
	u16 appl = 0;
	u8 cmd = 0, sub = 0;
	u16 num = 0;
	std::vector<u8> params;         /* after the header */
	std::vector<u8> data;           /* a DATA_B3's, after the message */
	/* cmd and sub as one word, as the RCAPI commands are */
	u16 command() const { return static_cast<u16>(cmd << 8 | sub); }
};

/* the message in octets (data after it); its length word the header's
   and the parameters', len_min at least (DATA_B3: 22 or 30) */
std::vector<u8> encode(const Msg &m, size_t len_min = 0);
/* a message's n octets (data, for a DATA_B3_REQ, after its length);
   false if they are not one */
bool decode(const u8 *p, size_t n, Msg &m);

/* parameters written in order */
class Writer {
public:
	/* appending to out */
	explicit Writer(std::vector<u8> &out) : out_(out) {}
	/* a byte */
	Writer &b(u8 v) { out_.push_back(v); return *this; }
	/* a word */
	Writer &w(u16 v) { return b(static_cast<u8>(v)).b(static_cast<u8>(v >> 8)); }
	/* a dword */
	Writer &d(u32 v) {
		return w(static_cast<u16>(v)).w(static_cast<u16>(v >> 16));
	}
	/* a struct of n octets */
	Writer &s(const u8 *p, size_t n);
	/* (of a string, without its end) */
	Writer &s(const std::string &v) {
		return s(reinterpret_cast<const u8 *>(v.data()), v.size());
	}
	/* (of octets) */
	Writer &s(const std::vector<u8> &v) { return s(v.data(), v.size()); }
	/* an empty struct */
	Writer &empty() { return b(0); }
private:
	std::vector<u8> &out_;
};

/* parameters read in order; past the end, zeros and empty structs, and
   ok() false */
class Reader {
public:
	/* n octets at p */
	Reader(const u8 *p, size_t n) : p_(p), end_(p + n) {}
	/* (all of v) */
	explicit Reader(const std::vector<u8> &v)
		: Reader(v.data(), v.size()) {}
	/* (not of a temporary, which would be gone before the reading) */
	explicit Reader(std::vector<u8> &&) = delete;
	/* a byte */
	u8 b();
	/* a word */
	u16 w() { u16 lo = b(); return static_cast<u16>(lo | b() << 8); }
	/* a dword */
	u32 d() { u32 lo = w(); return lo | static_cast<u32>(w()) << 16; }
	/* a struct's octets */
	std::vector<u8> s();
	/* nothing has run past the end */
	bool ok() const { return ok_; }
	/* octets left */
	size_t left() const { return static_cast<size_t>(end_ - p_); }
private:
	const u8 *p_, *end_;
	bool ok_ = true;
};

/* a command's name, for the log ("CONNECT_REQ") */
std::string name(u8 cmd, u8 sub);

} // namespace capi

#endif
