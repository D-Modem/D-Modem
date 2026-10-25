# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
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
	if(DMODEM_ZSTD_INCLUDE AND DMODEM_ZSTD_LIB)
		add_executable(zstd_ref_test test/zstd_ref_test.cpp)
		target_include_directories(zstd_ref_test PRIVATE
			${DMODEM_ZSTD_INCLUDE})
		target_link_libraries(zstd_ref_test PRIVATE dmodem_link
			${DMODEM_ZSTD_LIB})
		dmodem_flags(zstd_ref_test EXTRA)
		dmodem_gtest(zstd_ref_test LABELS quick)
	endif()
endif()
