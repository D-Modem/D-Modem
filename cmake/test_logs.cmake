# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
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
# <command...>): add_test() under it.  Both require a fixture that
# test_logs_begin sets up: DMODEM_STEADY_FIXTURE, whose cleanup test
# (dmodem_flaky_gate) itself requires DMODEM_TEST_FIXTURE, or for the
# known flaky tests (flaky_tests.cmake) DMODEM_TEST_FIXTURE alone, after
# the gate and serially (gtest.cmake, dmodem_add_test).  test_logs_end, the
# cleanup of DMODEM_TEST_FIXTURE, so ends after all of them.

set(DMODEM_TEST_LOG_ROOT "${PROJECT_BINARY_DIR}/test-logs" CACHE PATH
	"where each ctest run's test logs go")
set(DMODEM_TEST_LOG_KEEP 20 CACHE STRING
	"how many ctest runs' test logs are kept")
set(DMODEM_TEST_FIXTURE dmodem_test_logs)
set(DMODEM_STEADY_FIXTURE dmodem_steady)
include(${CMAKE_CURRENT_LIST_DIR}/flaky_tests.cmake)
# (bash: on Windows Git's, not System32's WSL launcher)
if(CMAKE_HOST_WIN32)
	find_program(DMODEM_BASH bash HINTS "$ENV{ProgramFiles}/Git/bin"
		"$ENV{ProgramW6432}/Git/bin" NO_DEFAULT_PATH)
endif()
find_program(DMODEM_BASH bash REQUIRED)
set(DMODEM_TEST_RUNNER ${DMODEM_BASH} "${PROJECT_SOURCE_DIR}/cmake/test_run.sh"
	"${DMODEM_TEST_LOG_ROOT}")
set(DMODEM_TEST_LAUNCHER ${DMODEM_TEST_RUNNER})
# (a cross build's programs then through the emulator, Wine for Windows)
if(CMAKE_CROSSCOMPILING_EMULATOR)
	list(APPEND DMODEM_TEST_LAUNCHER ${CMAKE_CROSSCOMPILING_EMULATOR})
endif()
# a cross build with nothing here to run its programs (Haiku's, from
# cmake/haiku.cmake): the gtest programs built, their tests not found
# (that runs them) nor registered; ctest runs them on that system
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
	set(DMODEM_TESTS_RUN OFF)
	message(STATUS "tests: built, not run here (no emulator for this "
		"system)")
else()
	set(DMODEM_TESTS_RUN ON)
endif()

add_test(NAME test_logs_begin COMMAND ${DMODEM_BASH}
	"${PROJECT_SOURCE_DIR}/cmake/test_logs.sh" begin
	"${DMODEM_TEST_LOG_ROOT}" ${DMODEM_TEST_LOG_KEEP})
set_tests_properties(test_logs_begin PROPERTIES
	FIXTURES_SETUP "${DMODEM_TEST_FIXTURE};${DMODEM_STEADY_FIXTURE}"
	LABELS logs)
# (once the steady tests have all ended: the flaky ones after it)
add_test(NAME dmodem_flaky_gate COMMAND ${CMAKE_COMMAND} -E echo
	"the steady tests are over")
set_tests_properties(dmodem_flaky_gate PROPERTIES
	FIXTURES_CLEANUP ${DMODEM_STEADY_FIXTURE}
	FIXTURES_REQUIRED ${DMODEM_TEST_FIXTURE} LABELS logs)
add_test(NAME test_logs_end COMMAND ${DMODEM_BASH}
	"${PROJECT_SOURCE_DIR}/cmake/test_logs.sh" end "${DMODEM_TEST_LOG_ROOT}")
set_tests_properties(test_logs_end PROPERTIES
	FIXTURES_CLEANUP ${DMODEM_TEST_FIXTURE} LABELS logs)

function(dmodem_test_launcher target)
	set_property(TARGET ${target} PROPERTY CROSSCOMPILING_EMULATOR
		${DMODEM_TEST_LAUNCHER})
endfunction()

function(dmodem_add_test name)
	# (a cross build's own program, $<TARGET_FILE:...>, through the
	# emulator after test_run.sh's --name; the host's, a script's
	# interpreter, as it is)
	set(_cmd ${DMODEM_TEST_RUNNER} --name ${name})
	list(GET ARGN 0 _prog)
	if(CMAKE_CROSSCOMPILING_EMULATOR AND _prog MATCHES "^\\$<TARGET_FILE:")
		list(APPEND _cmd ${CMAKE_CROSSCOMPILING_EMULATOR})
	endif()
	add_test(NAME ${name} COMMAND ${_cmd} ${ARGN})
	if(name IN_LIST DMODEM_FLAKY_TESTS)
		set_tests_properties(${name} PROPERTIES
			FIXTURES_REQUIRED ${DMODEM_TEST_FIXTURE}
			DEPENDS dmodem_flaky_gate RUN_SERIAL TRUE)
		set_property(TEST ${name} APPEND PROPERTY LABELS flaky)
	else()
		set_tests_properties(${name} PROPERTIES
			FIXTURES_REQUIRED ${DMODEM_STEADY_FIXTURE})
	endif()
endfunction()

# (dmodem_gtest) an include file for ctest, after the discovery of
# target's flaky tests (into tests_file): each of them also labelled
# flaky (ctest adds to a list property here; a discovery's properties
# take one value each)
function(dmodem_flaky_label target tests_file)
	set(_f "${CMAKE_CURRENT_BINARY_DIR}/${target}_flaky.cmake")
	file(WRITE ${_f}
		"# the known flaky tests of ${target}: labelled flaky\n"
		"if(EXISTS \"${tests_file}\")\n"
		"  file(READ \"${tests_file}\" _tests)\n"
		"  foreach(_t ${DMODEM_FLAKY_TESTS})\n"
		"    string(FIND \"\${_tests}\" \"add_test([=[\${_t}]=]\" _at)\n"
		"    if(_at GREATER -1)\n"
		"      set_tests_properties(\${_t} PROPERTIES LABELS flaky)\n"
		"    endif()\n"
		"  endforeach()\n"
		"endif()\n")
	set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES ${_f})
endfunction()
