# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# GoogleTest for the tests: the system's (libgtest-dev) or else fetched.
# dmodem_gtest(<target> [LABELS ...] [TIMEOUT s] [ENVIRONMENT ...]
# [RUN_SERIAL]) links a test program with it and registers each of its tests
# with ctest, in a process of its own (gtest_discover_tests), its output
# kept in the run's test logs (test_logs.cmake); RUN_SERIAL: none of them
# with other tests.  Those of them in flaky_tests.cmake run last, one at
# a time.

find_package(GTest QUIET)
if(NOT GTest_FOUND)
	include(FetchContent)
	FetchContent_Declare(googletest
		GIT_REPOSITORY https://github.com/google/googletest.git
		GIT_TAG v1.14.0
		GIT_SHALLOW TRUE)
	set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
	set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
	FetchContent_MakeAvailable(googletest)
	if(NOT TARGET GTest::gtest_main)
		add_library(GTest::gtest_main ALIAS gtest_main)
	endif()
endif()
include(GoogleTest)

function(dmodem_gtest target)
	cmake_parse_arguments(G "RUN_SERIAL" "TIMEOUT" "LABELS;ENVIRONMENT"
		${ARGN})
	target_link_libraries(${target} PRIVATE GTest::gtest_main)
	dmodem_test_launcher(${target})
	# (each property one value: gtest_discover_tests passes no lists)
	set(_props "")
	if(G_LABELS)
		list(APPEND _props LABELS "${G_LABELS}")
	endif()
	if(G_TIMEOUT)
		list(APPEND _props TIMEOUT ${G_TIMEOUT})
	endif()
	if(G_ENVIRONMENT)
		list(APPEND _props ENVIRONMENT "${G_ENVIRONMENT}")
	endif()
	set(_steady FIXTURES_REQUIRED ${DMODEM_STEADY_FIXTURE} ${_props})
	if(G_RUN_SERIAL)
		list(APPEND _steady RUN_SERIAL TRUE)
	endif()
	set(_args DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
	# the known flaky ones (flaky_tests.cmake) apart, last and serially
	# (test_logs.cmake): the program's tests found twice, the others,
	# then those (TEST_FILTER, CMake 3.22); with an older CMake they
	# run with the others
	if(NOT DMODEM_FLAKY_TESTS OR CMAKE_VERSION VERSION_LESS 3.22)
		gtest_discover_tests(${target} ${_args} PROPERTIES ${_steady})
		return()
	endif()
	string(JOIN ":" _flaky ${DMODEM_FLAKY_TESTS})
	gtest_discover_tests(${target} ${_args} TEST_FILTER "-${_flaky}"
		PROPERTIES ${_steady})
	gtest_discover_tests(${target} ${_args} TEST_FILTER "${_flaky}"
		PROPERTIES FIXTURES_REQUIRED ${DMODEM_TEST_FIXTURE}
		DEPENDS dmodem_flaky_gate RUN_SERIAL TRUE ${_props})
	get_property(_n TARGET ${target} PROPERTY CTEST_DISCOVERED_TEST_COUNTER)
	dmodem_flaky_label(${target}
		"${CMAKE_CURRENT_BINARY_DIR}/${target}[${_n}]_tests.cmake")
endfunction()
