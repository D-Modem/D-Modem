#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
"""haiku_sysroot.py: a Haiku (x86_64) sysroot for cmake/haiku.cmake,
from Haiku's and HaikuPorts' package repositories, on Linux.

  python3 cmake/haiku_sysroot.py DIR [--cache CACHE] [PACKAGE...]

Each package is downloaded (once, into CACHE: DIR/../haiku-packages by
default), checked against its repository's SHA-256 and extracted under
DIR/boot/system, as Haiku installs it.  The packages are those
cmake/haiku.cmake names (haiku, haiku_devel, gcc and its libraries,
libusb, openssl3), or the ones given.  Their pkg-config files name the
packages' own mount points (/packages/<package>/.self); here those are
/boot/system, where the files are.

Haiku's package format (a header, a heap of compressed chunks, a tree of
LEB128-tagged attributes) is read here: zlib heaps with Python's zlib,
zstd heaps with the system's libzstd (ctypes).  Files only: no extended
attributes, owners or times.
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import os
import re
import struct
import sys
import urllib.request
import zlib

# the repositories: Haiku R1/beta5's own, and HaikuPorts' for the rest
HAIKU_REPO = 'https://eu.hpkg.haiku-os.org/haiku/r1beta5/x86_64/current'
PORTS_REPO = 'https://eu.hpkg.haiku-os.org/haikuports/master/x86_64/current'
PACKAGES = ['haiku', 'haiku_devel', 'gcc', 'gcc_syslibs', 'gcc_syslibs_devel',
            'libusb', 'libusb_devel', 'openssl3', 'openssl3_devel']

# attribute ids (Haiku's PackageAttributes.h)
A_ENTRY, A_TYPE, A_PERMS, A_DATA, A_LINK = 0, 1, 2, 13, 14
A_NAME, A_ARCH, A_MAJOR, A_MINOR, A_MICRO = 15, 21, 22, 23, 24
A_RELEASE, A_CHECKSUM, A_PRE, A_PACKAGE = 25, 35, 36, 54
ARCH = {0: 'any', 1: 'x86', 2: 'x86_gcc2', 3: 'source', 4: 'x86_64'}

_zstd = None


# a zstd frame of ulen octets, decompressed (the system's libzstd)
def unzstd(c, ulen):
    global _zstd
    if _zstd is None:
        name = ctypes.util.find_library('zstd') or 'libzstd.so.1'
        _zstd = ctypes.CDLL(name)
        _zstd.ZSTD_decompress.restype = ctypes.c_size_t
        _zstd.ZSTD_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                          ctypes.c_char_p, ctypes.c_size_t]
        _zstd.ZSTD_isError.argtypes = [ctypes.c_size_t]
    out = ctypes.create_string_buffer(ulen)
    n = _zstd.ZSTD_decompress(out, ulen, c, len(c))
    if _zstd.ZSTD_isError(n) or n != ulen:
        sys.exit('haiku_sysroot: a zstd chunk does not decompress')
    return out.raw


# an unsigned LEB128 at b[p]: (its value, the next p)
def uleb(b, p):
    v = s = 0
    while True:
        c = b[p]
        p += 1
        v |= (c & 0x7f) << s
        s += 7
        if not c & 0x80:
            return v, p


# the heap of a package or repository file, uncompressed: its chunks
# (chunk octets each but the last), then their compressed sizes
def heap(d, start, comp, chunk, csize, usize):
    n = (usize + chunk - 1) // chunk
    tab = start + csize - 2 * (n - 1)
    out = bytearray()
    p = start
    for i in range(n):
        ulen = min(chunk, usize - i * chunk)
        clen = (struct.unpack_from('>H', d, tab + 2 * i)[0] + 1
                if i < n - 1 else tab - p)
        c = d[p:p + clen]
        p += clen
        if comp == 0 or clen == ulen:
            out += c
        elif comp == 1:
            out += zlib.decompress(c)
        elif comp == 2:
            out += unzstd(c, ulen)
        else:
            sys.exit('haiku_sysroot: heap compression %d' % comp)
    return bytes(out)


# n NUL-terminated strings at b[p]
def strings(b, p, n):
    t = []
    for _ in range(n):
        e = b.index(b'\0', p)
        t.append(b[p:e].decode('utf-8', 'replace'))
        p = e + 1
    return t


# a list of attributes at b[p], to its end tag: ([(id, value, children)],
# the next p).  A tag is 1 + id (7 bits), type (3), children (1),
# encoding (2): an integer's width, a string inline or in the table, raw
# data inline or in the heap.
def attrs(b, p, tab):
    out = []
    while True:
        tag, p = uleb(b, p)
        if tag == 0:
            return out, p
        tag -= 1
        aid, typ = tag & 0x7f, (tag >> 7) & 7
        kids, enc = (tag >> 10) & 1, (tag >> 11) & 3
        v = None
        if typ in (1, 2):
            w = 1 << enc
            v = int.from_bytes(b[p:p + w], 'big', signed=(typ == 1))
            p += w
        elif typ == 3:
            if enc == 0:
                e = b.index(b'\0', p)
                v = b[p:e].decode('utf-8', 'replace')
                p = e + 1
            else:
                i, p = uleb(b, p)
                v = tab[i]
        elif typ == 4:
            size, p = uleb(b, p)
            if enc == 1:
                off, p = uleb(b, p)
                v = ('heap', off, size)
            else:
                v = ('inline', b[p:p + size])
                p += size
        ch = []
        if kids:
            ch, p = attrs(b, p, tab)
        out.append((aid, v, ch))


# a version attribute as package file names spell it
def version(v, ch):
    k = {a: x for a, x, _ in ch}
    for a, sep in ((A_MINOR, '.'), (A_MICRO, '.'), (A_PRE, '~')):
        if k.get(a):
            v += sep + k[a]
    if A_RELEASE in k:
        v += '-%d' % k[A_RELEASE]
    return v


# a repository file's packages: {name: (file name, sha256)}
def repo_packages(d):
    if d[:4] != b'hpkr':
        sys.exit('haiku_sysroot: not a package repository')
    (hs, _, _, _, comp, chunk, cs, us, info, _, _, slen,
     scount) = struct.unpack_from('>HHQHHIQQIIQQQ', d, 4)
    h = heap(d, hs, comp, chunk, cs, us)
    tab = strings(h, info, scount)
    pk = {}
    for aid, _, ch in attrs(h, info + slen, tab)[0]:
        if aid != A_PACKAGE:
            continue
        k = {a: (x, c) for a, x, c in ch}
        name = k[A_NAME][0]
        arch = ARCH.get(k[A_ARCH][0], '?')
        pk[name] = ('%s-%s-%s.hpkg' % (name, version(*k[A_MAJOR]), arch),
                    k[A_CHECKSUM][0])
    return pk


# a package file's files under dest
def extract(d, dest):
    if d[:4] != b'hpkg':
        sys.exit('haiku_sysroot: not a package')
    (hs, _, _, _, comp, chunk, cs, us, alen, _, _, _, tlen, tslen,
     tscount) = struct.unpack_from('>HHQHHIQQIIIIQQQ', d, 4)
    h = heap(d, hs, comp, chunk, cs, us)
    toc = us - alen - tlen
    tab = strings(h, toc, tscount)

    # (an entry's children: its type, mode, data or link, its entries)
    def walk(entries, base):
        for aid, name, ch in entries:
            if aid != A_ENTRY:
                continue
            if '/' in name or name in ('', '.', '..'):
                sys.exit('haiku_sysroot: entry name %r' % name)
            k = {a: x for a, x, _ in ch}
            out = os.path.join(base, name)
            if k.get(A_TYPE, 0) == 1:
                os.makedirs(out, exist_ok=True)
                walk(ch, out)
                continue
            if os.path.lexists(out):
                os.remove(out)
            if k.get(A_TYPE, 0) == 2:
                os.symlink(k[A_LINK], out)
                continue
            r = k.get(A_DATA)
            data = (h[r[1]:r[1] + r[2]] if r and r[0] == 'heap' else
                    r[1] if r else b'')
            with open(out, 'wb') as f:
                f.write(data)
            os.chmod(out, k.get(A_PERMS, 0o644) & 0o755)

    os.makedirs(dest, exist_ok=True)
    walk(attrs(h, toc + tslen, tab)[0], dest)


# url's content, and the url it came from after redirects
def fetch(url):
    with urllib.request.urlopen(url) as r:
        return r.read(), r.geturl()


# the sysroot's pkg-config files with /boot/system for the packages'
# own mount points
def fix_pkgconfig(system):
    pc = os.path.join(system, 'develop', 'lib', 'pkgconfig')
    if not os.path.isdir(pc):
        return
    for f in os.listdir(pc):
        p = os.path.join(pc, f)
        if not f.endswith('.pc') or os.path.islink(p):
            continue
        s = open(p).read()
        t = re.sub(r'/packages/[^/\s]+/\.self', '/boot/system', s)
        if t != s:
            open(p, 'w').write(t)


# the sysroot: each package fetched (or taken from the cache), checked and
# extracted
def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('sysroot')
    ap.add_argument('--cache')
    ap.add_argument('--haiku-repo', default=HAIKU_REPO)
    ap.add_argument('--ports-repo', default=PORTS_REPO)
    ap.add_argument('packages', nargs='*')
    a = ap.parse_args()
    root = os.path.abspath(a.sysroot)
    cache = os.path.abspath(a.cache or os.path.join(root, '..',
                                                    'haiku-packages'))
    os.makedirs(cache, exist_ok=True)
    want = a.packages or PACKAGES
    found = {}
    for repo in (a.haiku_repo, a.ports_repo):
        d, url = fetch(repo + '/repo')
        base = url.rsplit('/', 1)[0]
        for name, (fn, sha) in repo_packages(d).items():
            found.setdefault(name, (base + '/packages/' + fn, fn, sha))
    system = os.path.join(root, 'boot', 'system')
    for name in want:
        if name not in found:
            sys.exit('haiku_sysroot: no package %s' % name)
        url, fn, sha = found[name]
        path = os.path.join(cache, fn)
        d = open(path, 'rb').read() if os.path.exists(path) else b''
        if hashlib.sha256(d).hexdigest() != sha:
            print('haiku_sysroot: fetching', fn)
            d = fetch(url)[0]
            if hashlib.sha256(d).hexdigest() != sha:
                sys.exit('haiku_sysroot: %s: SHA-256 differs from its '
                         'repository\'s' % fn)
            with open(path, 'wb') as f:
                f.write(d)
        print('haiku_sysroot: extracting', fn)
        extract(d, system)
    fix_pkgconfig(system)
    print('haiku_sysroot: done:', root)


if __name__ == '__main__':
    main()
