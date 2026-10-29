#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# build_asterisk.sh <prefix>: Asterisk 22 (LTS) from source into <prefix>,
# its pjproject bundled, with spandsp's fax (res_fax_spandsp).  Ubuntu
# 24.04's Asterisk 20.6 has a T.38 gateway (FAXOPT(gateway)) that passes
# no fax: its audio side stops sending once the gateway starts, between
# its own SendFAX and ReceiveFAX too (t38.sh's cases 6 and 7).
#
# The packages it builds with, on Debian or Ubuntu:
#   build-essential pkg-config libssl-dev libjansson-dev libxml2-dev
#   libsqlite3-dev uuid-dev libedit-dev libspandsp-dev libtiff-dev
#   libcurl4-openssl-dev bzip2 patch
#
# SRC=<dir> keeps the sources there (default: <prefix>/src).  The tests
# then want: <prefix>/sbin on PATH, AST_MODULES=<prefix>/lib/asterisk/modules,
# AST_DATA=<prefix>/var/lib/asterisk, and app_dmodem built with
# -DASTERISK_INCLUDE_DIR=<prefix>/include.
set -e
version=22.11.0
prefix=${1:?usage: build_asterisk.sh <prefix>}
mkdir -p "$prefix"
prefix=$(cd "$prefix" && pwd)
src=${SRC:-$prefix/src}
mkdir -p "$src"
jobs=$(nproc 2>/dev/null || echo 4)

cd "$src"
if [ ! -d "asterisk-$version" ]; then
	curl -fsSL -o "asterisk-$version.tar.gz" \
		"https://downloads.asterisk.org/pub/telephony/asterisk/releases/asterisk-$version.tar.gz"
	tar xzf "asterisk-$version.tar.gz"
	rm "asterisk-$version.tar.gz"
fi
cd "asterisk-$version"
./configure -q --prefix="$prefix" --with-pjproject-bundled >/dev/null
make -s menuselect.makeopts >/dev/null
# (spandsp's fax must be there: the tests' fax and T.38)
menuselect/menuselect --enable res_fax_spandsp menuselect.makeopts
grep -q "^MENUSELECT_RES=.*res_fax_spandsp" menuselect.makeopts &&
	{ echo "res_fax_spandsp cannot be built (libspandsp-dev?)"; exit 1; } || true
make -s -j"$jobs" >/dev/null
make -s install >/dev/null
# (the headers too, for app_dmodem: make install leaves them out)
make -s install-headers >/dev/null
test -f "$prefix/lib/asterisk/modules/res_fax_spandsp.so"
test -f "$prefix/include/asterisk/module.h"
echo "Asterisk $version in $prefix"
