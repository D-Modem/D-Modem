#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# t38.sh: faxes over T.38 through app_dmodem (app_dmodem.so from the
# CMake build) in a private Asterisk (installed, with chan_pjsip and
# res_fax_spandsp) that calls itself over PJSIP, out on loopout, in on
# loopin, T.38 on both:
#   1. SendFAX (spandsp) calls DModemReceiveFax
#   2. ReceiveFAX (spandsp) calls DModemSendFax
#   3. DModemSendFax calls DModemReceiveFax
#   4. SendFAX calls DModem(bbs), which hears the CNG and receives
#   5. SendFAX calls DModemReceiveFax from loopaudio, without T.38:
#      DModem's T.38 refused, the fax on the audio
#   6. SendFAX calls from loopaudio through Asterisk's T.38 gateway
#      (FAXOPT(gateway)) to DModem(bbs) over T.38
#   7. DModemSendFax calls through the gateway to ReceiveFAX on the
#      audio (loopaudio): the gateway asks for T.38 when it hears the
#      DIS, DModem's fax goes over
#   8. DModemReceiveFax calls DModem(bbs) and asks for T.38 before the
#      modems connect: DModem takes the call as a fax machine's, over
#      T.38 (two receivers: no document, only that is checked)
# Each document received (1 to 7) must be the one sent; all but 5 over
# T.38.
# ARI=1: through dmodem-ari instead (D-Modem as an ARI application,
# ../../ari, with t38 = yes: each call's line a leg to its own SIP side;
# the dialplan's DModem...() as Stasis(dmodem,...)).
# CASES="1 3" runs only those; DMODEM_DEBUG=3 for the DSP traces,
# SIP_LOG=1 for the SIP messages in console.log, FAX_DEBUG=1 for
# spandsp's (res_fax's) there.
here=$(cd "$(dirname "$0")" && pwd)
# the binaries of the CMake build (DMODEM_BUILD, default <repo>/build)
build=${DMODEM_BUILD:-$here/../../build}
t4=$build/dsp/test/fax_doc
run=${TMPDIR:-/tmp}/dmodem-t38-test.$$
rm -rf "$run"; mkdir -p "$run/etc" "$run/run" "$run/log" "$run/spool" "$run/lib" "$run/mod" "$run/fax"
[ -x "$t4" ] || { echo "build first: cmake --build $build" >&2; exit 1; }
"$t4" light "$run/doc.tif" || exit 1
# Asterisk on 15160 (and 15170 for the calls in), dmodem-ari's SIP side
# on 15190
sip=15160; loop=15170; leg=15190
cp "$here/extensions.conf" "$run/etc/"
sed -e "s|^\[general\]|[general]\nfax_dir = $run/fax\nfax_ident = +1 555 0199|" \
    -e "s/^debug = .*/debug = ${DMODEM_DEBUG:-1}/" "$here/dmodem.conf" > "$run/etc/dmodem.conf"
[ -z "$ARI" ] ||
	sed -i "s|^\[general\]|[general]\nt38 = yes\nsip_endpoint = dmodem\nsip_bind = 127.0.0.1:$leg|" \
		"$run/etc/dmodem.conf"
cat > "$run/etc/pjsip.conf" <<EOT
[global]
type = global
endpoint_identifier_order = username,ip

[transport-udp]
type = transport
protocol = udp
bind = 127.0.0.1:$sip

[transport-loop]
type = transport
protocol = udp
bind = 127.0.0.1:$loop

; dmodem-ari's SIP side (ARI=1): the legs of its calls
[dmodem]
type = endpoint
transport = transport-udp
context = t38-none
disallow = all
allow = ulaw
allow = alaw
direct_media = no
t38_udptl = yes
t38_udptl_ec = redundancy
aors = dmodem

[dmodem]
type = aor
contact = sip:dmodem@127.0.0.1:$leg

