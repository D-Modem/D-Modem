#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# fax.sh: faxes over SIP in a private FreeSWITCH (see fs.sh), between
# dmodem's fax applications and against FreeSWITCH's own (mod_spandsp):
#   1. dmodem_send_fax calls dmodem bbs, which sees the CNG and receives
#      the fax into fax-dir by itself
#   2. dmodem_send_fax calls dmodem_receive_fax
#   3. txfax (spandsp) calls dmodem bbs
#   4. txfax calls dmodem_receive_fax
#   5. dmodem_send_fax calls rxfax (spandsp)
#   6. txfax calls dmodem_receive_fax, txfax without T.38
#      (fax_enable_t38=false): dmodem's T.38 refused
#   7. (T38=yes only) dmodem_receive_fax calls dmodem bbs and asks for
#      T.38 before the modems connect: dmodem takes the call as a fax
#      machine's, over T.38 (two receivers: no document, only that is
#      checked)
# The document is the two page test file of dsp/test/fax_doc; each one
# received must be the one sent.  On the audio, or with T38=yes over
# T.38 (dmodem's fax-t38 and mod_spandsp's enable-t38 and
# enable-t38-request on; 6 on the audio still).  CASES="2 5" runs only
# those; FAX_MAX_RATE=9600: dmodem's fax no faster; DMODEM_DEBUG=3: the
# DSP traces in freeswitch.log.
. "$(dirname "$0")/fs.sh"
t4=$build/dsp/test/fax_doc
[ -x "$t4" ] || { echo "build first: cmake --build $build" >&2; exit 1; }
fs_start fax
"$t4" light "$run/doc.tif" || exit 1
log=$run/log/freeswitch.log

# dmodem's faxes that went through over T.38, so far
over_t38() {
	grep -c "dmodem fax \(sent\|received\): OK.*over T.38" "$log"
}
# one fax: the destination, the sender's application, the file that
# should come (none: no document to compare), how many of dmodem's
# faxes go over T.38 (with T38=yes),
# the calling leg's variables (default fax_enable_t38 as T38: without
# it FreeSWITCH refuses an offer); n: the faxes so far
n=0
fax() {
	n=$((n+1))
	[ "$3" = auto ] || [ "$3" = none ] || rm -f "$3"
	t=$(over_t38)
	v=${5:-fax_enable_t38=$([ "$T38" = yes ] && echo true || echo false)}
	fs_cli "bgapi originate {absolute_codec_string=${CODEC:-PCMU},origination_caller_id_number=5550100,$v}sofia/modem/$1@127.0.0.1:5080 '&$2'" >/dev/null
	# both ends are done when the call is gone
	sleep 5
	i=0; until fs_cli "show calls count" | grep -q "^0 total"; do
		sleep 1; i=$((i+1)); [ $i -lt ${FAX_WAIT:-300} ] || break
	done
	sleep 1
	grep -E "dmodem fax (sent|received)|Fax processing|remote station|Fax successfully|ECM" "$log" |
		sed 's/^[^ ]* [^ ]* //' | tail -n +$((last+1))
	last=$(grep -cE "dmodem fax (sent|received)|Fax processing|remote station|Fax successfully|ECM" "$log")
	f=$3
	[ "$f" = auto ] && f=$(ls -t "$run"/fax/*.tif 2>/dev/null | head -1)
	[ "$f" = none ] || "$t4" compare "$run/doc.tif" "${f:-/nonexistent}" || r=1
	want=0; [ "$T38" != yes ] || want=$4
	got=$(($(over_t38) - t))
	if [ "$got" -ne "$want" ]; then
		echo "FAIL: $got of dmodem's faxes over T.38, not $want"
		r=1
	fi
}
last=0
r=0
for c in ${CASES:-1 2 3 4 5 6 7}; do case $c in
1)	echo "== 1: dmodem_send_fax -> dmodem"
	fax fax-auto "dmodem_send_fax($run/doc.tif +1 555 0100)" auto 2 ;;
2)	echo "== 2: dmodem_send_fax -> dmodem_receive_fax"
	fax fax-recv "dmodem_send_fax($run/doc.tif +1 555 0100)" "$run/received.tif" 2 ;;
3)	echo "== 3: txfax (spandsp) -> dmodem"
	fax fax-auto "txfax($run/doc.tif)" auto 1 ;;
4)	echo "== 4: txfax (spandsp) -> dmodem_receive_fax"
	fax fax-recv "txfax($run/doc.tif)" "$run/received.tif" 1 ;;
5)	echo "== 5: dmodem_send_fax -> rxfax (spandsp)"
	fax sp-recv "dmodem_send_fax($run/doc.tif +1 555 0100)" "$run/spandsp.tif" 1 ;;
6)	echo "== 6: txfax without T.38 -> dmodem_receive_fax"
	fax fax-recv "txfax($run/doc.tif)" "$run/received.tif" 0 fax_enable_t38=false ;;
7)	[ "$T38" = yes ] || continue
	echo "== 7: dmodem_receive_fax -> dmodem, T.38 before the connect"
	l=$(wc -l < "$log")
	fax fax-auto "dmodem_receive_fax($run/caller.tif)" none 0
	if ! tail -n +$((l + 1)) "$log" |
	    grep -q "a fax machine is calling (T.38)"; then
		echo "FAIL: dmodem took no fax over T.38"
		r=1
	fi ;;
esac; done
fs_stop
exit $r
