#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# efax.sh: faxes of dmodemd (built in ../..) with efax on its pty (class
# 1) over SIP to a private Asterisk (installed, with chan_pjsip and
# res_fax_spandsp):
#   1. efax sends: dmodemd calls ReceiveFAX; the pages must have come as
#      efax sent them (it puts a header of its own on them)
#   2. efax receives: SendFAX calls dmodemd, which answers (S0=1); the
#      pages must be the document's
# over T.38 (the endpoint's t38_udptl): Asterisk asks for it in 1,
# dmodemd in 2; each fax must have gone over T.38 (dmodemd's log says
# so).  T38=no: the endpoint without it, the faxes on the audio
# (dmodemd's ask in 2 refused); DMODEMD_T38=no: dmodemd without it
# (SIP_T38=no), Asterisk's ask in 1 refused, and in 2 SendFAX on the
# audio from the start (F): on a channel with T.38 it would first wait
# 10.5 s for the receiver to ask for it, efax's first DIS unanswered,
# and efax 0.9a takes the OK of its abort of the +FRH=3 after it (T.31
# 6.2.2) for a frame of a length it never set.  The document is the two
# page test file of dsp/test/fax_doc.
# CASES="1" runs only those; DMODEMD_DEBUG=<n> dmodemd's -d (1),
# SIP_LOG=1 the SIP messages in console.log, FAX_DEBUG=1 spandsp's
# there, EFAX_V=<letters> efax's -v (ewin; ewinchmr: its commands and
# the modem's answers), EFAX_STRACE=1 what efax reads and writes in 2
# (efax2.strace).
here=$(cd "$(dirname "$0")" && pwd)
# the binaries of the CMake build (DMODEM_BUILD, default <repo>/build)
build=${DMODEM_BUILD:-$here/../../build}
t4=$build/dsp/test/fax_doc
dmodemd=$build/dmodemd/dmodemd
run=${TMPDIR:-/tmp}/dmodem-efax-test.$$
rm -rf "$run"; mkdir -p "$run/etc" "$run/run" "$run/log" "$run/spool" "$run/lib" "$run/mod"
[ -x "$t4" ] && [ -x "$dmodemd" ] ||
	{ echo "build first: cmake --build $build" >&2; exit 1; }
command -v efax >/dev/null || { echo "efax is not installed" >&2; exit 1; }
command -v tiffcp >/dev/null || { echo "tiffcp is not installed" >&2; exit 1; }
# (tiffcp: efax's received pages, a file each, into one)
"$t4" light "$run/doc.tif" || exit 1
# the same for efax to send (it reads a page's data up to its RTC)
"$t4" efax "$run/efax.tif" > /dev/null || exit 1
# Asterisk on 15260, dmodemd on 15280
sip=15260; ua=15280
t38=yes
[ "$T38" != no ] || t38=no
cp "$here/extensions.conf" "$run/etc/"
cat > "$run/etc/pjsip.conf" <<EOT
[transport-udp]
type = transport
protocol = udp
bind = 127.0.0.1:$sip

[modem]
type = endpoint
context = efax-in
disallow = all
allow = ulaw
auth = modem-auth
aors = modem
direct_media = no
rtp_symmetric = yes
t38_udptl = $t38
t38_udptl_ec = redundancy

[modem-auth]
type = auth
auth_type = userpass
username = modem
password = modem

[modem]
type = aor
max_contacts = 1
remove_existing = yes
EOT
printf '[general]\nrtpstart = 20000\nrtpend = 29999\n' > "$run/etc/rtp.conf"
printf '[modules]\nautoload = yes\nnoload = chan_sip.so\nnoload = app_dmodem.so\n' > "$run/etc/modules.conf"
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
dpid=
# (dmodemd waited for: its port free for the next run)
trap 'kill $pid $dpid 2>/dev/null; wait 2>/dev/null' EXIT
i=0; until $ast -rx "core waitfullybooted" >/dev/null 2>&1; do
	sleep 0.5; i=$((i+1)); [ $i -lt 60 ] || { tail "$run/console.log"; exit 1; }
done
$ast -rx "module show like fax" | grep -E "res_fax"
[ -z "$SIP_LOG" ] || $ast -rx "pjsip set logger on" >/dev/null
[ -z "$FAX_DEBUG" ] || $ast -rx "fax set debug on" >/dev/null
$ast -rx "dialplan set global FAXIN $run/doc.tif" >/dev/null
$ast -rx "dialplan set global SPOUT $run/spandsp.tif" >/dev/null
sendopt=f
[ "${DMODEMD_T38:-yes}" != no ] || sendopt=fF
$ast -rx "dialplan set global SENDOPT $sendopt" >/dev/null

