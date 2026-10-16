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

set(_front options.cpp pty.cpp datafile.cpp pipe_modem.cpp run_loop.cpp
	line_recorder.cpp)
list(TRANSFORM _front PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
set(DMODEM_FRONT_SRCS ${_front} PARENT_SCOPE)
set(DMODEM_FRONT_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
add_library(dmodem_front OBJECT ${_front})
target_link_libraries(dmodem_front PUBLIC dmodem_core)
target_compile_options(dmodem_front PRIVATE -Wall -Wextra -g -O2)

if(DMODEM_DMODEMD)
	# the modem daemon: its SIP user agent (sip.cpp) and a pty
	add_executable(dmodemd main.cpp sip.cpp sip_dial.cpp rtp_events.cpp)
	target_compile_options(dmodemd PRIVATE -Wall -g -O2)
	target_link_libraries(dmodemd PRIVATE dmodem_front dmodem_core
		dmodem_dsp dmodem_t30 dmodem_link libre m pthread)
	install(TARGETS dmodemd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
	# -a: a softmodem's codec through ALSA for the line (alsa_line.cpp),
	# if libasound2-dev is there
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
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
			target_compile_options(asound_module_dmline PRIVATE
				-Wall -Wextra -g)
			# (alsa-lib's plugin symbols, as a shared object
			# has them)
			target_compile_definitions(asound_module_dmline
				PRIVATE PIC)
			target_link_libraries(asound_module_dmline PRIVATE
				PkgConfig::ALSA)
			add_executable(line_exchange test/line_exchange.cpp)
			target_compile_options(line_exchange PRIVATE -Wall
				-Wextra -g -O2)
			target_link_libraries(line_exchange PRIVATE dmodem_core dmodem_dsp
				dmodem_link m pthread)
		endif()
	else()
		message(STATUS "dmodemd: no ALSA (libasound2-dev): no -a")
	endif()

	# test/root_test.py: dmodemd started as root, what needs root's
	# rights after it has dropped them (skipped unless ctest is root)
	# test/sip_dial_test.cpp: the SIP user of a dial string
	if(DMODEM_TESTS)
		add_executable(sip_dial_test test/sip_dial_test.cpp sip_dial.cpp)
		target_include_directories(sip_dial_test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR})
		target_compile_options(sip_dial_test PRIVATE -Wall -Wextra -g)
		dmodem_gtest(sip_dial_test LABELS quick)
		# test/rtp_events_test.cpp: the far end's telephone-events
		# as tones (rtp_events.h)
		add_executable(rtp_events_test test/rtp_events_test.cpp
			rtp_events.cpp)
		target_include_directories(rtp_events_test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR})
		target_compile_options(rtp_events_test PRIVATE -Wall -Wextra -g)
		target_link_libraries(rtp_events_test PRIVATE m)
		dmodem_gtest(rtp_events_test LABELS quick)
	endif()

	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(DMODEM_TESTS AND Python3_Interpreter_FOUND)
		dmodem_add_test(dmodemd_root ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/test/root_test.py
			$<TARGET_FILE:dmodemd>)
		set_tests_properties(dmodemd_root PROPERTIES LABELS quick
			SKIP_RETURN_CODE 77 TIMEOUT 60)
		if(ALSA_FOUND)
			dmodem_add_test(dmodemd_alsa ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/test/alsa_test.py
				$<TARGET_FILE:dmodemd>
				$<TARGET_FILE:line_exchange>
				$<TARGET_FILE_DIR:asound_module_dmline>)
			set_tests_properties(dmodemd_alsa PROPERTIES
				LABELS alsa TIMEOUT 300 COST 1000)
		endif()
	endif()
endif()

# modem_test: two modems back to back, each with a pty
add_executable(modem_test modem_test.cpp)
target_compile_options(modem_test PRIVATE -Wall -Wextra -g -O2)
target_link_libraries(modem_test PRIVATE dmodem_front dmodem_core dmodem_dsp
	dmodem_link m pthread)

# libdmodem: the objects of dmodem_core, dmodem_dsp, dmodem_t30 and
# dmodem_link go into libdmodem.a
add_library(dmodem STATIC dmodem.cpp dmodem_fax.cpp
	$<TARGET_OBJECTS:dmodem_core> $<TARGET_OBJECTS:dmodem_dsp>
	$<TARGET_OBJECTS:dmodem_t30> $<TARGET_OBJECTS:dmodem_link>)
target_include_directories(dmodem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE ${DMODEM_CORE_NEW_DIR} ${DMODEM_DSP_DIR} ${DMODEM_LINK_DIR})
target_compile_options(dmodem PRIVATE -Wall -g -O2)
set_target_properties(dmodem PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dmodem PUBLIC m pthread)
install(TARGETS dmodem ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES dmodem.h dmodem.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
