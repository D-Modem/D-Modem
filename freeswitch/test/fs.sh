# fs.sh: sourced by the tests; a private FreeSWITCH (the installed one,
# FS_PREFIX, default /usr/local/freeswitch) with mod_dmodem.so from the
# CMake build and the configuration here (freeswitch.xml):
#
#   fs_start <name>     $run (the directories, fs.log), then FreeSWITCH
#   fs_cli <command>    an API command through the event socket
#   fs_wait <pattern> <count> [seconds]
#                       until fs.log has the pattern that many times
#   fs_stop
#
# Before fs_start: DMODEM_DEBUG (the modem's debug level), FAX_MAX_RATE
# (dmodem's fax no faster), TERM_INIT (AT commands for the calling modem
# of profile term, e.g. +MS=122), ANSWER (the command line the answering
# profiles bbs, bbsalaw and bbsexec answer with, default ATA).
here=$(cd "$(dirname "$0")" && pwd)
# the binaries of the CMake build (DMODEM_BUILD, default <repo>/build)
build=${DMODEM_BUILD:-$here/../../build}
FS_PREFIX=${FS_PREFIX:-/usr/local/freeswitch}
asterisk_test=$here/../../asterisk/test

fs_start() {
	run=${TMPDIR:-/tmp}/dmodem-fs-$1
	# one a killed test left behind
	[ -f "$run/run/freeswitch.pid" ] &&
		kill -9 "$(cat "$run/run/freeswitch.pid")" 2>/dev/null
	rm -rf "$run"
	mkdir -p "$run/conf" "$run/log" "$run/db" "$run/run" "$run/mod" \
		"$run/storage" "$run/fax" "$run/tmp"
	[ -f "$build/freeswitch/mod_dmodem.so" ] ||
		{ echo "build first: cmake --build $build" >&2; exit 1; }
	# its modules: <prefix>/mod, or lib/freeswitch/mod (a prefix other than
	# /usr/local/freeswitch has that layout)
	fsmod=$FS_PREFIX/mod
	[ -d "$fsmod" ] || fsmod=$FS_PREFIX/lib/freeswitch/mod
	for m in "$fsmod"/*.so; do ln -s "$m" "$run/mod/"; done
	ln -sf "$build/freeswitch/mod_dmodem.so" "$run/mod/mod_dmodem.so"
	{
		echo "<X-PRE-PROCESS cmd=\"set\" data=\"run=$run\"/>"
		echo "<X-PRE-PROCESS cmd=\"set\" data=\"test=$here\"/>"
		echo "<X-PRE-PROCESS cmd=\"set\" data=\"dmodem_debug=${DMODEM_DEBUG:-1}\"/>"
		echo "<X-PRE-PROCESS cmd=\"set\" data=\"fax_max_rate=${FAX_MAX_RATE:-14400}\"/>"
		echo "<X-PRE-PROCESS cmd=\"set\" data=\"term_init=${TERM_INIT:-}\"/>"
		echo "<X-PRE-PROCESS cmd=\"set\" data=\"bbs_answer=${ANSWER:-ATA}\"/>"
	} > "$run/conf/vars.xml"
	cp "$here/freeswitch.xml" "$run/conf/"
	export DMODEM_TEST="$asterisk_test"
	"$FS_PREFIX/bin/freeswitch" -nc -nonat \
		-conf "$run/conf" -log "$run/log" -db "$run/db" \
		-run "$run/run" -mod "$run/mod" -storage "$run/storage" \
		-temp "$run/tmp" > "$run/console.log" 2>&1
	i=0; until fs_cli status 2>/dev/null | grep -q "^UP"; do
		sleep 0.5; i=$((i+1))
		[ $i -lt 60 ] || { tail -20 "$run/log/freeswitch.log"; exit 1; }
	done
	trap fs_stop EXIT
	trap 'fs_stop; exit 1' INT TERM
	fs_cli "module_exists mod_dmodem" | grep -q true || {
		grep -A1 "CRIT" "$run/log/freeswitch.log"; exit 1; }
}

fs_cli() {
	"$FS_PREFIX/bin/fs_cli" -H 127.0.0.1 -P 8022 -p dmodem -x "$*"
}

fs_wait() {
	i=0; until [ "$(grep -c -- "$1" "$run/log/freeswitch.log")" -ge "$2" ]; do
		sleep 1; i=$((i+1)); [ $i -lt "${3:-300}" ] || return 1
	done
}

fs_stop() {
	trap - EXIT INT TERM
	fs_cli shutdown >/dev/null 2>&1
	i=0; while [ -f "$run/run/freeswitch.pid" ] &&
		kill -0 "$(cat "$run/run/freeswitch.pid")" 2>/dev/null; do
		sleep 0.5; i=$((i+1))
		[ $i -lt 20 ] || { kill -9 "$(cat "$run/run/freeswitch.pid")"; break; }
	done
}
