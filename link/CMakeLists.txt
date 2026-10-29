# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# The data link layer (C++, namespace dlink): the V.42 detection phase,
# V.42 LAPM over HDLC, MNP 2 to 4 and 5, V.42bis, V.44, zstd, asynchronous
# characters, and V.80 synchronous access.  It does not depend on the modem core; the core is its
# Owner (link.h).
#
#   dmodem_link        object library
#   DMODEM_LINK_SRCS   the same as a source list (32 bit interop tests)
#   DMODEM_LINK_DIR

set(DMODEM_LINK_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(_link hdlc.cpp lapm.cpp mnp.cpp mnp5.cpp v42bis.cpp v44.cpp v44_preset.cpp v80.cpp zstd_codec.cpp zstd_dict.cpp link.cpp)
list(TRANSFORM _link PREPEND ${DMODEM_LINK_DIR}/)
add_library(dmodem_link OBJECT ${_link})
target_include_directories(dmodem_link PUBLIC ${DMODEM_LINK_DIR})
dmodem_flags(dmodem_link EXTRA)
set_target_properties(dmodem_link PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(DMODEM_LINK_DIR ${DMODEM_LINK_DIR} PARENT_SCOPE)
set(DMODEM_LINK_SRCS ${_link} PARENT_SCOPE)

if(DMODEM_TESTS)
	add_executable(link_test test/link_test.cpp)
	target_link_libraries(link_test PRIVATE dmodem_link)
	dmodem_flags(link_test EXTRA)
	dmodem_gtest(link_test LABELS quick)
	# zstd against the reference library, where it is found
	find_path(DMODEM_ZSTD_INCLUDE zstd.h)
	find_library(DMODEM_ZSTD_LIB zstd)
	# and only where it links for this build's every architecture (a
	# universal macOS build, Homebrew's libzstd being arm64 only)
	set(_zstd OFF)
	if(DMODEM_ZSTD_INCLUDE AND DMODEM_ZSTD_LIB)
		include(CheckCXXSourceCompiles)
		set(CMAKE_REQUIRED_INCLUDES ${DMODEM_ZSTD_INCLUDE})
		set(CMAKE_REQUIRED_LIBRARIES ${DMODEM_ZSTD_LIB})
		check_cxx_source_compiles([[
#include <zstd.h>
int main() { return ZSTD_versionNumber() == 0; }
]] DMODEM_ZSTD_LINKS)
		unset(CMAKE_REQUIRED_INCLUDES)
		unset(CMAKE_REQUIRED_LIBRARIES)
		set(_zstd ${DMODEM_ZSTD_LINKS})
	endif()
	# the compressors' ratio and speed on files (libzstd's beside them
	# where it is found); --check, their round trips, as a test
	add_executable(compress_bench test/compress_bench.cpp)
	target_link_libraries(compress_bench PRIVATE dmodem_link)
	dmodem_flags(compress_bench EXTRA)
	if(_zstd)
		target_include_directories(compress_bench PRIVATE
			${DMODEM_ZSTD_INCLUDE})
		target_link_libraries(compress_bench PRIVATE ${DMODEM_ZSTD_LIB})
		target_compile_definitions(compress_bench PRIVATE
			DMODEM_HAVE_LIBZSTD)
	endif()
	dmodem_test_launcher(compress_bench)
	if(DMODEM_TESTS_RUN)
		dmodem_add_test(compress_bench_check
			$<TARGET_FILE:compress_bench> --check)
		set_tests_properties(compress_bench_check PROPERTIES
			LABELS quick)
	endif()
	if(_zstd)
		add_executable(zstd_ref_test test/zstd_ref_test.cpp)
		target_include_directories(zstd_ref_test PRIVATE
			${DMODEM_ZSTD_INCLUDE})
		target_link_libraries(zstd_ref_test PRIVATE dmodem_link
			${DMODEM_ZSTD_LIB})
		dmodem_flags(zstd_ref_test EXTRA)
		dmodem_gtest(zstd_ref_test LABELS quick)
	endif()
endif()
