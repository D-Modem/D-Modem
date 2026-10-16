#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# sip.sh [AT commands, ';' separated]: D-Modem (dmodemd, built in ../..)
# calls DModem(bbs) over SIP in a private Asterisk (installed
# with chan_pjsip; app_dmodem.so from the CMake build), or with INCOMING=1 answers
# DModem(term,o)'s call; see sip_call.py.  ARI=1: the same calls through
# dmodem-ari (D-Modem as an ARI application, ../../ari) instead of
# app_dmodem: Stasis(dmodem,...) in the dialplan
here=$(cd "$(dirname "$0")" && pwd)
# the binaries of the CMake build (DMODEM_BUILD, default <repo>/build)
build=${DMODEM_BUILD:-$here/../../build}
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
# ARI=1: the calls through dmodem-ari (ari_env.sh)
. "$here/ari_env.sh"
printf '[modules]\nautoload = yes\nnoload = chan_sip.so\n' > "$run/etc/modules.conf"
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
$ast -f -n -T -vvvvv > "$run/console.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null' EXIT
i=0; until $ast -rx "core waitfullybooted" >/dev/null 2>&1; do
	sleep 0.5; i=$((i+1)); [ $i -lt 60 ] || { tail "$run/console.log"; exit 1; }
done
$ast -rx "pjsip show endpoints" | grep -E "Endpoint:.*modem"
if [ -n "$ARI" ]; then
	trap 'kill $pid 2>/dev/null; ari_stop' EXIT
	ari_start "$run" "$run/etc/dmodem.conf" || exit 1
fi
export SIP_LOGIN=modem:modem@127.0.0.1:5070 SIP_LOG=$run/dmodemd.log
# INCOMING=1: Asterisk calls D-Modem (dmodemd -L), DModem(term,o) calls
if [ -n "$INCOMING" ]; then
	export SIP_IN="$ast -rx 'channel originate PJSIP/modem extension s@modem-call'"
fi
# VOICE=1: Asterisk calls D-Modem, which answers in voice mode and plays
# DTMF and hook flashes (sip_call.py); the dialplan sends it a digit.
# Asterisk's RTP debug shows the flashes come as RFC 4733 event 16.
if [ -n "$VOICE" ]; then
	export SIP_IN="$ast -rx 'channel originate PJSIP/modem extension s@modem-voice'"
	$ast -rx "rtp set debug on" >/dev/null
fi
"$here/sip_call.py" "$build/dmodemd/dmodemd" 5551000 "$1"
r=$?
sleep 2
if [ -n "$VOICE" ]; then
	# event 16 (0x10) as RTP RFC2833 packets from dmodemd: the start,
	# and the end bit on the last of each flash
	n=$(grep -c "Got  RTP RFC2833 .*event 00000010, end 1" "$run/console.log")
	echo "asterisk: $n end packets of event 16"
	grep -m 4 "Got  RTP RFC2833 .*event 00000010" "$run/console.log"
	[ "$n" -ge 2 ] || r=1
fi
grep -E "DModem|DMODEMSTATUS" "$run/console.log"
[ -z "$ARI" ] || ari_log "$run"
$ast -rx "core stop now" >/dev/null 2>&1
exit $r