; Asterisk calling itself: out on loopout (T.38) or loopaudio (none),
; in on loopin (the From user)
[loopout]
type = endpoint
transport = transport-udp
context = t38-none
from_user = loopin
disallow = all
allow = ulaw
direct_media = no
t38_udptl = yes
t38_udptl_ec = redundancy
aors = loopout

[loopout]
type = aor
contact = sip:127.0.0.1:$loop

[loopaudio]
type = endpoint
transport = transport-udp
context = t38-none
from_user = loopin
disallow = all
allow = ulaw
direct_media = no
aors = loopaudio

[loopaudio]
type = aor
contact = sip:127.0.0.1:$loop

[loopin]
type = endpoint
transport = transport-loop
context = t38-in
identify_by = username
disallow = all
allow = ulaw
direct_media = no
t38_udptl = yes
t38_udptl_ec = redundancy
EOT
printf '[general]\nrtpstart = 20000\nrtpend = 29999\n' > "$run/etc/rtp.conf"
. "$here/ari_env.sh"
if [ -n "$ARI" ]; then
	ari_setup "$run/etc"
else
	printf '[modules]\nautoload = yes\nnoload = chan_sip.so\n' > "$run/etc/modules.conf"
fi
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
[ -n "$ARI" ] || ln -sf "$build/asterisk/app_dmodem.so" "$run/mod/app_dmodem.so"
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
trap 'kill $pid 2>/dev/null; ari_stop' EXIT
i=0; until $ast -rx "core waitfullybooted" >/dev/null 2>&1; do
	sleep 0.5; i=$((i+1)); [ $i -lt 60 ] || { tail "$run/console.log"; exit 1; }
done
$ast -rx "module show like fax" | grep -E "res_fax|app_dmodem"
[ -z "$SIP_LOG" ] || $ast -rx "pjsip set logger on" >/dev/null
[ -z "$FAX_DEBUG" ] || $ast -rx "fax set debug on" >/dev/null
if [ -n "$ARI" ]; then
	ari_start "$run" "$run/etc/dmodem.conf" || exit 1
	log=$run/dmodem-ari.log
else
	log=$run/console.log
fi
$ast -rx "dialplan set global FAXIN $run/doc.tif" >/dev/null
$ast -rx "dialplan set global FAXOUT $run/received.tif" >/dev/null
$ast -rx "dialplan set global SPOUT $run/spandsp.tif" >/dev/null

