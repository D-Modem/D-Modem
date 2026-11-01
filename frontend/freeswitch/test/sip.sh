#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# sip.sh [AT commands, ';' separated]: D-Modem (dmodemd, built in ../../..)
# registers with a private FreeSWITCH (see fs.sh) and calls dmodem bbs, or
# with INCOMING=1 answers FreeSWITCH's call, dmodem term o calling; see
# frontend/asterisk/test/sip_call.py.  CODEC=PCMA: the call in A-law.
. "$(dirname "$0")/fs.sh"
fs_start sip
export SIP_LOGIN=modem:modem@127.0.0.1:5080 SIP_LOG=$run/dmodemd.log
if [ -n "$INCOMING" ]; then
	# the contact dmodemd registered, then the call to it
	cli="$FS_PREFIX/bin/fs_cli -H 127.0.0.1 -P 8022 -p dmodem -x"
	cat > "$run/call_in.sh" <<EOT
#!/bin/sh
c=\$($cli "sofia_contact modem/modem@127.0.0.1")
$cli "bgapi originate {absolute_codec_string=${CODEC:-PCMU}}\$c &dmodem(term,o)"
EOT
	chmod +x "$run/call_in.sh"
	export SIP_IN="$run/call_in.sh"
else
	# the law of the call: what FreeSWITCH offers and takes
	[ -n "$CODEC" ] && fs_cli "sofia profile modem stop" >/dev/null &&
		sed -i "s/value=\"PCMU,PCMA\"/value=\"$CODEC\"/" "$run/conf/freeswitch.xml" &&
		fs_cli "reloadxml" >/dev/null && fs_cli "sofia profile modem start" >/dev/null &&
		sleep 2
fi
"$asterisk_test/sip_call.py" "$build/dmodemd/dmodemd" 5551000 "$1"
r=$?
sleep 2
grep -E "dmodem (answering|calling|connected|done)|mod_dptools.c:[0-9]* ANSWERER" "$run/log/freeswitch.log" |
	sed 's/^[^ ]* [^ ]* //'
fs_stop
exit $r
