# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# dmodem-capid: a CAPI 2.0 controller on a D-Modem, its applications
# connecting with libcapi20's remote CAPI ("rcapi") over TCP (POSIX).
#
#   dmodem_capi    the controller and the server (a static library, for
#                  the tests)

add_library(dmodem_capi STATIC capi_msg.cpp capi_controller.cpp
	rcapi_server.cpp)
target_include_directories(dmodem_capi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
dmodem_flags(dmodem_capi EXTRA)

add_executable(dmodem-capid capid.cpp)
target_link_libraries(dmodem-capid PRIVATE dmodem_capi)
dmodem_flags(dmodem-capid EXTRA)
install(TARGETS dmodem-capid RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(DMODEM_TESTS)
	# the controller on libdmodem's modems, and the server over TCP
	add_executable(capi_test test/capi_test.cpp)
	target_include_directories(capi_test PRIVATE
		${PROJECT_SOURCE_DIR}/frontend/dmodemd)
	target_link_libraries(capi_test PRIVATE dmodem_capi dmodem)
	dmodem_flags(capi_test)
	dmodem_gtest(capi_test LABELS quick)
endif()
