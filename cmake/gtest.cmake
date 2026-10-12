# GoogleTest for the tests: the system's (libgtest-dev) or else fetched.
# dmodem_gtest(<target> [LABELS ...] [TIMEOUT s] [ENVIRONMENT ...]) links a
# test program with it and registers each of its tests with ctest, in a
# process of its own (gtest_discover_tests).

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
	cmake_parse_arguments(G "" "TIMEOUT" "LABELS;ENVIRONMENT" ${ARGN})
	target_link_libraries(${target} PRIVATE GTest::gtest_main)
	set(_props)
	if(G_LABELS)
		list(APPEND _props LABELS "${G_LABELS}")
	endif()
	if(G_TIMEOUT)
		list(APPEND _props TIMEOUT ${G_TIMEOUT})
	endif()
	if(G_ENVIRONMENT)
		list(APPEND _props ENVIRONMENT "${G_ENVIRONMENT}")
	endif()
	gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		PROPERTIES ${_props})
endfunction()
