#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# fax.sh: faxes between DModem's applications over a Local channel in a
# private Asterisk (installed, app_dmodem.so from the CMake build):
#   1. DModemSendFax calls DModem(bbs), which sees the CNG and receives
#      the fax into fax_dir by itself
#   2. DModemSendFax calls DModemReceiveFax(file)
#   3. DMODEMFAXOPT(): two files in one fax at 9600 bit/s without ECM,
#      the receiver's lowest rate 9600, options d and F; res_fax's
#      variables (FAXSTATUS and the rest) set too
#   4. the receiver's lowest rate 9600 and the sender's top rate 4800:
#      no fax (FAXERROR TRAINING)
#   5. option T on a Local channel (no T.38): no fax (FAXERROR T38)
#   6. the CLI: dmodem fax show stats, settings, sessions and
#      capabilities; the AMI actions registered; AMI (ami_events.py on
#      AMI_PORT, default 15038): DModemFaxStatus (option s), the
#      DModemSendFax and DModemReceiveFax events, DModemFaxStats
# With ARI=1, 3 to 5 set the channel variables DMODEMFAXOPT_<ITEM> in
# place of DMODEMFAXOPT() (ari_env.sh), and DMODEMFAXOPT()'s results are
# not checked (app_dmodem only); 6 checks dmodem-ari's user events
# (AMI UserEvent) and its SIGUSR1 report in place of the CLI and the
# AMI actions
# The document is the two page test file of dsp/test/fax_doc;
# the received ones must be the same.  ARI=1: DModem's side of the
# calls through dmodem-ari instead (ari_env.sh).
here=$(cd "$(dirname "$0")" && pwd)
# the binaries of the CMake build (DMODEM_BUILD, default <repo>/build)
build=${DMODEM_BUILD:-$here/../../../build}
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
ami_port=${AMI_PORT:-15038}
cat > "$run/etc/manager.conf" <<EOT
[general]
enabled = yes
bindaddr = 127.0.0.1
port = $ami_port
[test]
secret = test
read = all
write = all
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

# checks that the console has the line, else fails (r=1)
has() {
	if grep -qE "$1" "$run/console.log"; then
		echo "PASS: $2"
	else
		echo "FAIL: $2"; r=1
	fi
}
# the results of DMODEMFAXOPT() read, as the console has them: empty
# with ARI=1 (the variables DMODEMFAXOPT_PAGES and the others are none)
if [ -z "$ARI" ]; then
	opt_pages="pages=4 error=NO_ERROR"
	opt_file="filename=$run/doc.tif session=[0-9]+"
else
	opt_pages="pages= error="
	opt_file="filename= session="
fi
python3 -I "$here/ami_events.py" "$ami_port" "$run/ami.txt" &
sleep 1
if true; then
	rm -f "$run/received.tif"
	$ast -rx "dialplan set global FAXMIN 9600" >/dev/null
	$ast -rx "dialplan set global FAXMAX 9600" >/dev/null
	$ast -rx "channel originate Local/s@faxopt-recv/n extension s@faxopt-send"
	wait_for " SENDOPT " 1 && wait_for " RECVOPT " 1
	grep -E " (ROPT|SOPT|RECVOPT|SENDOPT) " "$run/console.log"
	has " ROPT minrate=9600 localstationid=\+1 555 0177" "DMODEMFAXOPT() read back"
	has " SOPT maxrate=9600 ecm=no" "DMODEMFAXOPT() maxrate, ecm"
	has " SENDOPT FAXSTATUS=SUCCESS FAXERROR=NO_ERROR FAXPAGES=4 FAXBITRATE=9600 DMODEMFAXECM=no $opt_file" "two files at 9600 bit/s without ECM"
	has " RECVOPT FAXSTATUS=SUCCESS FAXERROR=NO_ERROR FAXPAGES=4 FAXBITRATE=9600 LOCALSTATIONID=\+1 555 0177 REMOTESTATIONID=\+1 555 0100 $opt_pages" "res_fax's variables, receiver"
	if [ -z "$ARI" ]; then
		has "DModem fax [0-9]+: " "option d: the phases in the log"
	else
		ari_log "$run" > "$run/ari-lines.txt"
		grep -qE "dmodem fax [0-9]+: " "$run/ari-lines.txt" &&
			echo "PASS: option d: the phases in the log" ||
			{ echo "FAIL: option d: the phases in the log"; r=1; }
	fi

	$ast -rx "dialplan set global FAXMAX 4800" >/dev/null
	$ast -rx "channel originate Local/s@faxopt-recv/n extension s@faxopt-send"
	wait_for " SENDOPT " 2 && wait_for " RECVOPT " 2
	grep -E " RECVOPT " "$run/console.log" | tail -1
	has " RECVOPT FAXSTATUS=FAILED FAXERROR=TRAINING FAXPAGES=0" "the lowest rate refused"

	$ast -rx "channel originate Local/s@faxopt-t38only/n application Wait 20"
	wait_for " T38ONLY " 1
	grep -E " T38ONLY " "$run/console.log"
	has " T38ONLY FAXSTATUS=FAILED FAXERROR=T38" "option T without T.38"
