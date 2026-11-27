#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# e2e.sh: the FreePBX module's output in a real Asterisk.  gen.php makes
# dmodem.conf and the app-dmodem context the way the module's page does;
# a private Asterisk (installed, with chan_pjsip; app_dmodem.so from the
# CMake build) runs them, an inbound "route" sends 5551000 to the
# profile's destination app-dmodem,bbs,1, and D-Modem (dmodemd) calls it
# over SIP and uses the BBS (frontend/asterisk/test/sip_call.py).  Then a call
# out: the module's dial-out (dial.php, as fwconsole dmodem dial) has the
# manager originate 5551000 in the dial-out context with profile term, to
# D-Modem listening (dmodemd -L), and the modem calls it.  Then the
# module's line check runs on the live endpoints through the manager:
# the test's endpoint allows both laws, which it must flag.  ARI=1: the
# module set to dmodem-ari, which takes the calls through Stasis().
# Last, fax to email: a fax machine (DModemSendFax) calls the fax
# recipient's destination app-dmodem,fax-sales,1; when the call ends, its
# mail handler runs fwconsole dmodem fax2mail (a stand-in: fax2mail.php),
# and the mail must have the text, a PDF of the pages and the TIFF.  Then
# the same through an inbound route (5551002 in ext-did-0001, as Core
# writes it) with D-Modem's fax detection to that destination; and a call
# without a fax on that route, which must go on to the route's own
# destination (app-dmodem,bbs,1).
# Then the module sends a fax (sendfax.php, as fwconsole dmodem sendfax:
# Ghostscript, a cover page) through the dial-out context to a fax
# machine there, and a fax to a number that does not answer.
set -e
here=$(cd "$(dirname "$0")" && pwd)
ast_test=$here/../../asterisk/test
build=${DMODEM_BUILD:-$here/../../../build}
run=${TMPDIR:-/tmp}/dmodem-freepbx-test
rm -rf "$run"; mkdir -p "$run/etc" "$run/run" "$run/log" "$run/spool" "$run/lib" "$run/mod"

engine=app
[ -z "$ARI" ] || engine=ari
php "$here/gen.php" "$run/etc" "$engine" "${ARI_PORT:-18088}"
cp "$ast_test/pjsip.conf" "$run/etc/"
cat > "$run/etc/extensions.conf" <<'EOT'
; the inbound route FreePBX would make: the DID to the profile's destination
[modem-in]
exten => 5551000,1,Goto(app-dmodem,bbs,1)

