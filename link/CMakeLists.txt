# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# The data link layer (C++, namespace dlink): the V.42 detection phase,
# V.42 LAPM over HDLC, MNP 2 to 4 and 5, V.42bis, V.44, zstd, asynchronous
# characters, V.80 synchronous access, and V.70's multiplexer (V.76, V.75
# with H.245) and voice coder (G.729 Annex A).  It does not depend on the modem core; the core is its
# Owner (link.h).
#
#   dmodem_link        object library
#   DMODEM_LINK_SRCS   the same as a source list (32 bit interop tests)
#   DMODEM_LINK_DIR

set(DMODEM_LINK_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(_link g729a_common.cpp g729a_dec.cpp g729a_enc.cpp g729a_tab.cpp h245.cpp hdlc.cpp lapb.cpp lapm.cpp ppp_sync.cpp mnp.cpp mnp5.cpp v42bis.cpp v44.cpp v44_preset.cpp v76.cpp v80.cpp zstd_codec.cpp zstd_dict.cpp link.cpp)
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
	# G.729 Annex A; its vectors where docs/specs/fetch_g729.sh puts them
	add_executable(g729a_test test/g729a_test.cpp)
	target_link_libraries(g729a_test PRIVATE dmodem_link)
	target_compile_definitions(g729a_test PRIVATE
		DMODEM_G729_VECTORS="${PROJECT_SOURCE_DIR}/docs/specs/g729a")
	dmodem_flags(g729a_test EXTRA)
	dmodem_gtest(g729a_test LABELS quick)
	# and against bcg729 where it is found (GPL: a test only)
	find_path(DMODEM_BCG729_INCLUDE bcg729/encoder.h)
	find_library(DMODEM_BCG729_LIB bcg729)
	set(_bcg729 OFF)
	if(DMODEM_BCG729_INCLUDE AND DMODEM_BCG729_LIB)
		include(CheckCXXSourceCompiles)
		set(CMAKE_REQUIRED_INCLUDES ${DMODEM_BCG729_INCLUDE})
		set(CMAKE_REQUIRED_LIBRARIES ${DMODEM_BCG729_LIB})
		check_cxx_source_compiles([[
extern "C" {
#include <bcg729/encoder.h>
}
int main() { return initBcg729EncoderChannel(0) == nullptr; }
]] DMODEM_BCG729_LINKS)
		unset(CMAKE_REQUIRED_INCLUDES)
		unset(CMAKE_REQUIRED_LIBRARIES)
		set(_bcg729 ${DMODEM_BCG729_LINKS})
	endif()
	if(_bcg729)
		add_executable(g729a_bcg729_test test/g729a_bcg729_test.cpp)
		target_include_directories(g729a_bcg729_test PRIVATE
			${DMODEM_BCG729_INCLUDE})
		target_link_libraries(g729a_bcg729_test PRIVATE dmodem_link
			${DMODEM_BCG729_LIB})
		target_compile_definitions(g729a_bcg729_test PRIVATE
			DMODEM_G729_VECTORS="${PROJECT_SOURCE_DIR}/docs/specs/g729a")
		dmodem_flags(g729a_bcg729_test EXTRA)
		dmodem_gtest(g729a_bcg729_test LABELS quick)
	endif()
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
