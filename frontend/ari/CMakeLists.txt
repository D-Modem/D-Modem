# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# dmodem-ari: D-Modem's modem for Asterisk calls as an ARI application, a
# program of its own beside Asterisk (no module in it).  Its call is the
# PBX modules' (frontend/pbx/dmodem_bridge.c) on libdmodem.

add_executable(dmodem-ari dmodem_ari.cpp client.cpp json.cpp sip.cpp
	${DMODEM_PBX_DIR}/dmodem_bridge.c)
target_include_directories(dmodem-ari PRIVATE ${DMODEM_PBX_DIR}
	${PROJECT_SOURCE_DIR}/dsp)
target_compile_definitions(dmodem-ari PRIVATE _GNU_SOURCE)
target_compile_options(dmodem-ari PRIVATE -Wall)
find_package(Threads REQUIRED)
target_link_libraries(dmodem-ari PRIVATE dmodem Threads::Threads)

install(TARGETS dmodem-ari RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	COMPONENT dmodem-ari)

# the systemd unit (dmodem-ari.service.in), on Linux: in
# <prefix>/lib/systemd/system, a directory that systemd reads for
# /usr/local and /usr.  DMODEM_SYSTEMD_UNIT_DIR selects a different one.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	set(DMODEM_SYSTEMD_UNIT_DIR lib/systemd/system CACHE STRING
		"Where cmake --install puts dmodem-ari.service")
	configure_file(dmodem-ari.service.in dmodem-ari.service @ONLY)
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dmodem-ari.service
		DESTINATION ${DMODEM_SYSTEMD_UNIT_DIR} COMPONENT dmodem-ari)
endif()

# install-dmodem-ari: dmodem-ari and its unit alone (the dmodem-ari
# component), not all of cmake --install:
#   cmake --build build --target dmodem-ari
#   sudo cmake --build build --target install-dmodem-ari
add_custom_target(install-dmodem-ari
	COMMAND ${CMAKE_COMMAND} --install ${PROJECT_BINARY_DIR}
		--component dmodem-ari --config $<CONFIG>
	DEPENDS dmodem-ari
	COMMENT "Installing dmodem-ari"
	VERBATIM)