fi
if [ -z "$ARI" ]; then

	$ast -rx "dmodem fax show stats" > "$run/stats.txt"
	cat "$run/stats.txt"
	if grep -qE "Completed faxes: +[0-9]+" "$run/stats.txt" &&
	   grep -qE "Failed faxes: +[1-9]" "$run/stats.txt" &&
	   grep -qE "T38 +1" "$run/stats.txt" &&
	   grep -qE "Current sessions: +0" "$run/stats.txt"; then
		echo "PASS: dmodem fax show stats"
	else
		echo "FAIL: dmodem fax show stats"; r=1
	fi
	if $ast -rx "dmodem fax show settings" | grep -q "Fax directory: *$run/fax" &&
	   $ast -rx "dmodem fax show sessions" | grep -q "0 active DModem fax sessions" &&
	   $ast -rx "dmodem fax show capabilities" | grep -q "T.38"; then
		echo "PASS: dmodem fax show settings, sessions, capabilities"
	else
		echo "FAIL: dmodem fax show settings, sessions, capabilities"; r=1
	fi
fi
# the AMI events (app_dmodem's, or dmodem-ari's UserEvents)
sleep 1
if [ -z "$ARI" ]; then
	evs="Event: DModemFaxStatus|Event: DModemReceiveFax|Event: DModemSendFax|ActionID: stats1"
else
	evs="UserEvent: DModemFaxStatus|UserEvent: DModemReceiveFax|UserEvent: DModemSendFax"
fi
evs="$evs|Error: TRAINING|Error: T38|Status: SUCCESS"
echo "$evs" | tr '|' '\n' | while read -r e; do
	grep -q "^$e" "$run/ami.txt" || echo "$e" >> "$run/ami-missing.txt"
done
if [ -s "$run/ami-missing.txt" ]; then
	sed 's/^/FAIL: AMI /' "$run/ami-missing.txt"; r=1
else
	echo "PASS: AMI events"
fi
if [ -n "$ARI" ]; then
	kill -USR1 "$ari_pid"
	sleep 1
	if grep -qE "dmodem-ari: fax: 0 now; transmit attempts [0-9]+, receive attempts [0-9]+, completed [0-9]+, failed [1-9]" "$run/dmodem-ari.log" &&
	   grep -q "dmodem-ari: fax: failed T38: 1" "$run/dmodem-ari.log"; then
		echo "PASS: dmodem-ari's SIGUSR1 report"
	else
		echo "FAIL: dmodem-ari's SIGUSR1 report"; r=1
	fi
fi
if [ -z "$ARI" ]; then
	for a in DModemFaxSessions DModemFaxSession DModemFaxStats; do
		if $ast -rx "manager show command $a" | grep -q "$a"; then
			echo "PASS: AMI action $a"
		else
			echo "FAIL: AMI action $a"; r=1
		fi
	done
fi
[ -z "$ARI" ] || ari_log "$run"
$ast -rx "core stop now" >/dev/null 2>&1
exit $r
