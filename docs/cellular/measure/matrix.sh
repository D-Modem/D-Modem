#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# matrix.sh: D-Modem's modulations (half_open8 both ends) through each
# speech codec on netsim's line; one line per call: codec, +MS, result
# (CONNECT rate or none), the octets received each way and whether the
# data was right.  The modems run unpaced, so a call takes seconds.  The
# build is DMODEM_BUILD (default <repo>/build); the modem logs go to
# logs/ under the current directory.
here=$(cd "$(dirname "$0")" && pwd)
B=${DMODEM_BUILD:-$here/../../../build}/dsp/test
S=$(pwd)
export NETSIM=$B/netsim
export NET=8000:8000:ulaw STREAM=10
mkdir -p $S/logs
for codec in none amr:12.2 amr:7.4 amr:4.75 amrwb:23.85 amrwb:12.65 amrwb:6.6 \
	"amr:12.2 LOSS=1" "amr:12.2 LOSS=3" "amrwb:23.85 LOSS=1"; do
	spec=${codec%% *}
	loss=${codec#* LOSS=}
	[ "$loss" = "$codec" ] && loss=0
	for ms in 21 103 23 22 212 122 32 132 34; do
		d=$S/logs/$(echo "$codec" | tr ': =' '___')_$ms
		mkdir -p "$d"
		if [ "$spec" = none ]; then
			unset NET_CODEC NET_CODEC_LOSS
		else
			export NET_CODEC=$spec NET_CODEC_LOSS=$loss
		fi
		out=$(LOG_DIR=$d timeout 600 $B/interop_call $B/half_open8 $B/half_open8 "ATX3;AT+MS=$ms" 2>&1)
		echo "$out" > "$d/call.txt"
		con=$(echo "$out" | grep -o "CONNECT[^\\]*" | head -1 | tr -d '\r')
		recv=$(echo "$out" | grep "received" | sed 's/^\(.\) received \([0-9]*\)\/\([0-9]*\) ok=\(\w*\)/\1:\2\/\3:\4/' | tr '\n' ' ')
		res=$(echo "$out" | grep -E "^(PASS|FAIL)" | tail -1)
		printf '%-20s %-4s %-18s %-40s %s\n' "$codec" "$ms" "${con:-no connect}" "$recv" "$res"
	done
done
echo done
