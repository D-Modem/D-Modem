#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# fax.sh: faxes between DModem's applications over a Local channel in a
# private Asterisk (installed, app_dmodem.so from the CMake build):
#   1. DModemSendFax calls DModem(bbs), which sees the CNG and receives
#      the fax into fax_dir by itself
#   2. DModemSendFax calls DModemReceiveFax(file)
# The document is the two page test file of dsp/test/fax_doc;
# the received ones must be the same.  ARI=1: DModem's side of the
# calls through dmodem-ari instead (ari_env.sh).
here=$(cd "$(dirname "$0")" && pwd)
# the binaries of the CMake build (DMODEM_BUILD, default <repo>/build)
build=${DMODEM_BUILD:-$here/../../build}
t4=$build/dsp/test/fax_doc
run=${TMPDIR:-/tmp}/dmodem-fax-test.$$
rm -rf "$run"; mkdir -p "$run/etc" "$run/run" "$run/log" "$run/spool" "$run/lib" "$run/mod" "$run/fax"
[ -x "$t4" ] || { echo "build first: cmake --build $build" >&2; exit 1; }
"$t4" light "$run/doc.tif" || exit 1
cp "$here/modules.conf" "$run/etc/"
echo "load = res_clioriginate.so" >> "$run/etc/modules.conf"
cp "$here/extensions.conf" "$run/etc/"
sed "s|^\[general\]|[general]\nfax_dir = $run/fax\nfax_ident = +1 555 0199|" "$here/dmodem.conf" > "$run/etc/dmodem.conf"
. "$here/ari_env.sh"
[ -z "$ARI" ] || ari_setup "$run/etc"
# Asterisk's modules (AST_MODULES, else where asterisk.pc says, else
# Debian's or the default place)
astmod=${AST_MODULES:-$(pkg-config --variable=modulesdir asterisk 2>/dev/null || true)}
for d in /usr/lib/*/asterisk/modules /usr/lib/asterisk/modules; do
	[ -n "$astmod" ] || [ ! -d "$d" ] || astmod=$d
done
# its data, the XML documentation it needs to start (AST_DATA, else
# Debian's or the default place)
astdata=${AST_DATA:-}
for d in /usr/share/asterisk /var/lib/asterisk; do
	[ -n "$astdata" ] || [ ! -f "$d/documentation/core-en_US.xml" ] ||
		astdata=$d
done
for m in "$astmod"/*.so; do ln -s "$m" "$run/mod/"; done
ln -sf "$build/asterisk/app_dmodem.so" "$run/mod/app_dmodem.so"
cat > "$run/etc/asterisk.conf" <<EOT
[directories]
astetcdir => $run/etc
astmoddir => $run/mod
astvarlibdir => $run/lib
astdbdir => $run/lib
astkeydir => $run/lib
astdatadir => ${astdata:-/var/lib/asterisk}
astagidir => $run/lib
astspooldir => $run/spool
astrundir => $run/run
astlogdir => $run/log
[options]
EOT
ast="asterisk -C $run/etc/asterisk.conf"
$ast -f -n -vvvvv > "$run/console.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null' EXIT
i=0; until $ast -rx "core waitfullybooted" >/dev/null 2>&1; do
	sleep 0.5; i=$((i+1)); [ $i -lt 40 ] || { tail "$run/console.log"; exit 1; }
done
if [ -n "$ARI" ]; then
	trap 'kill $pid 2>/dev/null; ari_stop' EXIT
	ari_start "$run" "$run/etc/dmodem.conf" || exit 1
fi
$ast -rx "dialplan set global FAXIN $run/doc.tif" >/dev/null
$ast -rx "dialplan set global FAXOUT $run/received.tif" >/dev/null

# waits for the line of the given kind (AUTO, RECV, SEND) twice
wait_for() {
	i=0; until [ "$(grep -c "$1" "$run/console.log")" -ge "$2" ]; do
		sleep 1; i=$((i+1)); [ $i -lt 400 ] || return 1
	done
}
r=0
$ast -rx "channel originate Local/s@fax-auto/n extension s@fax-send"
wait_for " SEND DMODEMFAXSTATUS" 1 && wait_for " AUTO DMODEMSTATUS" 1
grep -E " (AUTO|SEND) DMODEM" "$run/console.log"
f=$(ls "$run"/fax/*.tif 2>/dev/null | head -1)
"$t4" compare "$run/doc.tif" "${f:-/nonexistent}" || r=1

$ast -rx "channel originate Local/s@fax-recv/n extension s@fax-send"
wait_for " SEND DMODEMFAXSTATUS" 2 && wait_for " RECV DMODEMFAXSTATUS" 1
grep -E " RECV DMODEM" "$run/console.log"
grep -E " SEND DMODEM" "$run/console.log" | tail -1
"$t4" compare "$run/doc.tif" "$run/received.tif" || r=1
[ -z "$ARI" ] || ari_log "$run"
$ast -rx "core stop now" >/dev/null 2>&1
exit $r
