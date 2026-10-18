# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# dmodemd (the modem daemon behind a pty, on SIP calls), modem_test, and
# libdmodem (the modem as a library, dmodem.h).  They run the modem core
# (../core, dmodem_core) with the data pumps (../dsp, dmodem_dsp) and the
# data link layer (../link, dmodem_link).
#
#   DMODEM_FRONT_SRCS    the front ends' parts (options, pty, a modem on
#                        two descriptors, the main loop, a line recorder),
#                        for the 32 bit interop tests

set(_front options.cpp datafile.cpp run_loop.cpp line_recorder.cpp)
if(WIN32)
	# the DTE a COM port or a TCP connection (windows_dte.cpp), for the
	# pty and the pipe modems of POSIX
	list(APPEND _front windows_dte.cpp)
else()
	list(APPEND _front pty.cpp pipe_modem.cpp)
endif()
list(TRANSFORM _front PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
set(DMODEM_FRONT_SRCS ${_front} PARENT_SCOPE)
set(DMODEM_FRONT_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
add_library(dmodem_front OBJECT ${_front})
target_link_libraries(dmodem_front PUBLIC dmodem_core)
dmodem_flags(dmodem_front EXTRA)
set_target_properties(dmodem_front PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(DMODEM_DMODEMD)
	# libdmodem_sip: the modem on a SIP line, its DTE a descriptor
	# (sip_modem.h), with its SIP user agent (sip.cpp), the front
	# ends' parts and the objects of the core, the pumps and the link;
	# for dmodemd, and for a program that has the modem in a thread of
	# its own (an Android app)
	add_library(dmodem_sip STATIC sip_modem.cpp sip.cpp sip_dial.cpp
		rtp_events.cpp
		$<TARGET_OBJECTS:dmodem_front> $<TARGET_OBJECTS:dmodem_core>
		$<TARGET_OBJECTS:dmodem_dsp> $<TARGET_OBJECTS:dmodem_t30>
		$<TARGET_OBJECTS:dmodem_link>)
	target_include_directories(dmodem_sip PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR} ${DMODEM_CORE_NEW_DIR}
		${DMODEM_DSP_DIR} ${DMODEM_LINK_DIR})
	dmodem_flags(dmodem_sip EXTRA)
	set_target_properties(dmodem_sip PROPERTIES
		POSITION_INDEPENDENT_CODE ON)
	target_link_libraries(dmodem_sip PUBLIC libre ${DMODEM_LIBM} Threads::Threads)

	# the modem daemon: the modem on a SIP line (or with -a an ALSA
	# one, below) and a pty
	if(WIN32)
		# (Windows: its DTE a COM port or TCP, windows_dte.cpp; the
		# system's timer at 1 ms, winmm)
		add_executable(dmodemd main_windows.cpp)
		target_link_libraries(dmodemd PRIVATE dmodem_sip winmm)
		install(TARGETS dmodemd RUNTIME
			DESTINATION ${CMAKE_INSTALL_BINDIR})
	else()
		add_executable(dmodemd main.cpp)
		target_link_libraries(dmodemd PRIVATE dmodem_sip)
		install(TARGETS dmodemd RUNTIME
			DESTINATION ${CMAKE_INSTALL_SBINDIR})
		# HD-audio modem codecs outside snd-hda-codec-si3054's list
		# for -a (docs/hardware.md)
		install(PROGRAMS hda-modem-setup
			DESTINATION ${CMAKE_INSTALL_SBINDIR})
	endif()
	dmodem_flags(dmodemd)
	# -a: a softmodem's codec through ALSA for the line (alsa_line.cpp),
	# if libasound2-dev is there
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND AND NOT WIN32)
		pkg_check_modules(ALSA QUIET IMPORTED_TARGET alsa)
	endif()
	if(ALSA_FOUND)
		target_sources(dmodemd PRIVATE alsa_line.cpp)
		target_compile_definitions(dmodemd PRIVATE DMODEM_ALSA)
		target_link_libraries(dmodemd PRIVATE PkgConfig::ALSA)
		# test/alsa_test.py: two dmodemd on fake ALSA modem cards
		# (test/fake_modem_card.cpp, alsa-lib plugins) calling each
		# other through a simulated exchange (test/line_exchange.cpp)
		if(DMODEM_TESTS)
			add_library(asound_module_dmline MODULE
				test/fake_modem_card.cpp)
			dmodem_flags(asound_module_dmline EXTRA NOOPT)
			# (alsa-lib's plugin symbols, as a shared object
			# has them)
			target_compile_definitions(asound_module_dmline
				PRIVATE PIC)
			target_link_libraries(asound_module_dmline PRIVATE
				PkgConfig::ALSA)
		endif()
	else()
		message(STATUS "dmodemd: no ALSA (libasound2-dev): no -a")
	endif()
	# --usb: a Smart Link SmartUSB56 (ST7554) through libusb for the
	# line (usb_line.cpp), if libusb-1.0-0-dev is there
	if(PkgConfig_FOUND AND NOT WIN32)
		pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
	endif()
	if(LIBUSB_FOUND)
		target_sources(dmodemd PRIVATE usb_line.cpp)
		target_compile_definitions(dmodemd PRIVATE DMODEM_USB)
		target_link_libraries(dmodemd PRIVATE PkgConfig::LIBUSB)
		# test/usb_test.py: two dmodemd on fake SmartUSB56s
		# (test/fake_st7554.cpp, libusb's calls, LD_PRELOAD) calling
		# each other through the simulated exchange
		if(DMODEM_TESTS)
			add_library(dmodem_fake_st7554 SHARED
				test/fake_st7554.cpp)
			dmodem_flags(dmodem_fake_st7554 EXTRA NOOPT)
			target_include_directories(dmodem_fake_st7554 PRIVATE
				${LIBUSB_INCLUDE_DIRS})
		endif()
	else()
		message(STATUS "dmodemd: no libusb (libusb-1.0-0-dev): no --usb")
	endif()
	# the simulated telephone exchange of the ALSA and USB lines' tests
	if(DMODEM_TESTS AND (ALSA_FOUND OR LIBUSB_FOUND))
		add_executable(line_exchange test/line_exchange.cpp)
		dmodem_flags(line_exchange EXTRA)
		target_link_libraries(line_exchange PRIVATE dmodem_core dmodem_dsp
			dmodem_link ${DMODEM_LIBM} Threads::Threads)
	endif()

	# test/root_test.py: dmodemd started as root, what needs root's
	# rights after it has dropped them (skipped unless ctest is root)
	# test/sip_dial_test.cpp: the SIP user of a dial string
	if(DMODEM_TESTS)
		add_executable(sip_dial_test test/sip_dial_test.cpp sip_dial.cpp)
		target_include_directories(sip_dial_test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR})
		dmodem_flags(sip_dial_test EXTRA NOOPT)
		dmodem_gtest(sip_dial_test LABELS quick)
		# test/rtp_events_test.cpp: the far end's telephone-events
		# as tones (rtp_events.h)
		add_executable(rtp_events_test test/rtp_events_test.cpp
			rtp_events.cpp)
		target_include_directories(rtp_events_test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR})
		dmodem_flags(rtp_events_test EXTRA NOOPT)
		target_link_libraries(rtp_events_test PRIVATE ${DMODEM_LIBM})
		dmodem_gtest(rtp_events_test LABELS quick)
		# test/pipe_test.cpp: dmodemd.exe's named pipe DTE, both
		# ways (and a bridge for asterisk/test's WIN_PIPE=1)
		if(WIN32)
			add_executable(pipe_test test/pipe_test.cpp)
			dmodem_flags(pipe_test EXTRA NOOPT)
			target_link_libraries(pipe_test PRIVATE ws2_32)
			add_dependencies(pipe_test dmodemd)
			dmodem_gtest(pipe_test LABELS quick TIMEOUT 120)
		endif()
	endif()

	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(DMODEM_TESTS AND Python3_Interpreter_FOUND AND NOT WIN32)
		dmodem_add_test(dmodemd_root ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test/root_test.py
			$<TARGET_FILE:dmodemd>)
		set_tests_properties(dmodemd_root PROPERTIES LABELS quick
			SKIP_RETURN_CODE 77 TIMEOUT 60)
		dmodem_add_test(hda_modem_setup ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test/hda_modem_setup_test.py
			${CMAKE_CURRENT_SOURCE_DIR}/hda-modem-setup)
		set_tests_properties(hda_modem_setup PROPERTIES LABELS quick
			TIMEOUT 60)
		if(ALSA_FOUND)
			dmodem_add_test(dmodemd_alsa ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/test/alsa_test.py
				$<TARGET_FILE:dmodemd>
				$<TARGET_FILE:line_exchange>
				$<TARGET_FILE_DIR:asound_module_dmline>)
			set_tests_properties(dmodemd_alsa PROPERTIES
				LABELS alsa TIMEOUT 300 COST 1000)
		endif()
		if(LIBUSB_FOUND)
			dmodem_add_test(dmodemd_usb ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/test/usb_test.py
				$<TARGET_FILE:dmodemd>
				$<TARGET_FILE:line_exchange>
				$<TARGET_FILE:dmodem_fake_st7554>)
			set_tests_properties(dmodemd_usb PROPERTIES
				LABELS alsa TIMEOUT 300 COST 1000)
		endif()
	endif()
