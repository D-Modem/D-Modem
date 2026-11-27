# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# libdmodem_embed: dmodemd as a library (dmodem_embed.h), for a program
# that has the modem in a thread of its own: the D-Modem desktop app.
# The SIP line (libdmodem_sip), and the USB, ALSA and mISDN lines of
# dmodemd where this build has them (as frontend/dmodemd/CMakeLists.txt
# finds them).

set(_dmd ${PROJECT_SOURCE_DIR}/frontend/dmodemd)
add_library(dmodem_embed STATIC dmodem_embed.cpp)
target_include_directories(dmodem_embed PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(dmodem_embed PRIVATE ${_dmd}
	${PROJECT_SOURCE_DIR}/dsp)
target_link_libraries(dmodem_embed PUBLIC dmodem_sip)
# (dmodemd's warnings for its lines' sources, -Wextra for ours)
dmodem_flags(dmodem_embed)
if(NOT MSVC)
	set_source_files_properties(dmodem_embed.cpp PROPERTIES
		COMPILE_OPTIONS -Wextra)
endif()
set_target_properties(dmodem_embed PROPERTIES POSITION_INDEPENDENT_CODE ON)

# the USB line: as dmodemd's (libusb fetched on Windows and macOS, the
# system's elsewhere)
find_package(PkgConfig QUIET)
set(_usb_srcs usb_line.cpp usb_agere.cpp usb_apple.cpp usb_cadmus2.cpp
	usb_wb.cpp si3056.cpp)
list(TRANSFORM _usb_srcs PREPEND ${_dmd}/)
if((WIN32 OR APPLE) AND DMODEM_USB AND TARGET dmodem_libusb)
	target_sources(dmodem_embed PRIVATE ${_usb_srcs})
	target_compile_definitions(dmodem_embed PRIVATE DMODEM_USB)
	target_link_libraries(dmodem_embed PUBLIC dmodem_libusb)
elseif(PkgConfig_FOUND AND NOT WIN32)
	pkg_check_modules(EMBED_LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
	if(EMBED_LIBUSB_FOUND)
		target_sources(dmodem_embed PRIVATE ${_usb_srcs})
		target_compile_definitions(dmodem_embed PRIVATE DMODEM_USB)
		target_link_libraries(dmodem_embed PUBLIC PkgConfig::EMBED_LIBUSB)
	endif()
endif()
# the ALSA line
if(PkgConfig_FOUND AND NOT WIN32)
	pkg_check_modules(EMBED_ALSA QUIET IMPORTED_TARGET alsa)
	if(EMBED_ALSA_FOUND)
		set(_alsa_srcs alsa_line.cpp hda_codec.cpp si3054.cpp
			cx_daa.cpp cx_country.cpp ac97_ports.cpp)
		list(TRANSFORM _alsa_srcs PREPEND ${_dmd}/)
		target_sources(dmodem_embed PRIVATE ${_alsa_srcs})
		target_compile_definitions(dmodem_embed PRIVATE DMODEM_ALSA)
		target_link_libraries(dmodem_embed PUBLIC PkgConfig::EMBED_ALSA)
	endif()
endif()
# the mISDN line: Linux's
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_sources(dmodem_embed PRIVATE ${_dmd}/misdn_line.cpp
		${_dmd}/q931.cpp)
	target_compile_definitions(dmodem_embed PRIVATE DMODEM_MISDN)
endif()

# test/embed_test.py: two programs, each with an embedded modem
# (test/embed_cat.c, in C), call each other over SIP on 127.0.0.1
if(DMODEM_TESTS AND NOT WIN32)
	enable_language(C)
	add_executable(embed_cat test/embed_cat.c)
	target_link_libraries(embed_cat PRIVATE dmodem_embed)
	# (a C program: the C++ library linked as C++)
	set_target_properties(embed_cat PROPERTIES LINKER_LANGUAGE CXX)
	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(Python3_Interpreter_FOUND)
		dmodem_add_test(dmodem_embed ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test/embed_test.py
			$<TARGET_FILE:embed_cat>)
		set_tests_properties(dmodem_embed PROPERTIES LABELS quick
			TIMEOUT 180)
	endif()
endif()
