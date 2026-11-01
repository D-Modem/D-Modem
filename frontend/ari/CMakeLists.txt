# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# dmodem-ari: D-Modem's modem for Asterisk calls as an ARI application, a
# program of its own beside Asterisk (no module in it).  Its call is the
# PBX modules' (frontend/pbx/dmodem_bridge.c) on libdmodem.

add_executable(dmodem-ari dmodem_ari.cpp client.cpp json.cpp sip.cpp
	${DMODEM_PBX_DIR}/dmodem_bridge.c)
target_include_directories(dmodem-ari PRIVATE ${DMODEM_PBX_DIR})
target_compile_definitions(dmodem-ari PRIVATE _GNU_SOURCE)
target_compile_options(dmodem-ari PRIVATE -Wall)
find_package(Threads REQUIRED)
target_link_libraries(dmodem-ari PRIVATE dmodem Threads::Threads)

install(TARGETS dmodem-ari RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
