#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# gen-ebcdic-tables.py - write core/ebcdic_tables.h from the code pages
# of go3270 (D-Modem/go3270, internal/codepage/cp*.go).  Those tables
# come from the ICU data of the Unicode Consortium (icu-data, the
# ibm-*.ucm files).  Only the code pages that map each of the 256 EBCDIC
# octets to one ISO 8859-1 octet go into the header.
#
#   tools/gen-ebcdic-tables.py ../go3270 > core/ebcdic_tables.h

import glob
import os
import re
import sys

PAGES = [37, 273, 275, 277, 278, 280, 284, 285, 297, 500, 871, 1047]


def table(path):
    text = open(path, encoding="utf-8").read()
    body = re.search(r"e2u: \[\]rune\{(.*?)\n\t\},", text, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body)
    vals = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", body)]
    ucm = re.search(r"ibm-[^ ]*\.ucm", text).group(0)
    if len(vals) != 256 or max(vals) > 255 or len(set(vals)) != 256:
        sys.exit(f"{path}: not a one-to-one map to ISO 8859-1")
    return vals, ucm


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "../go3270"
    cpdir = os.path.join(root, "internal", "codepage")
    print("""/*
 * SPDX-License-Identifier: AGPL-3.0-only AND Unicode-3.0
 * Copyright (C) 2026 D-Modem Authors
 *
 *    ebcdic_tables.h - IBM's EBCDIC code pages as ISO 8859-1, one octet
 *                      for each octet (ebcdic.h)
 *
 *    Written by tools/gen-ebcdic-tables.py from go3270's code pages,
 *    which come from the ICU data of the Unicode Consortium (each
 *    table names its .ucm file).  Do not edit.
 *
 *    The ICU data's licence:
 *
 *    UNICODE LICENSE V3
 *
 *    COPYRIGHT AND PERMISSION NOTICE
 *
 *    Copyright © 2016-2024 Unicode, Inc.
 *
 *    NOTICE TO USER: Carefully read the following legal agreement. BY
 *    DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR
 *    SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE
 *    TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT
 *    DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.
 *
 *    Permission is hereby granted, free of charge, to any person obtaining a
 *    copy of data files and any associated documentation (the "Data Files") or
 *    software and any associated documentation (the "Software") to deal in the
 *    Data Files or Software without restriction, including without limitation
 *    the rights to use, copy, modify, merge, publish, distribute, and/or sell
 *    copies of the Data Files or Software, and to permit persons to whom the
 *    Data Files or Software are furnished to do so, provided that either (a)
 *    this copyright and permission notice appear with all copies of the Data
 *    Files or Software, or (b) this copyright and permission notice appear in
 *    associated Documentation.
 *
 *    THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
 *    KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 *    THIRD PARTY RIGHTS.
 *
 *    IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE
 *    BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES,
 *    OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 *    WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
 *    ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA
 *    FILES OR SOFTWARE.
 *
 *    Except as contained in this notice, the name of a copyright holder shall
 *    not be used in advertising or otherwise to promote the sale, use or other
 *    dealings in these Data Files or Software without prior written
 *    authorization of the copyright holder.
 *
 */
#ifndef DCORE_EBCDIC_TABLES_H
#define DCORE_EBCDIC_TABLES_H

#include "dm_types.h"

namespace dcore {
namespace ebcdic {

/* a code page: its IBM number, the ISO 8859-1 octet of each EBCDIC
   octet */
struct Table {
	u16 id;
	u8 to_latin1[256];
};

const Table tables[] = {""")
    for cp in PAGES:
        vals, ucm = table(os.path.join(cpdir, f"cp{cp:03d}.go"))
        print(f"\t/* {ucm} */")
        print(f"\t{{ {cp}, {{")
        for row in range(16):
            v = ", ".join(f"0x{x:02X}" for x in vals[row * 16:row * 16 + 16])
            print(f"\t\t{v},")
        print("\t} },")
    print("""};

} // namespace ebcdic
} // namespace dcore

#endif""")


main()
