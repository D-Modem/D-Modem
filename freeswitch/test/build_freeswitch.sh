#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# build_freeswitch.sh <prefix>: FreeSWITCH 1.10.12 from source into
# <prefix>, with what it needs that distributions lack: sofia-sip, and
# FreeSWITCH's spandsp at the commit its 1.10.12 build uses (mod_spandsp
# does not build with spandsp's master).  A minimal set of modules, enough
# for the tests here (SIP, the dialplan, mod_spandsp's fax).
#
# The packages it builds with, on Debian or Ubuntu:
#   build-essential pkg-config autoconf automake libtool libtool-bin yasm
#   libssl-dev libcurl4-openssl-dev libpcre3-dev libspeex-dev
#   libspeexdsp-dev libsqlite3-dev libedit-dev libldns-dev libjpeg-dev
#   uuid-dev libtiff-dev libsndfile1-dev zlib1g-dev libpq-dev
#
# SRC=<dir> keeps the sources there (default: <prefix>/src); a build of
# the tests then wants -DFREESWITCH_PKGCONFIG=<prefix>/lib/pkgconfig and
# FS_PREFIX=<prefix>.
set -e
prefix=${1:?usage: build_freeswitch.sh <prefix>}
mkdir -p "$prefix"
prefix=$(cd "$prefix" && pwd)
src=${SRC:-$prefix/src}
mkdir -p "$src"
jobs=$(nproc 2>/dev/null || echo 4)
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CPPFLAGS="-I$prefix/include"
export LDFLAGS="-L$prefix/lib -Wl,-rpath,$prefix/lib"

cd "$src"
[ -d sofia-sip ] ||
	git clone -q --depth 1 -b v1.13.17 https://github.com/freeswitch/sofia-sip
[ -d spandsp ] || git clone -q https://github.com/freeswitch/spandsp
[ -d freeswitch ] ||
	git clone -q --depth 1 -b v1.10.12 https://github.com/signalwire/freeswitch

if [ ! -f "$prefix/lib/pkgconfig/sofia-sip-ua.pc" ]; then
	(cd sofia-sip && ./bootstrap.sh && ./configure -q --prefix="$prefix" &&
	 make -s -j"$jobs" && make -s install)
fi
if [ ! -f "$prefix/lib/pkgconfig/spandsp.pc" ]; then
	(cd spandsp && git checkout -q 0d2e6ac65e0e8f53d652665a743015a88bf048d4 &&
	 ./bootstrap.sh && ./configure -q --prefix="$prefix" &&
	 make -s -j"$jobs" && make -s install)
fi

cd freeswitch
[ -x configure ] || ./bootstrap.sh -j
# the modules: no signalwire (libks), verto, video, languages or
# PostgreSQL (mod_pgsql does not build with libpq 17)
for m in applications/mod_signalwire endpoints/mod_verto languages/mod_lua \
	applications/mod_av formats/mod_shout applications/mod_cv \
	endpoints/mod_skinny codecs/mod_opus databases/mod_pgsql; do
	sed -i "s|^$m\$|#$m|" modules.conf
done
./configure -q --prefix="$prefix" --disable-libvpx --disable-libyuv \
	--without-python --without-python3
make -s -j"$jobs"
make -s install
echo "FreeSWITCH in $prefix"
