#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
# compress_calls.sh <build dir> <out dir> <file>...: V.cvm calls between
# two open modems (half_open8, through netsim) with each file sent both
# ways (interop_call's BULK_FILE), for every line and compression below;
# one CSV line a call on stdout, each call's logs in <out dir>.
#
#   LINES="g711 amr122 amrwb"   G.711 alone, AMR 12.2, AMR-WB 23.85
#   COMPS="zstd_dict zstd v44_preset v44 none"
#   MS=V_CVM                    another +MS (34: V.34 at 9600 Hz)
#   JOBS=4                      calls at once
#
# The CSV: line, compression, file, exit status, CONNECT rate, octets,
# seconds to A, seconds to B (the call's clock), checks passed (2: both
# ways), what the link negotiated.  docs/link.md#compression-benchmark
# has results.

build=${1:?usage: $0 <build dir> <out dir> <file>...}
out=${2:?usage: $0 <build dir> <out dir> <file>...}
shift 2
[ $# -gt 0 ] || { echo "usage: $0 <build dir> <out dir> <file>..." >&2; exit 2; }
t=$(cd "$build/dsp/test" && pwd) || exit 1
mkdir -p "$out" || exit 1
out=$(cd "$out" && pwd)

# one call: line, compression, file
call() {
	local line=$1 comp=$2 file=$3
	local name d rc rate b n ta tb ok neg
	local half=$t/half_open8 at="ATX3;AT+MS=${MS:-V_CVM}"
	local envs=(NETSIM="$t/netsim" NET=8000:8000:ulaw BULK_FILE="$file")
	[ "${MS:-V_CVM}" = V_CVM ] || { half=$t/half_open; envs[1]=NET=9600:9600:ulaw; }
	name=$line.$comp.$(basename "$file")
	d=$out/$name
	mkdir -p "$d"
	envs+=(LOG_DIR="$d")
	case $line in
	amr122) envs+=(NET_CODEC=amr:12.2) ;;
	amrwb) envs+=(NET_CODEC=amrwb:23.85) ;;
	esac
	case $comp in
	zstd) envs+=(DSP_VCVM_V44DICT=0) ;;
	v44_preset) at="$at;AT+DSZ=0" ;;
	v44) at="$at;AT+DSZ=0"; envs+=(DSP_VCVM_V44DICT=0) ;;
	none) at="$at;AT%C0" ;;
	esac
	env "${envs[@]}" timeout 1800 "$t/interop_call" "$half" "$half" "$at" \
		> "$d/out.txt" 2>&1
	rc=$?
	rate=$(grep -o 'CONNECT [0-9]*' "$d/out.txt" | head -1 | cut -d' ' -f2)
	b=$(grep -o 'bench: .*' "$d/out.txt" | head -1)
	n=$(echo "$b" | awk '{print $2}')
	ta=$(echo "$b" | awk '{print $8}')
	tb=$(echo "$b" | awk '{print $12}')
	ok=$(grep -c 'ok=True' "$d/out.txt")
	neg=$(grep -h -o -E 'zstd: window [0-9]+, (dictionary [0-9a-f]+|no dictionary)|v44: preset dictionary [0-9]+|v44: [0-9]+ codewords' \
		"$d"/*.log 2>/dev/null | sort -u | tr '\n' ' ')
	echo "$line,$comp,$(basename "$file"),$rc,$rate,$n,$ta,$tb,$ok,$neg"
}

export -f call
export t out MS
echo "line,comp,file,rc,rate,octets,to_a_s,to_b_s,ok,negotiated"
for l in ${LINES:-g711 amr122 amrwb}; do
	for c in ${COMPS:-zstd_dict zstd v44_preset v44 none}; do
		for f in "$@"; do
			echo "$l $c $(cd "$(dirname "$f")" && pwd)/$(basename "$f")"
		done
	done
done | xargs -P "${JOBS:-4}" -L 1 bash -c 'call "$@"' _