# dmodemd, listening (registered: Asterisk's calls come to it), its pty
SIP_LOGIN=modem:modem@127.0.0.1:$sip SIP_PORT=$ua SIP_T38=${DMODEMD_T38:-yes} \
	stdbuf -oL -eL "$dmodemd" -d"${DMODEMD_DEBUG:-1}" -L \
	> "$run/dmodemd.log" 2>&1 &
dpid=$!
# (its link in /dev when root: the pty is a new one after each efax)
i=0; until pty=$(sed -n "s/^Use \`\([^']*\)' as modem device.*/\1/p" "$run/dmodemd.log") &&
	[ -n "$pty" ]; do
	sleep 0.5; i=$((i+1))
	[ $i -lt 20 ] || { echo "FAIL: dmodemd gave no pty"; tail "$run/dmodemd.log"; exit 1; }
done
i=0; until $ast -rx "pjsip show contacts" | grep -q "modem/sip:"; do
	sleep 0.5; i=$((i+1))
	[ $i -lt 20 ] || { echo "FAIL: dmodemd did not register"; exit 1; }
done
echo "dmodemd on $pty (T.38: $t38, dmodemd's ${DMODEMD_T38:-yes})"

# the console's lines of a kind, n of them in all (a few seconds then
# for the others)
wait_for() {
	i=0; until [ "$(grep -c "$1" "$run/console.log")" -ge "$2" ]; do
		sleep 1; i=$((i+1)); [ $i -lt ${FAX_WAIT:-180} ] || return 1
	done
	sleep 2
}
# dmodemd's calls over to T.38, so far
over_t38() {
	grep -c "T.38 on, to" "$run/dmodemd.log"
}
r=0
for c in ${CASES:-1 2}; do
	t=$(over_t38)
	case $c in
	1)
		echo "== 1: efax sends, dmodemd calls ReceiveFAX"
		rm -f "$run/spandsp.tif"
		# (X3: no dial tone to wait for on SIP)
		timeout 240 efax -d "$pty" -o1 -l "+1 555 0100" -h '' \
			-v ${EFAX_V:-ewin} -iX3 -t 5551000 "$run/efax.tif" \
			> "$run/efax1.log" 2>&1
		e=$?
		wait_for " EFAXRECV " 1
		grep " EFAXRECV " "$run/console.log" | tail -1
		[ $e -eq 0 ] || { echo "FAIL: efax returned $e"; tail -5 "$run/efax1.log"; r=1; }
		grep -q " EFAXRECV FAXSTATUS=SUCCESS .*FAXPAGES=2 " "$run/console.log" ||
			{ echo "FAIL: ReceiveFAX did not take 2 pages"; r=1; }
		# efax puts a header of its own on each page and reads the
		# pages its own way: each must have come as efax sent it, the
		# rows its log has
		sent=$(sed -n 's/.*sent \([0-9]*\)+\([0-9]*\) lines.*/\1 \2/p' \
			"$run/efax1.log" | awk '{ printf "%d ", $1 + $2 }')
		got=$(tiffinfo "$run/spandsp.tif" 2>/dev/null |
			sed -n 's/.*Image Length: \([0-9]*\).*/\1/p' |
			awk '{ printf "%d ", $1 }')
		echo "rows sent: $sent received: $got"
		[ -n "$sent" ] && [ "$sent" = "$got" ] ||
			{ echo "FAIL: not the rows efax sent"; r=1; }
		;;
	2)
		echo "== 2: SendFAX calls dmodemd, efax receives"
		rm -f "$run"/rx.*
		timeout 240 ${EFAX_STRACE:+strace -o $run/efax2.strace -e trace=read,write -s 200 -tt} \
			efax -d "$pty" -o1 -l "+1 555 0199" -v ${EFAX_V:-ewin} \
			-iS0=1 -w -r "$run/rx" > "$run/efax2.log" 2>&1 &
		epid=$!
		sleep 3
		$ast -rx "channel originate PJSIP/modem extension s@efax-send"
		wait $epid
		e=$?
		wait_for " EFAXSEND " 1
		grep " EFAXSEND " "$run/console.log" | tail -1
		[ $e -eq 0 ] || { echo "FAIL: efax returned $e"; tail -5 "$run/efax2.log"; r=1; }
		tiffcp "$run"/rx.0* "$run/rx.tif" 2>/dev/null
		"$t4" compare "$run/doc.tif" "$run/rx.tif" || r=1
		;;
	esac
	want=0; [ $t38 = no ] || [ "$DMODEMD_T38" = no ] || want=1
	got=$(($(over_t38) - t))
	if [ "$got" -ne "$want" ]; then
		echo "FAIL: $got of dmodemd's calls over T.38, not $want"
		r=1
	fi
done
$ast -rx "core stop now" >/dev/null 2>&1
exit $r
