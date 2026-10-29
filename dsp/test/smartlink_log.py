#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
"""smartlink_log.py [log]: the original library's "$!$ " lines (its
edprintf) decoded in place, the rest as it is (stdin without a file).

Each byte is two characters: its high and low nibbles, each plus the next
of a key that goes round (4 6 2 7 1 9 3 5 8 7, from the start of each
line) plus '0'; '????' pads the end."""
import sys

KEY = (4, 6, 2, 7, 1, 9, 3, 5, 8, 7)


# one line's encoded text (what follows "$!$ ") decoded: the key from its
# start, the "?" padding dropped, an odd last character ignored; the
# text (latin-1) without its trailing newline
def decode(s):
    out, k = bytearray(), 0
    s = s.rstrip('?')
    for j in range(0, len(s) - 1, 2):
        hi = ord(s[j]) - 0x30 - KEY[k]
        k = (k + 1) % len(KEY)
        lo = ord(s[j + 1]) - 0x30 - KEY[k]
        k = (k + 1) % len(KEY)
        out.append(((hi & 15) << 4) | (lo & 15))
    return out.decode('latin-1').rstrip('\n')


# the log (argv[1], else stdin) to stdout, each "$!$ " line's text
# decoded after a "| "
def main():
    f = open(sys.argv[1], errors='replace') if len(sys.argv) > 1 else sys.stdin
    for line in f:
        p = line.find('$!$ ')
        if p >= 0:
            line = line[:p] + '| ' + decode(line[p + 4:].strip()) + '\n'
        sys.stdout.write(line)


if __name__ == '__main__':
    try:
        main()
    except BrokenPipeError:
        pass
