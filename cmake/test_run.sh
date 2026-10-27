#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
# test_run.sh <log root> [--name <test>] <command> [args...]: one ctest
# test, its output kept if it fails (cmake/test_logs.cmake): ctest's
# launcher for every test.
#
# The test's directory is <run>/<test>/, <run> the directory of this
# ctest run (test_logs.sh begin wrote it to <log root>/.run-<ctest's
# pid>; without it <log root>/adhoc); a repeat of the test in the same
# run (ctest --repeat) gets <test>.2, <test>.3, ...  In it:
#
#   output.log   the test's stdout and stderr (still passed on to ctest)
#   info.txt     the test, its command, the debug levels, start time
#   result.txt   <test>[.<n>] PASS|FAIL|SKIP <exit status> <seconds>
#
# The test finds its directory in DMODEM_TEST_DIR, for its own files
# (the interop calls' modem logs, ...).  Its result.txt line goes to
# <run>/results.txt as well; a test that passed has its directory
# deleted then, unless DMODEM_TEST_KEEP_PASSED=1 (a failure's, and a
# skipped test's, stay).
#
# Verbose logging: the debug levels of the modems and libraries the tests
# run are DMODEM_TEST_DEBUG (5: what the modems log of each step, short
# of every bit and symbol, which is 6 and up), each only if not already
# set: DSP_DEBUG, CORE_DEBUG, DMODEM_DEBUG, DBG (interop_call's modems),
# DMODEMD_DEBUG (dmodemd's -d), LINK_DEBUG, SPANDSP_DEBUG.
# DMODEM_TEST_DEBUG=0 leaves them all as they are.
#
# gtest's discovery (--gtest_list_tests) runs the program as it is.

# now, in seconds with a fraction: bash 5's EPOCHREALTIME, else GNU date's
# %N, else (macOS's date, bash 3.2) whole seconds
now() {
	if [ -n "$EPOCHREALTIME" ]; then
		echo "${EPOCHREALTIME/,/.}"
	else
		local t
		t=$(date +%s.%N)
		case $t in
		*N) date +%s ;;
		*) echo "$t" ;;
		esac
	fi
}

root=$1
shift
name=
if [ "$1" = --name ]; then
	name=$2
	shift 2
fi
for a in "$@"; do
	case $a in
	--gtest_list_tests) exec "$@" ;;
	--gtest_filter=*) [ -n "$name" ] || name=${a#--gtest_filter=} ;;
	esac
done
[ -n "$name" ] || name=$(basename "$1")
name=${name//\//_}

run=$(cat "$root/.run-$PPID" 2>/dev/null)
[ -n "$run" ] && [ -d "$run" ] || run=$root/adhoc
dir=$run/$name
n=1
# (a name results.txt has is taken too: a passed test's directory is gone)
while ! mkdir -p "$run" 2>/dev/null ||
	awk -v n="$(basename "$dir")" '$1 == n { f = 1 } END { exit !f }' \
		"$run/results.txt" 2>/dev/null ||
	! mkdir "$dir" 2>/dev/null; do
	n=$((n + 1))
	dir=$run/$name.$n
	[ $n -lt 1000 ] || { echo "test_run.sh: no directory in $run" >&2; exit 1; }
done
export DMODEM_TEST_DIR=$dir

level=${DMODEM_TEST_DEBUG:-5}
if [ "$level" != 0 ]; then
	for v in DSP_DEBUG CORE_DEBUG DMODEM_DEBUG DBG DMODEMD_DEBUG; do
		[ -n "${!v}" ] || export "$v=$level"
	done
	[ -n "$LINK_DEBUG" ] || export LINK_DEBUG=1
	[ -n "$SPANDSP_DEBUG" ] || export SPANDSP_DEBUG=1
fi

{
	echo "test: $name"
	echo "command: $*"
	echo "directory: $PWD"
	echo "start: $(date '+%Y-%m-%d %H:%M:%S') ($(now))"
	echo "debug: DMODEM_TEST_DEBUG=$level"
	env | grep -E '^(DSP_|DMODEM|CORE_DEBUG|LINK_DEBUG|SPANDSP_DEBUG|DBG=|NET|MODEM_|INTEROP)' | sort
} > "$dir/info.txt"

t0=$(now)
"$@" > >(tee "$dir/output.log") 2>&1 &
pid=$!
trap 'kill -TERM $pid 2>/dev/null' TERM INT HUP
wait $pid
rc=$?
# (a signal ends the first wait; the test's own status is the second's)
while kill -0 $pid 2>/dev/null; do
	wait $pid
	rc=$?
done
t1=$(now)

case $rc in
0) st=PASS ;;
77) st=SKIP ;;
*) st=FAIL ;;
esac
line=$(awk -v n="$(basename "$dir")" -v s=$st -v r=$rc -v a="$t0" -v b="$t1" \
	'BEGIN { printf "%s %s %d %.2f", n, s, r, b - a }')
echo "$line" > "$dir/result.txt"
echo "$line" >> "$run/results.txt"
if [ $st = PASS ] && [ "${DMODEM_TEST_KEEP_PASSED:-0}" = 0 ]; then
	rm -rf "$dir"
fi
exit $rc