; the dial-out context (FreePBX's from-internal: its outbound routes)
[modem-out]
exten => _X.,1,Dial(PJSIP/modem,30)
 same => n,Hangup()
; a fax machine for the module's sent faxes: it keeps them in FAXRX
exten => 5559999,1,Answer()
 same => n,DModemReceiveFax(${FAXRX})
 same => n,Hangup()
; a number that does not answer
exten => 5559998,1,Hangup(17)

; a fax machine for the fax-to-email case: it sends FAXDOC
[fax-e2e-send]
exten => s,1,DModemSendFax(${FAXDOC})
 same => n,Hangup()

#include extensions_dmodem.conf
EOT
printf '[modules]\nautoload = yes\nnoload = chan_sip.so\n' > "$run/etc/modules.conf"
ami_port=${AMI_PORT:-15038}
printf '[general]\nenabled = yes\nport = %s\nbindaddr = 127.0.0.1\n[dmodem]\nsecret = dmodem\nread = all\nwrite = all\n' \
	"$ami_port" > "$run/etc/manager.conf"
. "$ast_test/ari_env.sh"
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
# (|| true: Asterisk is gone after 'core stop now', and set -e would make
# the failed kill the script's status)
trap 'kill $pid 2>/dev/null || true' EXIT
i=0; until $ast -rx "core waitfullybooted" >/dev/null 2>&1; do
	sleep 0.5; i=$((i+1)); [ $i -lt 60 ] || { tail "$run/console.log"; exit 1; }
done
echo "--- the context, as Asterisk has it"
$ast -rx "dialplan show app-dmodem" | grep -E "bbs|receivefax|DModem|Stasis" || true
if [ -n "$ARI" ]; then
	trap 'kill $pid 2>/dev/null || true; ari_stop' EXIT
	ari_start "$run" "$run/etc/dmodem.conf"
fi

r=0
echo "--- the call"
export SIP_LOGIN=modem:modem@127.0.0.1:5070 SIP_LOG=$run/dmodemd.log
"$ast_test/sip_call.py" "$build/dmodemd/dmodemd" 5551000 || r=1
sleep 2
grep -E "D-Modem profile bbs|DModem\(|Stasis\(|D-Modem bbs:" "$run/console.log" || true
grep -q "D-Modem profile bbs" "$run/console.log" ||
	{ echo "FAIL: the call did not go through app-dmodem,bbs,1"; r=1; }
[ -z "$ARI" ] || ari_log "$run"

echo "--- a call out (the module's dial-out)"
SIP_IN="php $here/dial.php $ami_port $engine term 5551000" SIP_LOG=$run/dmodemd-out.log \
	"$ast_test/sip_call.py" "$build/dmodemd/dmodemd" 5551000 || r=1
sleep 2
grep -E "Local/5551000@modem-out|DModem\(\"Local|Stasis\(\"Local" "$run/console.log" | head -5 || true
grep -q "Local/5551000@modem-out" "$run/console.log" ||
	{ echo "FAIL: no call out through the dial-out context"; r=1; }
[ -z "$ARI" ] || ari_log "$run"

echo "--- the line check"
php "$here/linecheck.php" "$ami_port" modem warn "$engine" ||
	{ echo "FAIL: the line check"; r=1; }

"$build/dsp/test/fax_doc" light "$run/doc.tif"
$ast -rx "dialplan set global FAXDOC $run/doc.tif" >/dev/null
# the mails that the mail handler made
mails() {
	ls "$run"/etc/mail-*.eml 2>/dev/null | wc -l
}
# waits for mail number $1, then checks it (the recipient, the text, a PDF
# of 2 pages and the TIFF); $2: what the case is
check_mail() {
	i=0; until [ "$(mails)" -ge "$1" ] || [ $i -ge 240 ]; do
		sleep 1; i=$((i+1))
	done
	mail=$(ls -tr "$run"/etc/mail-*.eml 2>/dev/null | sed -n "$1p")
	if [ -n "$mail" ] && python3 -I - "$mail" <<'EOT'
import email, sys
m = email.message_from_binary_file(open(sys.argv[1], "rb"))
parts = {p.get_content_type(): p.get_payload(decode=True) for p in m.walk()
         if not p.is_multipart()}
assert m["To"] == "sales@example.com", m["To"]
assert m["Subject"].startswith("Fax from "), m["Subject"]
assert b"2 pages, SUCCESS" in parts["text/plain"], parts["text/plain"]
assert parts["application/pdf"].startswith(b"%PDF-1.4"), "no PDF"
assert parts["application/pdf"].count(b"/Type /Page ") == 2, "not 2 pages"
assert parts["image/tiff"][:4] == b"II*\0", "no TIFF"
print("the mail: to %s, \"%s\", a PDF of 2 pages and the TIFF" % (m["To"], m["Subject"]))
EOT
	then
		echo "PASS: $2"
	else
		echo "FAIL: $2"; r=1
		cat "$run/etc/fax2mail.log" 2>/dev/null || true
		grep -E "fax-sales|fax2mail|DMODEMFAX|FaxDetect|faxdetect" "$run/console.log" | tail -20 || true
		[ -z "$ARI" ] || ari_log "$run"
	fi
}

echo "--- fax to email (the recipient sales' destination, its mail handler)"
$ast -rx "channel originate Local/fax-sales@app-dmodem/n extension s@fax-e2e-send" >/dev/null
check_mail 1 "fax to email"

echo "--- the inbound route's fax detection: a fax machine"
$ast -rx "channel originate Local/5551002@ext-did-0001/n extension s@fax-e2e-send" >/dev/null
check_mail 2 "the route heard the fax and sent it to the recipient"
grep -q "fax-sales@app-dmodem:1\] NoOp(\"Local/5551002@ext-did-0001" "$run/console.log" ||
	{ echo "FAIL: the fax did not go from the route to fax-sales"; r=1; }

echo "--- the inbound route's fax detection: no fax machine"
n=$(grep -c "D-Modem profile bbs" "$run/console.log")
$ast -rx "channel originate Local/5551002@ext-did-0001/n application Wait 12" >/dev/null
i=0; until [ "$(grep -c "D-Modem profile bbs" "$run/console.log")" -gt "$n" ] || [ $i -ge 30 ]; do
	sleep 1; i=$((i+1))
done
if [ "$(grep -c "D-Modem profile bbs" "$run/console.log")" -gt "$n" ]; then
	echo "PASS: no fax: the route went on to its destination"
else
	echo "FAIL: no fax: the route did not go on to its destination"; r=1
	grep -E "5551002|FaxDetect|faxdetect" "$run/console.log" | tail -10 || true
fi

echo "--- the fax log (the fax destination: no recipient, no mail)"
$ast -rx "channel originate Local/receivefax@app-dmodem/n extension s@fax-e2e-send" >/dev/null
i=0; until grep -q "logged" "$run/etc/fax2mail.log" 2>/dev/null || [ $i -ge 240 ]; do
	sleep 1; i=$((i+1))
done
if grep -q '^args: \["fax2mail","","[0-9.]*","SUCCESS","2",' "$run/etc/fax2mail.log" &&
   grep -q "logged" "$run/etc/fax2mail.log"; then
	echo "PASS: the fax log: the fax destination's fax logged"
else
	echo "FAIL: the fax log: no entry for the fax destination's fax"; r=1
	cat "$run/etc/fax2mail.log" 2>/dev/null || true
fi
# each fax gave the rate (the 8th argument); the data calls gave nothing
if grep '^args: ' "$run/etc/fax2mail.log" | grep -qv ',"[0-9][0-9]*"\]$'; then
	echo "FAIL: the fax log: a call without the rate"; r=1
	grep '^args: ' "$run/etc/fax2mail.log"
elif [ "$(grep -c '^args: ' "$run/etc/fax2mail.log")" -ne 3 ]; then
	echo "FAIL: the fax log: not 3 faxes (a call without a fax ran fax2mail)"; r=1
	grep '^args: ' "$run/etc/fax2mail.log"
else
	echo "PASS: the fax log: 3 faxes with their rate, none for the data calls"
fi

echo "--- send a fax (the module's queue, Ghostscript, the cover page)"
sendfax() {
	php "$here/sendfax.php" "$run/etc" "$engine" "$ami_port" "$@"
}
if ! command -v gs >/dev/null; then
	echo "FAIL: no Ghostscript (gs) for the send cases"; r=1
else
	$ast -rx "dialplan set global FAXRX $run/rx.tif" >/dev/null
	sendfax sendfax 5559999 "$run/doc.tif" cover=yes message="From the e2e test" \
		retries=0 notify=never || r=1
	i=0; until grep -q '"faxsent",.*"SUCCESS"' "$run/etc/fax2mail.log" 2>/dev/null || [ $i -ge 240 ]; do
		sleep 1; i=$((i+1))
	done
	sent=$(sendfax faxes | grep -c "SUCCESS" || true)
	pages=$(tiffinfo "$run/rx.tif" 2>/dev/null | grep -c "TIFF Directory" || true)
	want=$(( $(tiffinfo "$run/doc.tif" 2>/dev/null | grep -c "TIFF Directory") + 1 ))
	if [ "$sent" -ge 1 ] && [ "$pages" -eq "$want" ] && [ -z "$(sendfax faxjobs)" ]; then
		echo "PASS: send a fax: $pages pages (the cover page first) to the far machine, in the log"
	else
		echo "FAIL: send a fax (sent $sent, $pages pages of $want)"; r=1
		cat "$run/etc/fax2mail.log" 2>/dev/null || true
		grep -E "faxout|faxsent|DModemSendFax|sendfax" "$run/console.log" | tail -20 || true
		[ -z "$ARI" ] || ari_log "$run"
	fi
	echo "--- send a fax to a number that does not answer"
	sendfax sendfax 5559998 "$run/doc.tif" retries=0 notify=never || r=1
	i=0; until grep -q '"faxsent",.*"NOANSWER"' "$run/etc/fax2mail.log" 2>/dev/null || [ $i -ge 60 ]; do
		sleep 1; i=$((i+1))
	done
	if grep -q '"faxsent",.*"NOANSWER"' "$run/etc/fax2mail.log" && [ -z "$(sendfax faxjobs)" ]; then
		echo "PASS: no answer: the failed extension gave the result"
	else
		echo "FAIL: no answer: no result"; r=1
		grep -E "faxout|failed|5559998" "$run/console.log" | tail -10 || true
	fi
fi

$ast -rx "core stop now" >/dev/null 2>&1 || true
[ $r -eq 0 ] && echo "PASS" || echo "FAIL"
exit $r
