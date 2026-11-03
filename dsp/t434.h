/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    t434.h - ITU-T T.434 binary file transfer (BFT): a file and its
 *             attributes as a BINARY-DATA message (ASN.1, BER), which
 *             a fax machine sends as an ECM page (T.30 Appendix V,
 *             DIS bit 53)
 *
 */

#ifndef __T434_H__
#define __T434_H__

#include <cstddef>
#include <string>
#include <vector>

#include "dm_types.h"

namespace dsp {

/* a file: what T.434 says of it that we use, and its octets.  What
   comes from the remote is its data: a name is not a safe path */
struct BftFile {
	std::string name;               /* filename's first component */
	std::string prefix;             /* ... the rest, joined (a path) */
	std::string mime;               /* mime-media-type, "type/sub" */
	std::string modified;           /* date-and-time-of-last-modification,
					   GeneralizedTime ("YYYYMMDDhhmmssZ") */
	s64 size = -1;                  /* filesize (-1: not given) */
	std::vector<unsigned char> data;
};

/* a BINARY-DATA message of the files appended to out */
void t434_encode(const std::vector<BftFile> &files, std::vector<unsigned char> *out);
void t434_encode(const BftFile &file, std::vector<unsigned char> *out);

/* the length of the BINARY-DATA message at data: 0 if its end has not
   come yet, -1 if it is none */
long t434_length(const unsigned char *data, size_t len);

/* the files of a BINARY-DATA message (len its octets); 0, or -1 when it
   is none */
s16 t434_decode(const unsigned char *data, size_t len, std::vector<BftFile> *files);

} // namespace dsp

#endif /* __T434_H__ */