endif()

# modem_test: two modems back to back, each with a pty
if(NOT WIN32)
	add_executable(modem_test modem_test.cpp)
	dmodem_flags(modem_test EXTRA)
	target_link_libraries(modem_test PRIVATE dmodem_front dmodem_core
		dmodem_dsp dmodem_link ${DMODEM_LIBM} Threads::Threads)
endif()

# libdmodem: the objects of dmodem_core, dmodem_dsp, dmodem_t30 and
# dmodem_link go into libdmodem.a
add_library(dmodem STATIC dmodem.cpp dmodem_fax.cpp
	$<TARGET_OBJECTS:dmodem_core> $<TARGET_OBJECTS:dmodem_dsp>
	$<TARGET_OBJECTS:dmodem_t30> $<TARGET_OBJECTS:dmodem_link>)
target_include_directories(dmodem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE ${DMODEM_CORE_NEW_DIR} ${DMODEM_DSP_DIR} ${DMODEM_LINK_DIR})
dmodem_flags(dmodem)
set_target_properties(dmodem PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dmodem PUBLIC ${DMODEM_LIBM} Threads::Threads)
if(WIN32)
	# (the DTE's socket: Winsock)
	target_link_libraries(dmodem PUBLIC ws2_32)
endif()
install(TARGETS dmodem ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES dmodem.h dmodem.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
