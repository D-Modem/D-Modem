#!/bin/sh
# run.sh [AT+MS=.. [answering profile]]: a call between two DModem() over a Local channel in a
# private Asterisk (installed, with app_dmodem.so from the CMake build), see bbs_sim.py
set -e
here=$(cd "$(dirname "$0")" && pwd)
# the binaries of the CMake build (DMODEM_BUILD, default <repo>/build)
build=${DMODEM_BUILD:-$here/../../build}
run=${TMPDIR:-/tmp}/dmodem-ast-test
rm -rf "$run"; mkdir -p "$run/etc" "$run/run" "$run/log" "$run/spool" "$run/lib" "$run/mod"
cp "$here/modules.conf" "$run/etc/"
sed "s/DModem(bbs)/DModem(${2:-bbs})/" "$here/extensions.conf" > "$run/etc/extensions.conf"
export DMODEM_TEST="$here"
[ "${2:-bbs}" = bbs ] || export BBS_EXEC=1
# LAW=alaw: both modems on an A-law channel
sed -e "${LAW:+s/^\(\[[a-z]*\]\)\$/\1\nlaw = $LAW/}" \
    -e "s/^\[term\]/[term]\ninit = ${1:-}/" \
    "$here/dmodem.conf" > "$run/etc/dmodem.conf"
for m in /usr/lib/asterisk/modules/*.so; do ln -s "$m" "$run/mod/"; done
ln -sf "$build/asterisk/app_dmodem.so" "$run/mod/app_dmodem.so"
cat > "$run/etc/asterisk.conf" <<EOT
[directories]
astetcdir => $run/etc
astmoddir => $run/mod
astvarlibdir => $run/lib
astdbdir => $run/lib
astkeydir => $run/lib
astdatadir => /var/lib/asterisk
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
i=0; until $ast -rx "core waitfullybooted" >/dev/null 2>&1; do sleep 0.5; i=$((i+1)); [ $i -lt 40 ]; done
$ast -rx "module show like dmodem"
"$here/bbs_sim.py" 150 &
sim=$!
sleep 0.5
$ast -rx "channel originate Local/s@modem-answer/n extension s@modem-call"
set +e
wait $sim; r=$?
sleep 3
grep -E "DModem|DMODEMSTATUS" "$run/console.log"
$ast -rx "core stop now" >/dev/null 2>&1
exit $r
