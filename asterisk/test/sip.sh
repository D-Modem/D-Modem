#!/bin/sh
# sip.sh [AT commands, ';' separated]: D-Modem (slmodemd and d-modem, built
# in ../..) calls DModem(bbs) over SIP in a private Asterisk (installed
# with chan_pjsip, app_dmodem.so built in ..); see sip_call.py
here=$(cd "$(dirname "$0")" && pwd)
top=$here/../..
run=${TMPDIR:-/tmp}/dmodem-sip-test
rm -rf "$run"; mkdir -p "$run/etc" "$run/run" "$run/log" "$run/spool" "$run/lib" "$run/mod"
cp "$here/extensions.conf" "$here/pjsip.conf" "$here/dmodem.conf" "$run/etc/"
printf '[modules]\nautoload = yes\nnoload = chan_sip.so\n' > "$run/etc/modules.conf"
for m in /usr/lib/asterisk/modules/*.so; do ln -s "$m" "$run/mod/"; done
ln -sf "$here/../app_dmodem.so" "$run/mod/app_dmodem.so"
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
i=0; until $ast -rx "core waitfullybooted" >/dev/null 2>&1; do
	sleep 0.5; i=$((i+1)); [ $i -lt 60 ] || { tail "$run/console.log"; exit 1; }
done
$ast -rx "pjsip show endpoints" | grep -E "Endpoint:.*modem"
SIP_LOGIN=modem:modem@127.0.0.1:5070 SIP_LOG=$run/slmodemd.log \
	"$here/sip_call.py" "$top/slmodemd/slmodemd" "${DMODEM:-$top/d-modem}" 5551000 "$1"
r=$?
sleep 2
grep -E "DModem|DMODEMSTATUS" "$run/console.log"
$ast -rx "core stop now" >/dev/null 2>&1
exit $r
