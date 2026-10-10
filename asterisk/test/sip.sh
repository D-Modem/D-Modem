#!/bin/sh
# sip.sh [AT commands, ';' separated]: D-Modem (slmodemd and d-modem, built
# in ../..) calls DModem(bbs) over SIP in a private Asterisk (installed
# with chan_pjsip, app_dmodem.so built in ..), or with INCOMING=1 answers
# DModem(term,o)'s call; see sip_call.py
here=$(cd "$(dirname "$0")" && pwd)
top=$here/../..
run=${TMPDIR:-/tmp}/dmodem-sip-test
rm -rf "$run"; mkdir -p "$run/etc" "$run/run" "$run/log" "$run/spool" "$run/lib" "$run/mod"
# CODEC=alaw (or ulaw): the call in that law only
if [ -n "$CODEC" ]; then
	sed "/^allow = /d; s/^disallow = all/disallow = all\nallow = $CODEC/" \
		"$here/pjsip.conf" > "$run/etc/pjsip.conf"
else
	cp "$here/pjsip.conf" "$run/etc/"
fi
# RECORD=1: the call's audio both ways in $run/rx.ulaw (from D-Modem) and
# $run/tx.ulaw (to it)
if [ -n "$RECORD" ]; then
	sed "s|^exten => _X.,1,DModem(bbs)|exten => _X.,1,MixMonitor($run/mix.wav,r($run/rx.ulaw)t($run/tx.ulaw))\n same => n,DModem(bbs)|" \
		"$here/extensions.conf" > "$run/etc/extensions.conf"
else
	cp "$here/extensions.conf" "$run/etc/"
fi
sed "s/^debug = .*/debug = ${DMODEM_DEBUG:-1}/" "$here/dmodem.conf" > "$run/etc/dmodem.conf"
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
$ast -f -n -T -vvvvv > "$run/console.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null' EXIT
i=0; until $ast -rx "core waitfullybooted" >/dev/null 2>&1; do
	sleep 0.5; i=$((i+1)); [ $i -lt 60 ] || { tail "$run/console.log"; exit 1; }
done
$ast -rx "pjsip show endpoints" | grep -E "Endpoint:.*modem"
export SIP_LOGIN=modem:modem@127.0.0.1:5070 SIP_LOG=$run/slmodemd.log
# INCOMING=1: Asterisk calls D-Modem (slmodemd -L), DModem(term,o) calls
if [ -n "$INCOMING" ]; then
	export SIP_IN="$ast -rx 'channel originate PJSIP/modem extension s@modem-call'"
fi
"$here/sip_call.py" "$top/slmodemd/slmodemd" "${DMODEM:-$top/d-modem}" 5551000 "$1"
r=$?
sleep 2
grep -E "DModem|DMODEMSTATUS" "$run/console.log"
$ast -rx "core stop now" >/dev/null 2>&1
exit $r