# the lines of a kind (in the console) n of them in all, a few seconds
# then for the others
wait_for() {
	i=0; until [ "$(grep -c "$1" "$run/console.log")" -ge "$2" ]; do
		sleep 1; i=$((i+1)); [ $i -lt ${FAX_WAIT:-300} ] || return 1
	done
	sleep 3
}
# DModem's faxes that went through over T.38, so far
over_t38() {
	grep -ci "dmodem fax \(sent\|received\): OK.*over T.38" "$log"
}
# DModem's fax lines since line $1 of its log
log_since() {
	tail -n +$(($1 + 1)) "$log" |
		grep -iE "dmodem fax|a fax machine|dmodem.*(ERROR|WARNING)"
}
r=0
n_send=0; n_in=0
for c in ${CASES:-1 2 3 4 5 6 7 8}; do
	t=$(over_t38); l=$(wc -l < "$log")
	rm -f "$run/received.tif" "$run/spandsp.tif"
	case $c in
	1)
		echo "== 1: SendFAX (spandsp, T.38) -> DModemReceiveFax"
		$ast -rx "channel originate PJSIP/recv@loopout extension s@sp-send"
		n_send=$((n_send+1)); n_in=$((n_in+1))
		wait_for " SPSEND FAXSTATUS" $n_send && wait_for " T38IN " $n_in
		grep -E " SPSEND| T38IN " "$run/console.log" | tail -2
		"$t4" compare "$run/doc.tif" "$run/received.tif" || r=1
		want=1
		;;
	2)
		echo "== 2: DModemSendFax -> ReceiveFAX (spandsp, T.38)"
		$ast -rx "channel originate PJSIP/send@loopout extension s@sp-recv"
		n_in=$((n_in+1))
		wait_for " SPRECV FAXSTATUS" 1 && wait_for " T38IN " $n_in
		grep -E " SPRECV| T38IN " "$run/console.log" | tail -2
		"$t4" compare "$run/doc.tif" "$run/spandsp.tif" || r=1
		want=1
		;;
	3)
		echo "== 3: DModemSendFax -> DModemReceiveFax"
		$ast -rx "channel originate PJSIP/recv@loopout extension s@t38-send"
		n_in=$((n_in+1))
		wait_for " T38SEND " 1 && wait_for " T38IN " $n_in
		grep -E " T38SEND | T38IN " "$run/console.log" | tail -2
		"$t4" compare "$run/doc.tif" "$run/received.tif" || r=1
		want=2
		;;
	4)
		echo "== 4: SendFAX (spandsp, T.38) -> DModem(bbs)"
		$ast -rx "channel originate PJSIP/auto@loopout extension s@sp-send"
		n_send=$((n_send+1)); n_in=$((n_in+1))
		wait_for " SPSEND FAXSTATUS" $n_send && wait_for " T38IN " $n_in
		grep -E " SPSEND| T38IN " "$run/console.log" | tail -2
		f=$(ls -t "$run"/fax/*.tif 2>/dev/null | head -1)
		"$t4" compare "$run/doc.tif" "${f:-/nonexistent}" || r=1
		want=1
		;;
	5)
		echo "== 5: SendFAX without T.38 -> DModemReceiveFax (on the audio)"
		$ast -rx "channel originate PJSIP/recv@loopaudio extension s@sp-send"
		n_send=$((n_send+1)); n_in=$((n_in+1))
		wait_for " SPSEND FAXSTATUS" $n_send && wait_for " T38IN " $n_in
		grep -E " SPSEND| T38IN " "$run/console.log" | tail -2
		"$t4" compare "$run/doc.tif" "$run/received.tif" || r=1
		want=0
		;;
	6)
		echo "== 6: SendFAX (audio) -> T.38 gateway -> DModem(bbs)"
		$ast -rx "channel originate PJSIP/gw@loopaudio extension s@sp-send"
		n_send=$((n_send+1)); n_in=$((n_in+2))
		wait_for " SPSEND FAXSTATUS" $n_send && wait_for " T38IN " $n_in
		grep -E " SPSEND| T38IN .*DMODEMSTATUS=FAX" "$run/console.log" |
			tail -2
		f=$(ls -t "$run"/fax/*.tif 2>/dev/null | head -1)
		"$t4" compare "$run/doc.tif" "${f:-/nonexistent}" || r=1
		want=1
		;;
	7)
		echo "== 7: DModemSendFax -> T.38 gateway -> ReceiveFAX (audio)"
		$ast -rx "channel originate PJSIP/gwrecv@loopout extension s@t38-send"
		n_in=$((n_in+2)); n_send7=$(grep -c " T38SEND " "$run/console.log")
		wait_for " T38SEND " $((n_send7+1)) && wait_for " T38IN " $n_in
		grep -E " T38SEND " "$run/console.log" | tail -1
		"$t4" compare "$run/doc.tif" "$run/spandsp.tif" || r=1
		want=1
		;;
	8)
		echo "== 8: DModemReceiveFax -> DModem(bbs), T.38 before the connect"
		$ast -rx "channel originate PJSIP/auto@loopout extension s@t38-recvcall"
		n_in=$((n_in+1))
		wait_for " T38IN " $n_in
		grep -E " T38IN " "$run/console.log" | tail -1
		if ! log_since "$l" | grep -q "a fax machine is calling (T.38)"; then
			echo "FAIL: DModem() took no fax over T.38"
			r=1
		fi
		want=0
		;;
	esac
	log_since "$l"
	got=$(($(over_t38) - t))
	if [ "$got" -ne "$want" ]; then
		echo "FAIL: $got of DModem's faxes over T.38, not $want"
		r=1
	fi
done
$ast -rx "core stop now" >/dev/null 2>&1
exit $r
