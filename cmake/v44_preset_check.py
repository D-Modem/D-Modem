#!/usr/bin/env python3
#
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
#    v44_preset_check.py - V.cvm's preset dictionary for V.44: the lines
#                          of link/v44_preset.cpp are those Annex E of the
#                          Recommendation lists (docs/cellular/annexes.tex,
#                          the verbatim* block after "The text"); and the
#                          header of zstd's dictionary (link/zstd_dict.cpp)
#                          the octets Annex F lists
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


def zstd_code(root):
    """the octets of zstd_dict.cpp's header[]"""
    src = open(root + '/link/zstd_dict.cpp').read()
    start = src.index('header[] = {')
    body = src[start:src.index('};', start)]
    return [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body)]


def zstd_annex(root):
    """the octets of Annex F's verbatim block (after each line's offset)"""
    tex = open(root + '/docs/cellular/annexes.tex').read()
    start = tex.index('sec:zstddict')
    begin = tex.index('\\begin{verbatim}\n', start) + len('\\begin{verbatim}\n')
    end = tex.index('\\end{verbatim}', begin)
    out = []
    for line in tex[begin:end].strip('\n').split('\n'):
        f = line.split()
        if int(f[0]) != len(out):
            raise ValueError('offset %s, expected %d' % (f[0], len(out)))
        out += [int(x, 16) for x in f[1:]]
    return out


def main():
    root = sys.argv[1]
    zc, za = zstd_code(root), zstd_annex(root)
    if zc != za:
        i = next((k for k in range(min(len(zc), len(za))) if zc[k] != za[k]),
                 min(len(zc), len(za)))
        print('zstd dictionary: octet %d differs (zstd_dict.cpp %d octets, '
              'the annex %d)' % (i, len(zc), len(za)))
        return 1
    print('zstd dictionary header: %d octets: the same' % len(zc))
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
