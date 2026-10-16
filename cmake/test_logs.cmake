# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# The tests' logs: each ctest run in a directory of its own under
# DMODEM_TEST_LOG_ROOT (<build>/test-logs/<YYYY-MM-DD_HH-MM-SS>/, latest
# pointing to the last), each test's output, files and result in
# <run>/<test>/ (deleted when it passes, unless DMODEM_TEST_KEEP_PASSED=1),
# the run's results.txt and summary.txt; verbose debug levels for every
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
set(DMODEM_TEST_LAUNCHER bash "${PROJECT_SOURCE_DIR}/cmake/test_run.sh"
	"${DMODEM_TEST_LOG_ROOT}")

add_test(NAME test_logs_begin COMMAND bash
	"${PROJECT_SOURCE_DIR}/cmake/test_logs.sh" begin
	"${DMODEM_TEST_LOG_ROOT}" ${DMODEM_TEST_LOG_KEEP})
set_tests_properties(test_logs_begin PROPERTIES
	FIXTURES_SETUP ${DMODEM_TEST_FIXTURE} LABELS logs)
add_test(NAME test_logs_end COMMAND bash
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
