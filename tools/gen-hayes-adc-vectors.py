#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# gen-hayes-adc-vectors.py - write link/test/hayes_adc_vectors.h from
# adc_vectors.json of the Hayes V-series firmware analysis.  That file
# has, for each frame of 5 connections, the DTE input and the output of
# the firmware's ADC encoder in a Z80 emulator (adc_vectors.py).
#
#   tools/gen-hayes-adc-vectors.py adc_vectors.json > link/test/hayes_adc_vectors.h

import json
import sys

HEAD = """/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    hayes_adc_vectors.h - test vectors for Hayes ADC: the output of the
 *                          firmware's own encoder (V-series Smartmodem
 *                          9600, firmware 1.4, bank 10 67C8h and 6818h)
 *                          in a Z80 emulator, for each frame of 5
 *                          connections
 *
 *    Made by tools/gen-hayes-adc-vectors.py from adc_vectors.json of the
 *    firmware analysis.  Each frame is the DTE's input and the
 *    information field that the firmware sent, in hexadecimal.
 *
 */

#ifndef DMODEM_LINK_TEST_HAYES_ADC_VECTORS_H
#define DMODEM_LINK_TEST_HAYES_ADC_VECTORS_H

struct AdcFrameVector {
\tconst char *in, *out;
};

struct AdcConnectionVector {
\tconst char *name;
\tconst AdcFrameVector *frames;
\tint count;
};
"""


def header(data):
    """Return the text of the header for the parsed JSON data."""
    lines = [HEAD]
    names = []
    for v in data["vectors"]:
        ident = "adc_" + v["name"].replace("-", "_")
        names.append((v["name"], ident, len(v["frames"])))
        lines.append("static const AdcFrameVector %s[] = {" % ident)
        for f in v["frames"]:
            lines.append('\t{ "%s",\n\t  "%s" },' % (f["in"], f["out"]))
        lines.append("};\n")
    lines.append("static const AdcConnectionVector ADC_VECTORS[] = {")
    for name, ident, count in names:
        lines.append('\t{ "%s", %s, %d },' % (name, ident, count))
    lines.append("};\n\n#endif")
    return "\n".join(lines) + "\n"


def main():
    """Read the JSON file named on the command line and write the header."""
    if len(sys.argv) != 2:
        sys.exit("usage: gen-hayes-adc-vectors.py adc_vectors.json")
    with open(sys.argv[1]) as f:
        sys.stdout.write(header(json.load(f)))


if __name__ == "__main__":
    main()
