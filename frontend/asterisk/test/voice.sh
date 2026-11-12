#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# voice.sh [ivr|wait|dsvd]: a caller speaks, then its modem calls (DModem(term,o)
# after 2 s of a voice-like sound), to DModem(voice), whose profile has
# voice = <ivr|wait>@voice-ivr: the person goes to that extension, and
#   ivr    the extension records 2 s of the caller and ends: the modem
#          answers then (the recording must have the caller's sound)
#   wait   the extension stays: the caller's modem's calling tone (V.25's
#          CT, the caller a Czech modem) ends its leg, and the modem
#          answers
#   dsvd   no person: the caller's modem calls with DSVD (V.70) and opens
#          its voice channel (voice_dsvd = open), the answerer's has
#          voice_dsvd: once they are connected, each side's voice
#          channel goes to its extension (talk and hear@voice-ivr) while
#          the data goes on.  (A Local channel has no clock: the modems
#          run faster than real time, so the extensions' audio is not
#          checked here; dmodem_bridge_test's DsvdVoiceToExtension
#          does that)
# then the modems connect and bbs_sim.py's session goes through, as in
# run.sh (in a private Asterisk, with app_dmodem.so from the CMake build).
# ARI=1: the same through dmodem-ari (ari_env.sh): its extension's leg a
# Local channel of its own in the call's bridge, the modem hearing a
# snoop of the caller meanwhile; DSVD's leg in a bridge with an external
# media channel of its own (the bridges' clock: real time)
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${DMODEM_BUILD:-$here/../../../build}
case=${1:-ivr}
[ "$case" != wait ] || CT=1
caller=voice-caller
if [ "$case" = dsvd ]; then
	caller=dsvd-caller
	DSVD=1
fi
run=${TMPDIR:-/tmp}/dmodem-voice-test
rm -rf "$run"; mkdir -p "$run/etc" "$run/run" "$run/log" "$run/spool" "$run/lib" "$run/mod"
{ cat "$here/modules.conf"
  printf 'load = app_playback.so\nload = app_playtones.so\nload = app_record.so\nload = format_sln.so\n'
} > "$run/etc/modules.conf"
{ printf '[globals]\nVOICE_RUN=%s\n\n' "$run"; cat "$here/extensions.conf"; } \
	> "$run/etc/extensions.conf"
# (wait: the calling modem sends V.25's calling tone, as a Czech one does:
# most countries' modems send none, and wait for the answer in silence)
voice=$case
[ "$case" != dsvd ] || voice=hear
sed -e "s/@VOICE@/$voice@voice-ivr/" \
    -e "${DSVD:+s/^voice_listen = .*/voice_dsvd = yes/}" \
    -e "${CT:+s/^\[term\]/[term]\ninit = +GCI=2E/}" \
    "$here/dmodem.conf" > "$run/etc/dmodem.conf"
python3 -I "$here/speech.py" "$run/speech.sln"
. "$here/ari_env.sh"
[ -z "$ARI" ] || ari_setup "$run/etc"
astmod=${AST_MODULES:-$(pkg-config --variable=modulesdir asterisk 2>/dev/null || true)}
for d in /usr/lib/*/asterisk/modules /usr/lib/asterisk/modules; do
	[ -n "$astmod" ] || [ ! -d "$d" ] || astmod=$d
done
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
	sleep 0.5; i=$((i+1)); [ $i -lt 40 ] || { tail "$run/console.log"; exit 1; }
done
if [ -n "$ARI" ]; then
	trap 'kill $pid 2>/dev/null; ari_stop' EXIT
	ari_start "$run" "$run/etc/dmodem.conf" || exit 1
fi
"$here/bbs_sim.py" 150 &
sim=$!
sleep 0.5
$ast -rx "channel originate Local/s@voice-answer/n extension s@$caller"
set +e
wait $sim; r=$?
sleep 3
log=$run/console.log
if [ -n "$ARI" ]; then
	cat "$run/console.log" "$run/dmodem-ari.log" > "$run/all.log"
	log=$run/all.log
	grep -E "dmodem (answering|calling|connected|done)|voice|extension|person|caller's modem|ERROR|WARNING" \
		"$run/dmodem-ari.log" | grep -v " dmodem: "
fi
grep -E "DModem|DMODEMSTATUS|IVR" "$run/console.log"
# the leg to the extension, and how it ended
case $case in
ivr) want="the extension's leg ended" ;;
wait) want="the caller's modem asks for data" ;;
dsvd) want="voice to Local/talk@voice-ivr|Executing .talk@voice-ivr|the voice channel opened" ;;
esac
IFS='|'
for w in "voice to Local/$voice@voice-ivr" "Executing .$voice@voice-ivr" $want \
	 "ANSWERER .*DMODEMRATE=[0-9]"; do
	grep -qE "$w" "$log" || { echo "no line matches $w"; r=1; }
done
unset IFS
if [ "$case" = ivr ]; then
	# the caller's sound reached the extension
	python3 -I "$here/heard.py" "$run/heard.sln" || r=1
fi
$ast -rx "core stop now" >/dev/null 2>&1
exit $r
