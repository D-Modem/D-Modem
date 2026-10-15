#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# run.sh [AT+MS=.. [answering profile]]: a call between two dmodem over SIP
# (RTP on 127.0.0.1) in a private FreeSWITCH (see fs.sh): the answering
# one serves the test BBS (asterisk/test/bbs_sim.py), the calling one
# (dmodem term,o) hands its data to the script playing the user.
#
#   run.sh                      V.34
#   run.sh AT+MS=122            V.22bis (also 22, 32, 132)
#   run.sh AT+MS=34 bbsexec     the BBS as a program on the pty
#   run.sh AT+MS=34 bbsalaw     the answering modem on A-law: transcoded
#   LAW=alaw run.sh             on A-law (PCMA)
#   ANSWER='AT+MS=132;A' run.sh the answering modem's command line (ATA)
#   EXPECT=regex                a line of the log the call must leave, e.g.
#                               'ANSWERER .*dmodem_rate=14400/'
. "$(dirname "$0")/fs.sh"
TERM_INIT=$(echo "${1:-}" | sed 's/^AT//')
profile=${2:-bbs}
codec=PCMU; [ "$LAW" = alaw ] && codec=PCMA
fs_start run
[ "$profile" = bbsexec ] && export BBS_EXEC=1
"$asterisk_test/bbs_sim.py" 150 &
sim=$!
sleep 0.5
fs_cli "bgapi originate {absolute_codec_string=$codec,origination_caller_id_number=5550100}sofia/modem/answer-$profile@127.0.0.1:5080 &dmodem(term,o)"
wait $sim; r=$?
fs_wait "ANSWERER dmodem_status" 1 10
grep -E "dmodem (answering|calling|connected|done)|mod_dptools.c:[0-9]* ANSWERER" "$run/log/freeswitch.log" |
	sed 's/^[^ ]* [^ ]* //'
if [ -n "${EXPECT:-}" ] && ! grep -qE "$EXPECT" "$run/log/freeswitch.log"; then
	echo "no line matches $EXPECT"; r=1
fi
fs_stop
exit $r
