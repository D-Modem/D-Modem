#!/bin/sh
# fax_spandsp.sh: faxes between DModem's fax and Asterisk's own (res_fax
# with spandsp, SendFAX / ReceiveFAX) over a Local channel in a private
# Asterisk (installed with res_fax_spandsp, app_dmodem.so built in ..):
#   1. SendFAX calls DModem(bbs), which sees the CNG and receives
#   2. SendFAX calls DModemReceiveFax
#   3. DModemSendFax calls ReceiveFAX
# Each received document must be the one sent.
here=$(cd "$(dirname "$0")" && pwd)
t4=$here/../../slmodemd/dsp/test/t4_test
run=${TMPDIR:-/tmp}/dmodem-faxsp-test.$$
rm -rf "$run"; mkdir -p "$run/etc" "$run/run" "$run/log" "$run/spool" "$run/lib" "$run/mod" "$run/fax"
[ -x "$t4" ] || make -C "$here/../../slmodemd/dsp/test" t4_test >/dev/null || exit 1
"$t4" light "$run/doc.tif" || exit 1
cp "$here/modules.conf" "$run/etc/"
printf 'load = res_clioriginate.so\nload = res_timing_timerfd.so\nload = res_fax.so\nload = res_fax_spandsp.so\n' >> "$run/etc/modules.conf"
cp "$here/extensions.conf" "$run/etc/"
sed "s|^\[general\]|[general]\nfax_dir = $run/fax\nfax_ident = +1 555 0199|" "$here/dmodem.conf" > "$run/etc/dmodem.conf"
for m in /usr/lib/asterisk/modules/*.so; do ln -s "$m" "$run/mod/"; done
ln -sf "$here/../app_dmodem.so" "$run/mod/app_dmodem.so"
cat > "$run/etc/asterisk.conf" <<EOT
[directories]
astetcdir => $run/etc
astmoddir => $run/mod
astvarlibdir => $run/lib
astdbdir => $run/lib
astkeydir => $run/lib
astdatadir => /var/lib/asterisk
astagidir => $run/lib
astspooldir => $run/spool
astrundir => $run/run
astlogdir => $run/log
[options]
EOT
ast="asterisk -C $run/etc/asterisk.conf"
$ast -f -n -vvvvv > "$run/console.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null' EXIT
i=0; until $ast -rx "core waitfullybooted" >/dev/null 2>&1; do
	sleep 0.5; i=$((i+1)); [ $i -lt 40 ] || exit 1
done
$ast -rx "module show like fax" | grep -E "res_fax|app_dmodem"
$ast -rx "dialplan set global FAXIN $run/doc.tif" >/dev/null
$ast -rx "dialplan set global FAXOUT $run/received.tif" >/dev/null
$ast -rx "dialplan set global SPOUT $run/spandsp.tif" >/dev/null

# the sender's result line (n of them in all)
wait_for() {
	i=0; until [ "$(grep -c "$1" "$run/console.log")" -ge "$2" ]; do
		sleep 1; i=$((i+1)); [ $i -lt 600 ] || return 1
	done
	sleep 2
}
r=0
echo "== 1: SendFAX (spandsp) -> DModem()"
$ast -rx "channel originate Local/s@fax-auto/n extension s@sp-send"
wait_for " SPSEND FAXSTATUS" 1
grep -E " SPSEND" "$run/console.log" | tail -1
grep -E "DModem fax received" "$run/console.log" | tail -1
f=$(ls "$run"/fax/*.tif 2>/dev/null | head -1)
"$t4" compare "$run/doc.tif" "${f:-/nonexistent}" || r=1

echo "== 2: SendFAX (spandsp) -> DModemReceiveFax"
$ast -rx "channel originate Local/s@fax-recv/n extension s@sp-send"
wait_for " SPSEND FAXSTATUS" 2
grep -E " SPSEND" "$run/console.log" | tail -1
grep -E "DModem fax received" "$run/console.log" | tail -1
"$t4" compare "$run/doc.tif" "$run/received.tif" || r=1

echo "== 3: DModemSendFax -> ReceiveFAX (spandsp)"
$ast -rx "channel originate Local/s@sp-recv/n extension s@fax-send"
wait_for " SEND DMODEMFAXSTATUS" 1
grep -E " SEND DMODEM" "$run/console.log" | tail -1
grep -E " SPRECV" "$run/console.log" | tail -1
"$t4" compare "$run/doc.tif" "$run/spandsp.tif" || r=1
$ast -rx "core stop now" >/dev/null 2>&1
exit $r
