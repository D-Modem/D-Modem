# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# The tests' logs: each ctest run in a directory of its own under
# DMODEM_TEST_LOG_ROOT (<build>/test-logs/<YYYY-MM-DD_HH-MM-SS>/, latest
# pointing to the newest), each test's output, files and result in
# <run>/<test>/ (deleted when it passes, and the run's directory too when
# nothing failed, unless DMODEM_TEST_KEEP_PASSED=1), the run's
# results.txt and summary.txt; verbose debug levels for every
# test (DMODEM_TEST_DEBUG, 5 by default; 0: the tests' own).
#
# A fixture does it: test_logs_begin makes the run's directory before
# any test, test_logs_end writes the summary after them all; every test
# requires it, so ctest runs both around whatever it runs (-R, -L, ...).
# Each test runs under cmake/test_run.sh, which keeps its output (and
# still passes it on to ctest).
#
# dmodem_test_launcher(<target>): a gtest program's tests under it (as
# its emulator, which gtest_discover_tests takes); dmodem_add_test(<name>
# <command...>): add_test() under it.  Both require the fixture
# (DMODEM_TEST_FIXTURE in a test's FIXTURES_REQUIRED).

set(DMODEM_TEST_LOG_ROOT "${PROJECT_BINARY_DIR}/test-logs" CACHE PATH
	"where each ctest run's test logs go")
set(DMODEM_TEST_LOG_KEEP 20 CACHE STRING
	"how many ctest runs' test logs are kept")
set(DMODEM_TEST_FIXTURE dmodem_test_logs)
# (bash: on Windows Git's, not System32's WSL launcher)
if(CMAKE_HOST_WIN32)
	find_program(DMODEM_BASH bash HINTS "$ENV{ProgramFiles}/Git/bin"
		"$ENV{ProgramW6432}/Git/bin" NO_DEFAULT_PATH)
endif()
find_program(DMODEM_BASH bash REQUIRED)
set(DMODEM_TEST_LAUNCHER ${DMODEM_BASH} "${PROJECT_SOURCE_DIR}/cmake/test_run.sh"
	"${DMODEM_TEST_LOG_ROOT}")
# (a cross build's programs then through the emulator, Wine for Windows)
if(CMAKE_CROSSCOMPILING_EMULATOR)
	list(APPEND DMODEM_TEST_LAUNCHER ${CMAKE_CROSSCOMPILING_EMULATOR})
endif()

add_test(NAME test_logs_begin COMMAND ${DMODEM_BASH}
	"${PROJECT_SOURCE_DIR}/cmake/test_logs.sh" begin
	"${DMODEM_TEST_LOG_ROOT}" ${DMODEM_TEST_LOG_KEEP})
set_tests_properties(test_logs_begin PROPERTIES
	FIXTURES_SETUP ${DMODEM_TEST_FIXTURE} LABELS logs)
add_test(NAME test_logs_end COMMAND ${DMODEM_BASH}
	"${PROJECT_SOURCE_DIR}/cmake/test_logs.sh" end "${DMODEM_TEST_LOG_ROOT}")
set_tests_properties(test_logs_end PROPERTIES
	FIXTURES_CLEANUP ${DMODEM_TEST_FIXTURE} LABELS logs)

function(dmodem_test_launcher target)
	set_property(TARGET ${target} PROPERTY CROSSCOMPILING_EMULATOR
		${DMODEM_TEST_LAUNCHER})
endfunction()

function(dmodem_add_test name)
	add_test(NAME ${name} COMMAND ${DMODEM_TEST_LAUNCHER} --name ${name}
		${ARGN})
	set_tests_properties(${name} PROPERTIES
		FIXTURES_REQUIRED ${DMODEM_TEST_FIXTURE})
endfunction()
