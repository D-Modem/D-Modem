#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# voice.sh [ivr|wait|dsvd]: mod_dmodem's voice legs in a private
# FreeSWITCH (see fs.sh), over SIP: a caller speaks (2 s of a voice-like
# sound), then its modem calls (dmodem term,o), to dmodem voice-<case>,
# whose profile puts a person through to an extension on a loopback leg:
#   ivr    the extension records 2 s of the caller and ends: the modem
#          answers then (the recording must have the caller's sound)
#   wait   the extension stays: the caller's modem's calling tone (V.25's
#          CT, the caller a Czech modem: +GCI=2E) ends its leg, and the
#          modem answers
#   dsvd   no person: the caller's modem calls with DSVD (V.70) and opens
#          its voice channel (profile dsvdterm, voice-dsvd open), its leg
#          to talk (a voice), the answerer's to hear, which records 3 s
#          of it (it must be the voice) and hangs up: the voice channel
#          closes, and the data goes on (the BBS slow to take the call
#          meanwhile)
# then the modems connect and bbs_sim.py's session goes through
. "$(dirname "$0")/fs.sh"
case=${1:-ivr}
caller=speak-caller
[ "$case" != wait ] || TERM_INIT=+GCI=2E
if [ "$case" = dsvd ]; then
	caller=dsvd-caller
	export BBS_CONNECT_DELAY=25
fi
fs_start voice
python3 -I "$asterisk_test/speech.py" "$run/speech.wav"
"$asterisk_test/bbs_sim.py" 150 &
sim=$!
sleep 0.5
fs_cli "bgapi originate {absolute_codec_string=PCMU,origination_caller_id_number=5550100}sofia/modem/voice-$case@127.0.0.1:5080 $caller XML modem-in"
wait $sim; r=$?
fs_wait "ANSWERER dmodem_status" 1 10
log=$run/log/freeswitch.log
grep -E "dmodem (answering|calling|connected|done)|dmodem: (voice|the|a person|no person)|IVR|TALK|ANSWERER" "$log" |
	sed 's/^[^ ]* [^ ]* //' | cut -c1-200
case $case in
ivr) want="voice to loopback/ivr/modem-in|IVR answered|the extension's leg ended" ;;
wait) want="voice to loopback/wait/modem-in|IVR answered|the caller's modem asks for data" ;;
dsvd) want="voice to loopback/hear/modem-in|voice to loopback/talk/modem-in|IVR answered|TALK answered|the voice channel opened|the extension hung up: the voice channel closed" ;;
esac
IFS='|'
for w in $want "ANSWERER .*dmodem_rate=[0-9]"; do
	grep -qE "$w" "$log" || { echo "no line matches $w"; r=1; }
done
unset IFS
if [ "$case" != wait ]; then
	# the caller's sound reached the extension
	python3 -I "$asterisk_test/heard.py" "$run/heard.wav" || r=1
fi
fs_stop
exit $r
