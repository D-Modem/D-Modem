/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    ebcdic.h - EBCDIC for the DTE: a code page both ways, and how the
 *               "AT" of a command line tells EBCDIC from ASCII
 *
 *    An EBCDIC DTE sends "AT" as 0xC1 0xE3 ("at": 0x81 0xA3) and "A/"
 *    as 0xC1 0x61, in each code page here.  No ASCII DTE sends these
 *    pairs, with or without a parity bit: ASCII "AT" with parity is
 *    0x41 or 0xC1, then 0x54 or 0xD4.  Thus the first two characters
 *    of a command line tell the character set, as they tell the rate
 *    and the parity to a modem that has autobaud.  The modem keeps
 *    ISO 8859-1 inside (ASCII for each command and result code); the
 *    code page changes the characters at the DTE's side only
 *    (docs/ebcdic.md).
 *
 */
#ifndef DCORE_EBCDIC_H
#define DCORE_EBCDIC_H

#include "dm_types.h"
#include "ebcdic_tables.h"

namespace dcore {
namespace ebcdic {

/* the code page when +DEBC names none: IBM 037 (USA and Canada) */
const u16 DEFAULT_PAGE = 37;

/* a code page both ways */
class CodePage {
public:
	/* IBM's code page id; DEFAULT_PAGE if there is no such table */
	explicit CodePage(u16 id = DEFAULT_PAGE) { select(id); }
	/* the table for id: false (the table unchanged) if there is none */
	bool select(u16 id) {
		for (const Table &t : tables) {
			if (t.id != id)
				continue;
			for (int i = 0; i < 256; i++) {
				in_[i] = t.to_latin1[i];
				out_[t.to_latin1[i]] = static_cast<u8>(i);
			}
			id_ = id;
			return true;
		}
		return id_ ? false : select(DEFAULT_PAGE);
	}
	/* the code page in use */
	u16 id() const { return id_; }
	/* an EBCDIC octet as ISO 8859-1 */
	char in(char c) const {
		return static_cast<char>(in_[static_cast<u8>(c)]);
	}
	/* an ISO 8859-1 octet as EBCDIC */
	char out(char c) const {
		return static_cast<char>(out_[static_cast<u8>(c)]);
	}
	/* n octets changed in place, each way */
	void in(char *buf, s32 n) const {
		for (s32 i = 0; i < n; i++)
			buf[i] = in(buf[i]);
	}
	/* (see in()) */
	void out(char *buf, s32 n) const {
		for (s32 i = 0; i < n; i++)
			buf[i] = out(buf[i]);
	}

private:
	u16 id_ = 0;
	u8 in_[256] = {}, out_[256] = {};
};

/* true if there is a table for IBM's code page id */
inline bool known(u32 id)
{
	for (const Table &t : tables)
		if (t.id == id)
			return true;
	return false;
}

/* the first two octets of a command line: 1 if an EBCDIC DTE sent
   them ("AT", "at", "A/" in EBCDIC), 0 if an ASCII DTE did (any
   parity), -1 if they are not the start of a command line */
inline int detect(char first, char second)
{
	u8 a = static_cast<u8>(first), b = static_cast<u8>(second);
	if ((a == 0xC1 || a == 0x81) && (b == 0xE3 || b == 0xA3 || b == 0x61))
		return 1;
	a &= 0x7F;
	b &= 0x7F;
	if ((a == 'A' || a == 'a') && (b == 'T' || b == 't' || b == '/'))
		return 0;
	return -1;
}

} // namespace ebcdic
} // namespace dcore

#endif
