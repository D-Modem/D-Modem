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

$ast -rx "core stop now" >/dev/null 2>&1 || true
[ $r -eq 0 ] && echo "PASS" || echo "FAIL"
exit $r
