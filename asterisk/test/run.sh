#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# run.sh [AT+MS=.. [answering profile]]: a call between two DModem() over a Local channel in a
# private Asterisk (installed, with app_dmodem.so from the CMake build), see bbs_sim.py
#   LAW=alaw       both modems on an A-law channel
#   ANSWER=...     the answering profiles' answer command line (ATA), as
#                  dmodem.conf has it (\; for a ;)
#   EXPECT=regex   a line of the console the call must leave, e.g.
#                  'ANSWERER .*DMODEMRATE=14400/'
#   ARI=1          the calls through dmodem-ari instead (ari_env.sh)
set -e
here=$(cd "$(dirname "$0")" && pwd)
# the binaries of the CMake build (DMODEM_BUILD, default <repo>/build)
build=${DMODEM_BUILD:-$here/../../build}
run=${TMPDIR:-/tmp}/dmodem-ast-test
rm -rf "$run"; mkdir -p "$run/etc" "$run/run" "$run/log" "$run/spool" "$run/lib" "$run/mod"
cp "$here/modules.conf" "$run/etc/"
sed "s/DModem(bbs)/DModem(${2:-bbs})/" "$here/extensions.conf" > "$run/etc/extensions.conf"
export DMODEM_TEST="$here"
case ${2:-bbs} in
bbs) ;;
# bbs_sim.py serves the BBS on the profile's unix socket, and checks the
# header that comes first
bbsunix) export BBS_UNIX="$run/bbs.sock" ;;
*) export BBS_EXEC=1 ;;
esac
sed -e "${LAW:+s/^\(\[[a-z]*\]\)\$/\1\nlaw = $LAW/}" \
    -e "s/^\[term\]/[term]\ninit = ${1:-}/" \
    -e "s#@RUN@#$run#" \
    "$here/dmodem.conf" |
awk '{ print }
     /^\[bbs(exec|unix)?\]$/ && ENVIRON["ANSWER"] != "" {
	print "answer = " ENVIRON["ANSWER"] }' > "$run/etc/dmodem.conf"
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
$ast -rx "module show like dmodem"
if [ -n "$ARI" ]; then
	trap 'kill $pid 2>/dev/null; ari_stop' EXIT
	ari_start "$run" "$run/etc/dmodem.conf" || exit 1
fi
"$here/bbs_sim.py" 150 &
sim=$!
sleep 0.5
$ast -rx "channel originate Local/s@modem-answer/n extension s@modem-call"
set +e
wait $sim; r=$?
sleep 3
grep -E "DModem|DMODEMSTATUS" "$run/console.log"
[ -z "$ARI" ] || ari_log "$run"
if [ -n "${EXPECT:-}" ] && ! grep -qE "$EXPECT" "$run/console.log"; then
	echo "no line matches $EXPECT"; r=1
fi
$ast -rx "core stop now" >/dev/null 2>&1
exit $r
