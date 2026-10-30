# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
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
	# the DTE a COM port, a pipe, the virtual modem driver or a TCP
	# connection (windows_dte.cpp), for the pty and the pipe modems of
	# POSIX; with --driver's frames (../winmodem/wire.cpp)
	list(APPEND _front windows_dte.cpp ../winmodem/wire.cpp)
else()
	list(APPEND _front pty.cpp pipe_modem.cpp)
endif()
list(TRANSFORM _front PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
set(DMODEM_FRONT_SRCS ${_front} PARENT_SCOPE)
set(DMODEM_FRONT_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
add_library(dmodem_front OBJECT ${_front})
target_link_libraries(dmodem_front PUBLIC dmodem_core)
if(WIN32)
	target_include_directories(dmodem_front PUBLIC
		${PROJECT_SOURCE_DIR}/winmodem)
endif()
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
		# for -a (docs/hardware.md): Linux's
		if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
			install(PROGRAMS hda-modem-setup
				DESTINATION ${CMAKE_INSTALL_SBINDIR})
		endif()
	endif()
	dmodem_flags(dmodemd)
	# dmodem_usb_fake(<fake> <main> <libusb headers>...): the tests'
	# dmodemd_fake_<fake>, dmodemd with test/fake_<fake>.cpp in libusb's
	# place, linked into it (where there is no LD_PRELOAD to put it in
	# front of libusb: Windows, macOS; DMODEM_TEST_BUILTIN_FAKES on
	# Linux); usb_test.py, pico_line_test.py, pico_wb_test.py and
	# agere_test.py run it with "-" for the fake
	function(dmodem_usb_fake fake main)
		add_executable(dmodemd_fake_${fake} ${main} usb_line.cpp
			usb_agere.cpp usb_wb.cpp si3056.cpp test/fake_${fake}.cpp)
		target_compile_definitions(dmodemd_fake_${fake} PRIVATE
			DMODEM_USB)
		target_include_directories(dmodemd_fake_${fake} SYSTEM PRIVATE
			${ARGN})
		target_include_directories(dmodemd_fake_${fake} PRIVATE
			${PROJECT_SOURCE_DIR}/dsp)
		target_link_libraries(dmodemd_fake_${fake} PRIVATE dmodem_sip)
		if(WIN32)
			target_link_libraries(dmodemd_fake_${fake} PRIVATE
				winmm ws2_32)
		endif()
		dmodem_flags(dmodemd_fake_${fake})
	endfunction()
	# -a: a softmodem's codec through ALSA for the line (alsa_line.cpp),
	# if libasound2-dev is there
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND AND NOT WIN32)
		pkg_check_modules(ALSA QUIET IMPORTED_TARGET alsa)
	endif()
	if(ALSA_FOUND)
		target_sources(dmodemd PRIVATE alsa_line.cpp hda_codec.cpp si3054.cpp
			cx_daa.cpp cx_country.cpp ac97_ports.cpp)
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
			# and an HD-audio Si3054's hwdep device in front of
			# alsa-lib's (LD_PRELOAD): the country's line settings
			add_library(dmodem_fake_hda_hwdep SHARED
				test/fake_hda_hwdep.cpp)
			dmodem_flags(dmodem_fake_hda_hwdep EXTRA NOOPT)
			target_link_libraries(dmodem_fake_hda_hwdep PRIVATE
				PkgConfig::ALSA)
		endif()
	else()
		message(STATUS "dmodemd: no ALSA (libasound2-dev): no -a")
	endif()
	# --usb: a Smart Link SmartUSB56 (ST7554), a pico-serial Line or WB
	# stick or an Agere USB modem through libusb for the line
	# (usb_line.cpp, usb_wb.cpp, usb_agere.cpp), if libusb-1.0-0-dev is
	# there
	if(PkgConfig_FOUND AND NOT WIN32)
		pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
	endif()
	# (Windows: libusb fetched and built here, cmake/libusb.cmake; its
	# devices on WinUSB, docs/windows.md)
	if((WIN32 OR APPLE) AND DMODEM_USB)
		include(libusb)
		target_sources(dmodemd PRIVATE usb_line.cpp usb_agere.cpp usb_wb.cpp si3056.cpp)
		target_compile_definitions(dmodemd PRIVATE DMODEM_USB)
		target_link_libraries(dmodemd PRIVATE dmodem_libusb)
		set(_usb_headers
			$<TARGET_PROPERTY:dmodem_libusb,INTERFACE_INCLUDE_DIRECTORIES>)
		# the tests' dmodemd with a fake modem in libusb's place (no
		# LD_PRELOAD on Windows, none to rely on on macOS): Windows'
		# under Wine, on the exchange of a Linux build
		# (DMODEM_HOST_LINE_EXCHANGE: its line_exchange); macOS's on
		# its own (below, with Linux's)
		if(DMODEM_TESTS AND WIN32)
			set(DMODEM_HOST_LINE_EXCHANGE
				"${PROJECT_SOURCE_DIR}/build/dmodemd/line_exchange"
				CACHE FILEPATH
				"a Linux build's line_exchange (Windows USB tests)")
			find_package(Python3 COMPONENTS Interpreter QUIET)
			foreach(_f st7554 pico_line pico_wb)
				dmodem_usb_fake(${_f} main_windows.cpp ${_usb_headers})
			endforeach()
			if(Python3_Interpreter_FOUND AND CMAKE_CROSSCOMPILING
			   AND EXISTS "${DMODEM_HOST_LINE_EXCHANGE}")
				dmodem_add_test(dmodemd_usb_windows
					${Python3_EXECUTABLE}
					${CMAKE_CURRENT_SOURCE_DIR}/test/usb_test.py
					$<TARGET_FILE:dmodemd_fake_st7554>
					${DMODEM_HOST_LINE_EXCHANGE} -)
				dmodem_add_test(dmodemd_pico_line_windows
					${Python3_EXECUTABLE}
					${CMAKE_CURRENT_SOURCE_DIR}/test/pico_line_test.py
					$<TARGET_FILE:dmodemd_fake_pico_line>
					${DMODEM_HOST_LINE_EXCHANGE} -)
				# (serial: real-time calls under Wine, each
				# test's own Wine processes started alone)
				set_tests_properties(dmodemd_usb_windows
					dmodemd_pico_line_windows PROPERTIES
					LABELS alsa TIMEOUT 400 RUN_SERIAL TRUE)
			endif()
		elseif(DMODEM_TESTS)
			set(_usb_builtin_fakes ON)
		endif()
	elseif(LIBUSB_FOUND)
		target_sources(dmodemd PRIVATE usb_line.cpp usb_agere.cpp usb_wb.cpp si3056.cpp)
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
				${LIBUSB_INCLUDE_DIRS} ${PROJECT_SOURCE_DIR}/dsp)
			# test/pico_line_test.py: the same on fake pico-serial
			# Line sticks (test/fake_pico_line.cpp)
			add_library(dmodem_fake_pico_line SHARED
				test/fake_pico_line.cpp)
			dmodem_flags(dmodem_fake_pico_line EXTRA NOOPT)
			target_include_directories(dmodem_fake_pico_line PRIVATE
				${LIBUSB_INCLUDE_DIRS})
			# test/pico_wb_test.py: two on fake pico-serial WB
			# sticks (test/fake_pico_wb.cpp) at the two ends of a
			# simulated bare pair (test/wb_pair.cpp)
			add_library(dmodem_fake_pico_wb SHARED
				test/fake_pico_wb.cpp)
			dmodem_flags(dmodem_fake_pico_wb EXTRA NOOPT)
			target_include_directories(dmodem_fake_pico_wb PRIVATE
				${LIBUSB_INCLUDE_DIRS})
			# test/agere_test.py: on fake Agere USB modems
			# (test/fake_agere.cpp)
			add_library(dmodem_fake_agere SHARED
				test/fake_agere.cpp)
			dmodem_flags(dmodem_fake_agere EXTRA NOOPT)
			target_include_directories(dmodem_fake_agere PRIVATE
				${LIBUSB_INCLUDE_DIRS})
			# (the fakes linked in libusb's place too, as macOS's
			# tests have them: their own tests, *_builtin)
			option(DMODEM_TEST_BUILTIN_FAKES
				"Linux: also test dmodemd with the fake USB modems linked in" OFF)
			if(DMODEM_TEST_BUILTIN_FAKES)
				set(_usb_builtin_fakes ON)
				set(_usb_headers ${LIBUSB_INCLUDE_DIRS})
			endif()
		endif()
	else()
		message(STATUS "dmodemd: no libusb (libusb-1.0-0-dev): no --usb")
	endif()
	if(_usb_builtin_fakes)
		foreach(_f st7554 pico_line pico_wb agere)
			dmodem_usb_fake(${_f} main.cpp ${_usb_headers})
		endforeach()
	endif()
	# the simulated telephone exchange of the ALSA and USB lines' tests
	if(DMODEM_TESTS AND (ALSA_FOUND OR LIBUSB_FOUND OR _usb_builtin_fakes))
		add_executable(line_exchange test/line_exchange.cpp)
		dmodem_flags(line_exchange EXTRA)
		target_link_libraries(line_exchange PRIVATE dmodem_core dmodem_dsp dmodem_t30
			dmodem_link ${DMODEM_LIBM} Threads::Threads)
	endif()
	# the simulated bare pair of the WB stick's test, on the V.wb tests'
	# (dsp/test/vwb_line.cpp)
	if(DMODEM_TESTS AND (LIBUSB_FOUND OR _usb_builtin_fakes))
		add_executable(wb_pair test/wb_pair.cpp
			${PROJECT_SOURCE_DIR}/dsp/test/vwb_line.cpp
			${PROJECT_SOURCE_DIR}/dsp/vwb.cpp
			${PROJECT_SOURCE_DIR}/dsp/rs.cpp)
		target_include_directories(wb_pair PRIVATE
			${PROJECT_SOURCE_DIR}/dsp/test ${PROJECT_SOURCE_DIR}/dsp
			${DMODEM_LINK_DIR} ${DMODEM_CORE_NEW_DIR})
		dmodem_flags(wb_pair EXTRA)
		target_link_libraries(wb_pair PRIVATE ${DMODEM_LIBM}
			Threads::Threads)
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
		# test/si3056_test.cpp: a country's line settings for the
		# pico-serial Line's DAA (si3056.h)
		add_executable(si3056_test test/si3056_test.cpp si3056.cpp)
		target_include_directories(si3056_test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR})
		dmodem_flags(si3056_test EXTRA NOOPT)
		dmodem_gtest(si3056_test LABELS quick)
		# test/si3054_test.cpp: the same for an HD-audio Si3054's
		# (si3054.h)
		add_executable(si3054_test test/si3054_test.cpp si3054.cpp)
		target_include_directories(si3054_test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR})
		dmodem_flags(si3054_test EXTRA NOOPT)
		dmodem_gtest(si3054_test LABELS quick)
		# test/cx_daa_test.cpp: a Conexant AC'97 codec's SmartDAA
		# (cx_daa.h) against a simulated one
		add_executable(cx_daa_test test/cx_daa_test.cpp cx_daa.cpp
			       cx_country.cpp)
		target_include_directories(cx_daa_test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR})
		dmodem_flags(cx_daa_test EXTRA NOOPT)
		dmodem_gtest(cx_daa_test LABELS quick)
		# test/pipe_test.cpp: dmodemd.exe's named pipe DTE, both
		# ways (and a bridge for asterisk/test's WIN_PIPE=1)
		if(WIN32)
			add_executable(pipe_test test/pipe_test.cpp)
			dmodem_flags(pipe_test EXTRA NOOPT)
			target_link_libraries(pipe_test PRIVATE ws2_32)
			add_dependencies(pipe_test dmodemd)
			dmodem_gtest(pipe_test LABELS quick TIMEOUT 120)
			# ../winmodem/test/vmodem_test.cpp: dmodemd.exe on the
			# virtual modem driver's pipe (--driver), the driver's
			# end of it run there (and a bridge for asterisk/test's
			# WIN_DRIVER=1); beside dmodemd.exe.  (Serial: --driver
			# auto takes the first driver's pipe there is.)
			add_executable(vmodem_test
				${PROJECT_SOURCE_DIR}/winmodem/test/vmodem_test.cpp)
			dmodem_flags(vmodem_test EXTRA NOOPT)
			target_link_libraries(vmodem_test PRIVATE dmodem_winlink
				ws2_32)
			add_dependencies(vmodem_test dmodemd)
			dmodem_gtest(vmodem_test LABELS quick TIMEOUT 180
				RUN_SERIAL)
			# ../winmodem/test/umdf1_host.cpp: the driver for
			# Windows 7 and 8 (dmodemvm7.dll, UMDF 1) in a host of
			# the test's own, dmodemd.exe on its pipe; both beside
			# it
			if(TARGET dmodemvm7)
				add_executable(umdf1_test
					${PROJECT_SOURCE_DIR}/winmodem/test/umdf1_host.cpp)
				dmodem_flags(umdf1_test NOOPT)
				target_include_directories(umdf1_test SYSTEM PRIVATE
					${DMODEM_UMDF1_INCLUDE})
				target_link_libraries(umdf1_test PRIVATE
					dmodem_winmodem ole32 uuid)
				# (the driver copied beside it whenever it is
				# built again)
				add_custom_command(
					OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dmodemvm7.dll
					COMMAND ${CMAKE_COMMAND} -E copy
					$<TARGET_FILE:dmodemvm7>
					${CMAKE_CURRENT_BINARY_DIR}/dmodemvm7.dll
					DEPENDS dmodemvm7)
				add_custom_target(umdf1_test_driver ALL DEPENDS
					${CMAKE_CURRENT_BINARY_DIR}/dmodemvm7.dll)
				add_dependencies(umdf1_test dmodemd umdf1_test_driver)
				dmodem_gtest(umdf1_test LABELS quick TIMEOUT 180
					RUN_SERIAL)
			endif()
		endif()
	endif()

	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(DMODEM_TESTS AND Python3_Interpreter_FOUND AND NOT WIN32)
		dmodem_add_test(dmodemd_root ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test/root_test.py
			$<TARGET_FILE:dmodemd>)
		set_tests_properties(dmodemd_root PROPERTIES LABELS quick
			SKIP_RETURN_CODE 77 TIMEOUT 60)
		# the clear 64 kbit/s channel over SIP (CLEARMODE), against
		# the script's own SIP peer and clear channel
		dmodem_add_test(dmodemd_clearmode ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test/clearmode_test.py
			$<TARGET_FILE:dmodemd>)
		set_tests_properties(dmodemd_clearmode PROPERTIES LABELS quick
			TIMEOUT 60)
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
				$<TARGET_FILE_DIR:asound_module_dmline>
				$<TARGET_FILE:dmodem_fake_hda_hwdep>)
			set_tests_properties(dmodemd_alsa PROPERTIES
				TIMEOUT 300 COST 1000)
			set_property(TEST dmodemd_alsa APPEND PROPERTY LABELS alsa)
		endif()
		# (the preloaded fakes: where they were built, not on macOS,
		# whose tests below have them linked in under the same names)
		if(TARGET dmodem_fake_st7554)
			dmodem_add_test(dmodemd_usb ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/test/usb_test.py
				$<TARGET_FILE:dmodemd>
				$<TARGET_FILE:line_exchange>
				$<TARGET_FILE:dmodem_fake_st7554>)
			set_tests_properties(dmodemd_usb PROPERTIES
				TIMEOUT 300 COST 1000)
			set_property(TEST dmodemd_usb APPEND PROPERTY LABELS alsa)
			dmodem_add_test(dmodemd_pico_line ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/test/pico_line_test.py
				$<TARGET_FILE:dmodemd>
				$<TARGET_FILE:line_exchange>
				$<TARGET_FILE:dmodem_fake_pico_line>)
			set_tests_properties(dmodemd_pico_line PROPERTIES
				TIMEOUT 300 COST 1000)
			set_property(TEST dmodemd_pico_line APPEND PROPERTY LABELS alsa)
			dmodem_add_test(dmodemd_pico_wb ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/test/pico_wb_test.py
				$<TARGET_FILE:dmodemd>
				$<TARGET_FILE:wb_pair>
				$<TARGET_FILE:dmodem_fake_pico_wb>)
			# (quick too: about 20 s, a call and its data; alone,
			# as its two modems run in real time at 96 kHz and a
			# loaded machine's stalls slip their samples)
			set_tests_properties(dmodemd_pico_wb PROPERTIES
				TIMEOUT 300 COST 1000 RUN_SERIAL TRUE)
			set_property(TEST dmodemd_pico_wb APPEND PROPERTY LABELS
				alsa quick)
			dmodem_add_test(dmodemd_agere ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/test/agere_test.py
				$<TARGET_FILE:dmodemd>
				$<TARGET_FILE:line_exchange>
				$<TARGET_FILE:dmodem_fake_agere>)
			set_tests_properties(dmodemd_agere PROPERTIES
				TIMEOUT 300 COST 1000)
			set_property(TEST dmodemd_agere APPEND PROPERTY LABELS alsa)
		endif()
		# the same with the fakes linked in (macOS's; on Linux with
		# DMODEM_TEST_BUILTIN_FAKES, as dmodemd_*_builtin)
		if(_usb_builtin_fakes)
			if(APPLE)
				set(_sfx "")
			else()
				set(_sfx _builtin)
			endif()
			# (<test>:<script>:<fake>:<its line's program>)
			foreach(_t usb:usb_test:st7554:line_exchange
				pico_line:pico_line_test:pico_line:line_exchange
				pico_wb:pico_wb_test:pico_wb:wb_pair
				agere:agere_test:agere:line_exchange)
				string(REPLACE ":" ";" _t ${_t})
				list(GET _t 0 _name)
				list(GET _t 1 _script)
				list(GET _t 2 _fake)
				list(GET _t 3 _line)
				dmodem_add_test(dmodemd_${_name}${_sfx}
					${Python3_EXECUTABLE}
					${CMAKE_CURRENT_SOURCE_DIR}/test/${_script}.py
					$<TARGET_FILE:dmodemd_fake_${_fake}>
					$<TARGET_FILE:${_line}> -)
				set_tests_properties(dmodemd_${_name}${_sfx}
					PROPERTIES TIMEOUT 300 COST 1000)
				set_property(TEST dmodemd_${_name}${_sfx} APPEND
					PROPERTY LABELS alsa)
			endforeach()
		endif()
	endif()
endif()

# modem_test: two modems back to back, each with a pty
if(NOT WIN32)
	add_executable(modem_test modem_test.cpp)
	dmodem_flags(modem_test EXTRA)
	target_link_libraries(modem_test PRIVATE dmodem_front dmodem_core dmodem_t30
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
