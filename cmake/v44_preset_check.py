#!/usr/bin/env python3
#
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
#    v44_preset_check.py - V.cvm's preset dictionary for V.44: the lines
#                          of link/v44_preset.cpp are those Annex E of the
#                          Recommendation lists (docs/cellular/annexes.tex,
#                          the verbatim* block after "The text")
#
#    usage: v44_preset_check.py SOURCE_DIR
#
#    Prints the first difference and exits 1 if they differ.
#

import re
import sys


def code_lines(root):
    """the C string literals of v44_preset.cpp's lines[], unescaped"""
    src = open(root + '/link/v44_preset.cpp').read()
    start = src.index('lines[] = {')
    body = src[start:src.index('};', start)]
    return [re.sub(r'\\(.)', r'\1', m) for m in
            re.findall(r'^\t"((?:[^"\\]|\\.)*)",$', body, re.M)]


def annex_lines(root):
    """the lines of the annex's verbatim* block"""
    tex = open(root + '/docs/cellular/annexes.tex').read()
    start = tex.index('annex:v44dict')
    begin = tex.index('\\begin{verbatim*}\n', start) + len('\\begin{verbatim*}\n')
    end = tex.index('\\end{verbatim*}', begin)
    return tex[begin:end].rstrip('\n').split('\n')


def main():
    root = sys.argv[1]
    a, b = code_lines(root), annex_lines(root)
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else None
        y = b[i] if i < len(b) else None
        if x != y:
            print('line %d: v44_preset.cpp %r, the annex %r' % (i + 1, x, y))
            return 1
    n = sum(len(x) + 2 for x in a)
    print('%d lines, %d octets: the same' % (len(a), n))
    return 0


if __name__ == '__main__':
    sys.exit(main())
