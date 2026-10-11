#!/bin/sh
# fax.sh: faxes over SIP in a private FreeSWITCH (see fs.sh), between
# dmodem's fax applications and against FreeSWITCH's own (mod_spandsp):
#   1. dmodem_send_fax calls dmodem bbs, which sees the CNG and receives
#      the fax into fax-dir by itself
#   2. dmodem_send_fax calls dmodem_receive_fax
#   3. txfax (spandsp) calls dmodem bbs
#   4. txfax calls dmodem_receive_fax
#   5. dmodem_send_fax calls rxfax (spandsp)
# The document is the two page test file of dsp/test/t4_test; each one
# received must be the one sent.  CASES="2 5" runs only those;
# FAX_MAX_RATE=9600: dmodem's fax no faster; DMODEM_DEBUG=3: the DSP
# traces in freeswitch.log.
. "$(dirname "$0")/fs.sh"
t4=$build/dsp/test/t4_test
[ -x "$t4" ] || { echo "build first: cmake --build $build" >&2; exit 1; }
fs_start fax
"$t4" light "$run/doc.tif" || exit 1
log=$run/log/freeswitch.log

# one fax: the destination, the sender's application, the file that
# should come; n: the faxes so far
n=0
fax() {
	n=$((n+1))
	[ "$3" = auto ] || rm -f "$3"
	fs_cli "bgapi originate {absolute_codec_string=${CODEC:-PCMU},origination_caller_id_number=5550100}sofia/modem/$1@127.0.0.1:5080 '&$2'" >/dev/null
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
	"$t4" compare "$run/doc.tif" "${f:-/nonexistent}" || r=1
}
last=0
r=0
for c in ${CASES:-1 2 3 4 5}; do case $c in
1)	echo "== 1: dmodem_send_fax -> dmodem"
	fax fax-auto "dmodem_send_fax($run/doc.tif +1 555 0100)" auto ;;
2)	echo "== 2: dmodem_send_fax -> dmodem_receive_fax"
	fax fax-recv "dmodem_send_fax($run/doc.tif +1 555 0100)" "$run/received.tif" ;;
3)	echo "== 3: txfax (spandsp) -> dmodem"
	fax fax-auto "txfax($run/doc.tif)" auto ;;
4)	echo "== 4: txfax (spandsp) -> dmodem_receive_fax"
	fax fax-recv "txfax($run/doc.tif)" "$run/received.tif" ;;
5)	echo "== 5: dmodem_send_fax -> rxfax (spandsp)"
	fax sp-recv "dmodem_send_fax($run/doc.tif +1 555 0100)" "$run/spandsp.tif" ;;
esac; done
fs_stop
exit $r
