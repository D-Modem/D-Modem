#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
# test_logs.sh begin|end <log root> [<runs kept>]: a ctest run's log
# directory (cmake/test_logs.cmake), ctest's fixture around every test.
#
# begin: <log root>/<YYYY-MM-DD_HH-MM-SS>/ for this run, named for ctest
#        (our parent) in <log root>/.run-<its pid> for test_run.sh,
#        <log root>/latest pointing to it; run.txt has when, the ctest
#        command, the commit; the oldest runs go, so that <runs kept>
#        (20) are left.
# end:   summary.txt in it: the counts, then each test's line of
#        results.txt (failures first; a test directory without a
#        result.txt was killed, e.g. at its ctest timeout), and that
#        printed.  (test_run.sh has deleted the directories of the tests
#        that passed already.)  A run without a failure (skipped tests
#        are none) leaves nothing: its directory goes, and latest points
#        to the newest run left (DMODEM_TEST_KEEP_PASSED=1 keeps it).

cmd=$1
root=$2
keep=${3:-20}
mkdir -p "$root" || exit 1

case $cmd in
begin)
	# (the pointers of ctest runs that are gone)
	for f in "$root"/.run-*; do
		[ -e "$f" ] || continue
		kill -0 "${f##*.run-}" 2>/dev/null || rm -f "$f"
	done
	stamp=$(date +%Y-%m-%d_%H-%M-%S)
	run=$root/$stamp
	n=1
	while ! mkdir "$run" 2>/dev/null; do
		n=$((n + 1))
		run=$root/$stamp.$n
	done
	echo "$run" > "$root/.run-$PPID"
	ln -sfn "$(basename "$run")" "$root/latest"
	{
		echo "start: $(date '+%Y-%m-%d %H:%M:%S')"
		echo "ctest: $(tr '\0' ' ' < /proc/$PPID/cmdline 2>/dev/null)"
		echo "directory: $(readlink /proc/$PPID/cwd 2>/dev/null)"
		src=$(dirname "$(readlink -f "$0")")/..
		echo "commit: $(git -C "$src" describe --always --dirty 2>/dev/null)"
		echo "host: $(uname -n), $(nproc) processors"
		echo "debug: DMODEM_TEST_DEBUG=${DMODEM_TEST_DEBUG:-5}"
	} > "$run/run.txt"
	# the oldest runs beyond <runs kept> (with this one)
	ls -1d "$root"/20[0-9][0-9]-* 2>/dev/null | sort | head -n -"$keep" |
		while read -r d; do
			rm -rf "$d"
		done
	echo "test logs: $run"
	;;
end)
	run=$(cat "$root/.run-$PPID" 2>/dev/null)
	[ -n "$run" ] && [ -d "$run" ] || { echo "test_logs.sh: no run"; exit 0; }
	cat "$run/results.txt" 2>/dev/null > "$run/.results"
	for d in "$run"/*/; do
		[ -d "$d" ] && [ ! -e "$d/result.txt" ] &&
			echo "$(basename "$d") FAIL killed -"
	done >> "$run/.results"
	{
		echo "end: $(date '+%Y-%m-%d %H:%M:%S')"
		awk '{ n[$2]++; t++ } END {
			printf "%d tests: %d passed, %d failed, %d skipped\n",
				t, n["PASS"], n["FAIL"], n["SKIP"] }' "$run/.results"
		echo
		grep ' FAIL ' "$run/.results" | sort
		grep -v ' FAIL ' "$run/.results" | sort
	} > "$run/summary.txt"
	failed=$(grep -c ' FAIL ' "$run/.results")
	rm -f "$run/.results" "$root/.run-$PPID"
	if [ "$failed" = 0 ] && [ "${DMODEM_TEST_KEEP_PASSED:-0}" = 0 ]; then
		sed -n 2p "$run/summary.txt"
		echo "test logs: none kept, nothing failed"
		rm -rf "$run"
		last=$(ls -1d "$root"/20[0-9][0-9]-* 2>/dev/null | sort | tail -n 1)
		if [ -n "$last" ]; then
			ln -sfn "$(basename "$last")" "$root/latest"
		else
			rm -f "$root/latest"
		fi
		exit 0
	fi
	echo "test logs: $run"
	head -3 "$run/summary.txt"
	grep ' FAIL ' "$run/summary.txt"
	;;
*)
	echo "usage: $0 begin|end <log root> [<runs kept>]" >&2
	exit 2
	;;
esac
exit 0
