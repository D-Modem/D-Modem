# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# ari_env.sh: sourced by the tests for ARI=1, their calls through
# dmodem-ari (D-Modem as an ARI application, ../../ari) instead of
# app_dmodem:
#
#   ari_setup <etc dir>     Asterisk's HTTP server and an ARI user on
#                           127.0.0.1:$ARI_PORT (18088), every module
#                           loaded but app_dmodem, and the dialplan's
#                           DModem(...), DModemSendFax(...) and
#                           DModemReceiveFax(...) as Stasis(dmodem,...)
#   ari_start <run dir> <dmodem.conf>
#                           dmodem-ari started (its log in
#                           <run>/dmodem-ari.log), until it has the app;
#                           ARI_SIP=1: with t38 = yes, the calls' lines
#                           legs to its SIP side (127.0.0.1:$ARI_SIP_PORT,
#                           15190) on a PJSIP endpoint dmodem (added to
#                           pjsip.conf, with a transport of its own if it
#                           has none)
#   ari_stop
#   ari_log <run dir>       its lines about the calls and faxes
#
# $build is the CMake build.
ari_port=${ARI_PORT:-18088}
ari_sip_port=${ARI_SIP_PORT:-15190}

ari_setup() {
	sed -i -e 's/DModemSendFax(\(.*\))/Stasis(dmodem,sendfax,\1)/' \
		-e 's/DModemReceiveFax(\(.*\))/Stasis(dmodem,receivefax,\1)/' \
		-e 's/DModem(\([a-z]*\),o)/Stasis(dmodem,\1,o)/' \
		-e 's/DModem(\([a-z]*\))/Stasis(dmodem,\1)/' "$1/extensions.conf"
	printf '[general]\nenabled = yes\nbindaddr = 127.0.0.1\nbindport = %s\n' \
		"$ari_port" > "$1/http.conf"
	printf '[general]\nenabled = yes\n[dmodem]\ntype = user\nread_only = no\npassword = dmodem\n' \
		> "$1/ari.conf"
	printf '[modules]\nautoload = yes\nnoload = chan_sip.so\nnoload = app_dmodem.so\n' > "$1/modules.conf"
	[ -z "$ARI_SIP" ] || ari_sip "$1"
}

# ARI_SIP=1: the endpoint of dmodem-ari's SIP side
ari_sip() {
	tr=$(sed -n 's/^\[\(transport-[a-z]*\)\]$/\1/p' "$1/pjsip.conf" 2>/dev/null |
		head -1)
	if [ -z "$tr" ]; then
		tr=transport-dmodem
		printf '[%s]\ntype = transport\nprotocol = udp\nbind = 127.0.0.1:15160\n\n' \
			"$tr" >> "$1/pjsip.conf"
	fi
	cat >> "$1/pjsip.conf" <<EOT

[dmodem]
type = endpoint
transport = $tr
context = default
disallow = all
allow = ulaw
allow = alaw
direct_media = no
t38_udptl = yes
t38_udptl_ec = redundancy
aors = dmodem

[dmodem]
type = aor
contact = sip:dmodem@127.0.0.1:$ari_sip_port
EOT
}

ari_start() {
	[ -z "$ARI_SIP" ] || grep -q '^t38 = yes' "$2" ||
		sed -i "s|^\[general\]|[general]\nt38 = yes\nsip_endpoint = dmodem\nsip_bind = 127.0.0.1:$ari_sip_port|" "$2"
	"$build/ari/dmodem-ari" -c "$2" -v -u "http://127.0.0.1:$ari_port" \
		-U dmodem:dmodem > "$1/dmodem-ari.log" 2>&1 &
	ari_pid=$!
	i=0; until grep -q "app dmodem on" "$1/dmodem-ari.log"; do
		sleep 0.5; i=$((i+1))
		[ $i -lt 20 ] || { cat "$1/dmodem-ari.log"; return 1; }
	done
}

ari_stop() {
	[ -z "$ari_pid" ] || kill "$ari_pid" 2>/dev/null
	ari_pid=
}

ari_log() {
	grep -E "dmodem (answering|calling|connected|done)|fax|T.38|ERROR|WARNING" \
		"$1/dmodem-ari.log"
}
