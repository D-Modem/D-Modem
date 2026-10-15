# Test programs of the open data pumps, the library and the fax, and their
# ctest registrations.  Labels:
#   quick     offline, faster than real time (a few minutes in all)
#   spandsp   against spandsp's V.17 transmitter and T.4 encoder
#             (libspandsp-dev)
#   smartlink, agere, sm56, pctel, hsf, spanpeer   calls with the other
#             soft modems: Smart Link's (its original binary library),
#             the Agere, SM56, PCTel and HSF ones, spandsp's (needs gcc
#             -m32 on x86_64); long
#   open      calls between two open modems through the same driver
#
#   ctest --test-dir build -L quick
set(S ${DMODEM_DSP_DIR})
set(_dsp ${DMODEM_DSP_SRCS})
# slmodemd's old C core (legacy/): Smart Link's original library calls
# into it (half_smartlink), and core_script_old compares the new core with it;
# both run its timers by hand on a clock of their own (dmodem_timer.cpp,
# not the SIGALRM ones of modem_timer.cpp)
set(DMODEM_LEGACY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/legacy)
set(DMODEM_LEGACY_SRCS modem.cpp modem_datafile.cpp modem_at.cpp
	dmodem_timer.cpp modem_param.cpp modem_debug.cpp sysdep_common.cpp)
list(TRANSFORM DMODEM_LEGACY_SRCS PREPEND ${DMODEM_LEGACY_DIR}/)
list(APPEND DMODEM_LEGACY_SRCS ${DMODEM_CORE_NEW_DIR}/homolog_data.cpp)
set(DMODEM_LEGACY_DIR ${DMODEM_LEGACY_DIR} PARENT_SCOPE)
set(DMODEM_LEGACY_SRCS ${DMODEM_LEGACY_SRCS} PARENT_SCOPE)
set(C ${DMODEM_LEGACY_DIR})
set(_fax ${S}/fax_modems.cpp ${S}/qam.cpp ${S}/qam_tables.cpp ${S}/dsp_util.cpp
	${S}/dp_fsk.cpp)

function(dmodem_test_exe name)
	add_executable(${name} ${ARGN})
	target_include_directories(${name} PRIVATE ${S} ${DMODEM_LINK_DIR}
		${DMODEM_CORE_NEW_DIR})
	target_compile_options(${name} PRIVATE -Wall -O2 -g)
	target_link_libraries(${name} PRIVATE m pthread)
endfunction()

# the data pumps back to back: pump_test, and dsp_loopback to try a line
# from the command line
add_library(dmodem_loopback OBJECT loopback.cpp test_util.cpp)
target_include_directories(dmodem_loopback PRIVATE ${S})
target_compile_options(dmodem_loopback PRIVATE -Wall -O2 -g)
dmodem_test_exe(dsp_loopback dsp_loopback.cpp)
target_link_libraries(dsp_loopback PRIVATE dmodem_loopback dmodem_dsp)
dmodem_test_exe(pump_test pump_test.cpp)
target_link_libraries(pump_test PRIVATE dmodem_loopback dmodem_dsp)
dmodem_gtest(pump_test LABELS quick)
# the PCM encoders against unusable CPs (the core's calls and the log
# from dmodem_loopback)
dmodem_test_exe(pcm_cp_test pcm_cp_test.cpp)
target_link_libraries(pcm_cp_test PRIVATE dmodem_loopback dmodem_dsp)
dmodem_gtest(pcm_cp_test LABELS quick)
# bits.h: fields, the V.34 and frame CRCs, the HDLC FCS
dmodem_test_exe(bits_test bits_test.cpp)
dmodem_gtest(bits_test LABELS quick)
# g711.h: the laws' octets, reference values, sign and magnitude
dmodem_test_exe(g711_test g711_test.cpp)
dmodem_gtest(g711_test LABELS quick)
# the emulated hardware's mixins (hw/) on a host of the test's own
dmodem_test_exe(hw_test hw_test.cpp)
dmodem_gtest(hw_test LABELS quick)
dmodem_test_exe(netsim netsim.cpp test_util.cpp ${S}/dsp_util.cpp ${S}/qam.cpp
	${S}/v34_tables.cpp)
# the fax without a core: the modulations, T.4 and TIFF, two T.30 machines
find_program(TIFFCP tiffcp)
dmodem_test_exe(fax_test fax_test.cpp fax_docs.cpp fax_stubs.cpp test_util.cpp ${S}/t30.cpp
	${S}/t4.cpp ${S}/tiff.cpp ${_fax})
target_compile_definitions(fax_test PRIVATE "TIFFCP=\"${TIFFCP}\"")
dmodem_gtest(fax_test LABELS quick)
# the test document for the Asterisk and FreeSWITCH fax scripts
dmodem_test_exe(fax_doc fax_doc.cpp fax_docs.cpp ${S}/t4.cpp ${S}/tiff.cpp)
target_link_libraries(fax_doc PRIVATE GTest::gtest)
# libdmodem: pairs of its modems back to back
dmodem_test_exe(dmodem_test dmodem_test.cpp)
target_link_libraries(dmodem_test PRIVATE dmodem)
dmodem_gtest(dmodem_test LABELS quick)
dmodem_test_exe(dmodem_fax_test dmodem_fax_test.cpp fax_docs.cpp)
target_link_libraries(dmodem_fax_test PRIVATE dmodem)
dmodem_gtest(dmodem_fax_test LABELS quick)

# --- spandsp: as Asterisk's SendFAX uses it (libspandsp-dev)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
	pkg_check_modules(SPANDSP QUIET spandsp)
endif()
if(SPANDSP_FOUND)
	# its V.17 transmitter and T.4 encoder into our receivers; our T.30
	# against its (fax_state_t), both ways, with and without error
	# correction mode, ECM also with line hits
	dmodem_test_exe(spandsp_test spandsp_test.cpp spandsp_tx.cpp
		t30_spandsp.cpp fax_docs.cpp fax_stubs.cpp test_util.cpp ${S}/t30.cpp
		${S}/t4.cpp ${S}/tiff.cpp ${_fax})
	target_include_directories(spandsp_test PRIVATE ${SPANDSP_INCLUDE_DIRS})
	target_link_libraries(spandsp_test PRIVATE ${SPANDSP_LDFLAGS})
	dmodem_gtest(spandsp_test LABELS spandsp)
else()
	message(STATUS "tests: spandsp not found, its tests skipped")
endif()

# --- the interop calls: Smart Link's original library on slmodemd's old
#     core (half_smartlink, legacy/modem_half.cpp) and the open data pumps
#     on the modem core (half_open, 32 bit like the library), and the
#     other modems below (classes on modem_half.cpp), through
#     interop_test.cpp (interop_call.cpp), with netsim for the G.711
#     network.
#     The original library (smartlink/dsplibs.o, Smart Link's binary-only
#     DSP library) is a 32 bit i386 object: -m32 on x86_64.
set(DMODEM_M32 "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
	set(DMODEM_M32 -m32)
endif()
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "${DMODEM_M32}")
set(CMAKE_REQUIRED_LINK_OPTIONS "${DMODEM_M32}")
check_cxx_source_compiles("int main() { return 0; }" DMODEM_HAVE_M32)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(NOT DMODEM_HAVE_M32 OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i.86)$")
	message(STATUS "tests: no 32 bit i386 compiler, interop tests skipped")
	return()
endif()
foreach(_rate 9600 8000)
	foreach(_kind smartlink open)
		set(_n half_${_kind})
		if(_rate EQUAL 8000)
			set(_n ${_n}8)
		endif()
		if(_kind STREQUAL "smartlink")
			# the original library calls into slmodemd's old core
			add_executable(${_n} ${C}/modem_half.cpp
				${C}/modem_cmdline.cpp ${DMODEM_LEGACY_SRCS}
				${DMODEM_LINK_SRCS}
				${CMAKE_CURRENT_SOURCE_DIR}/smartlink/dsplibs.o)
			target_include_directories(${_n} PRIVATE ${C} ${S}
				${DMODEM_LINK_DIR} ${DMODEM_CORE_NEW_DIR})
			set(_l -no-pie -Wl,-z,noexecstack)
		else()
			# the open data pumps on the modem core
			add_executable(${_n} half_open.cpp ${DMODEM_FRONT_SRCS}
				${DMODEM_CORE_NEW_SRCS} ${DMODEM_LINK_SRCS} ${_dsp})
			target_include_directories(${_n} PRIVATE
				${DMODEM_FRONT_DIR} ${S} ${DMODEM_LINK_DIR}
				${DMODEM_CORE_NEW_DIR})
			set(_l "")
		endif()
		target_compile_definitions(${_n} PRIVATE CONFIG_DEBUG_MODEM
			MODEM_RATE=${_rate})
		target_compile_options(${_n} PRIVATE ${DMODEM_M32} -w -g -O)
		target_link_options(${_n} PRIVATE ${DMODEM_M32} ${_l})
		target_link_libraries(${_n} PRIVATE m pthread)
	endforeach()
endforeach()

# the peers' harnesses (modem_half.cpp) read their line as PipeModem does
# (line_frames.h)
set_source_files_properties(modem_half.cpp PROPERTIES INCLUDE_DIRECTORIES
	${DMODEM_FRONT_DIR})

# the Agere Systems soft modem as another peer (half_agere.cpp): its library,
# agrsm_core.o of agrsm-20070804 (Agere SoftModem 2.1.40; docs/interop.md)
set(DMODEM_AGERE_LIB "${CMAKE_CURRENT_SOURCE_DIR}/agere/agrsm_core.o" CACHE
	FILEPATH "agrsm_core.o of the Agere soft modem (interop tests)")
if(EXISTS "${DMODEM_AGERE_LIB}")
	add_executable(half_agere half_agere.cpp modem_half.cpp ${DMODEM_AGERE_LIB})
	target_compile_options(half_agere PRIVATE ${DMODEM_M32} -g -O1)
	target_link_options(half_agere PRIVATE ${DMODEM_M32} -no-pie
		-Wl,-z,noexecstack)
	target_link_libraries(half_agere PRIVATE m)
	message(STATUS "tests: the Agere soft modem: ${DMODEM_AGERE_LIB}")
endif()

# spandsp's data modems as a peer (half_spandsp.cpp): its V.21, V.23,
# Bell 103 and V.22/V.22bis pumps (libspandsp 0.0.6, as Asterisk and
# FreeSWITCH have it) in a modem of their own
if(SPANDSP_FOUND)
	add_executable(half_spandsp half_spandsp.cpp modem_half.cpp)
	target_include_directories(half_spandsp PRIVATE ${SPANDSP_INCLUDE_DIRS})
	target_link_libraries(half_spandsp PRIVATE ${SPANDSP_LDFLAGS} m)
endif()

# Motorola's SM56 soft modem as a peer (half_sm56.cpp): sm56.ko of
# sm56-06.09.05 for Fedora Core 3's kernel 2.6.9, its imports renamed
set(DMODEM_SM56_KO "${CMAKE_CURRENT_SOURCE_DIR}/sm56/sm56.ko" CACHE
	FILEPATH "sm56.ko of the Motorola SM56 soft modem (interop tests)")
if(EXISTS "${DMODEM_SM56_KO}")
	set(_sm56o ${CMAKE_CURRENT_BINARY_DIR}/sm56k.o)
	add_custom_command(OUTPUT ${_sm56o}
		COMMAND ${CMAKE_COMMAND} -DKO=${DMODEM_SM56_KO} -DOUT=${_sm56o}
			-DNM=${CMAKE_NM} -DOBJCOPY=${CMAKE_OBJCOPY}
			-P ${CMAKE_CURRENT_SOURCE_DIR}/sm56_obj.cmake
		DEPENDS ${DMODEM_SM56_KO} sm56_obj.cmake
		COMMENT "sm56.ko's imports renamed")
	add_executable(half_sm56 half_sm56.cpp modem_half.cpp ${_sm56o})
	set_source_files_properties(${_sm56o} PROPERTIES EXTERNAL_OBJECT TRUE
		GENERATED TRUE)
	target_compile_options(half_sm56 PRIVATE ${DMODEM_M32} -g -O1)
	target_link_options(half_sm56 PRIVATE ${DMODEM_M32} -no-pie
		-Wl,-z,noexecstack)
	target_link_libraries(half_sm56 PRIVATE m)
	message(STATUS "tests: the Motorola SM56 soft modem: ${DMODEM_SM56_KO}")
endif()

# PCTel's HSP soft modem as a peer (half_pctel.cpp): pctel-0.9.7-9's Intel
# AMR data pumps (lib/amr/dsp.a) and controller (lib/control.a), linked
# as they are
set(DMODEM_PCTEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pctel" CACHE
	PATH "dsp.o and control.o of the PCTel HSP soft modem (interop tests)")
if(EXISTS "${DMODEM_PCTEL_DIR}/dsp.o" AND EXISTS "${DMODEM_PCTEL_DIR}/control.o")
	add_executable(half_pctel half_pctel.cpp modem_half.cpp ${DMODEM_PCTEL_DIR}/dsp.o
		${DMODEM_PCTEL_DIR}/control.o)
	set_source_files_properties(${DMODEM_PCTEL_DIR}/dsp.o
		${DMODEM_PCTEL_DIR}/control.o PROPERTIES EXTERNAL_OBJECT TRUE)
	target_compile_options(half_pctel PRIVATE ${DMODEM_M32} -g -O1)
	target_link_options(half_pctel PRIVATE ${DMODEM_M32} -no-pie
		-Wl,-z,noexecstack)
	target_link_libraries(half_pctel PRIVATE m)
	message(STATUS "tests: the PCTel HSP soft modem: ${DMODEM_PCTEL_DIR}")
endif()

# Conexant's HSF soft modem as a peer (half_hsf.cpp): hsfmodem-7.80.02.06oem's
# engine (hsfengine), its AC'97 ICH codec module (hsfmc97ich) and its
# soft DAA module (hsfsoar), their sections renamed per object (and the
# ICH module's microsecond busy wait, hsfmc97ich41_, half_hsf.cpp's), and the
# modem's .inf and country file for its NVM
set(DMODEM_HSF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/hsf" CACHE
	PATH "objects and NVM files of the HSF soft modem (interop tests)")
if(EXISTS "${DMODEM_HSF_DIR}/hsfengine.o")
	set(_hsfo "")
	set(_weak_hsfmc97ich hsfmc97ich41_)
	foreach(_b hsfengine hsfmc97ich hsfsoar)
		set(_o ${CMAKE_CURRENT_BINARY_DIR}/${_b}_r.o)
		add_custom_command(OUTPUT ${_o}
			COMMAND ${CMAKE_COMMAND} -DIN=${DMODEM_HSF_DIR}/${_b}.o
				-DOUT=${_o} -DNAME=${_b}
				"-DWEAK=${_weak_${_b}}" -DOBJDUMP=${CMAKE_OBJDUMP}
				-DOBJCOPY=${CMAKE_OBJCOPY}
				-P ${CMAKE_CURRENT_SOURCE_DIR}/hsf_obj.cmake
			DEPENDS ${DMODEM_HSF_DIR}/${_b}.o hsf_obj.cmake
			COMMENT "${_b}.o's sections renamed")
		list(APPEND _hsfo ${_o})
	endforeach()
	add_executable(half_hsf half_hsf.cpp modem_half.cpp ${_hsfo})
	set_source_files_properties(${_hsfo} PROPERTIES EXTERNAL_OBJECT TRUE
		GENERATED TRUE)
	target_compile_definitions(half_hsf PRIVATE
		HSF_NVM_DIR="${DMODEM_HSF_DIR}")
	target_compile_options(half_hsf PRIVATE ${DMODEM_M32} -g -O1)
	target_link_options(half_hsf PRIVATE ${DMODEM_M32} -no-pie
		-Wl,-z,noexecstack)
	target_link_libraries(half_hsf PRIVATE m)
	message(STATUS "tests: the Conexant HSF soft modem: ${DMODEM_HSF_DIR}")
endif()

# Broadcom's V.92 soft modem for Windows as a peer (half_broadcom.cpp): its
# WDM driver (BCMSM.sys 3.4.19, a PE image) and its INF (the registry), on
# interop_windows.cpp, a Windows kernel of our own for such drivers
set(DMODEM_BROADCOM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/broadcom" CACHE
	PATH "BCMSM.sys and BCMSMwxp.inf of the Broadcom soft modem (interop tests)")
if(EXISTS "${DMODEM_BROADCOM_DIR}/BCMSM.sys")
	add_executable(half_broadcom half_broadcom.cpp interop_windows.cpp
		modem_half.cpp)
	target_compile_definitions(half_broadcom PRIVATE
		BROADCOM_SYS="${DMODEM_BROADCOM_DIR}/BCMSM.sys"
		BROADCOM_INF="${DMODEM_BROADCOM_DIR}/BCMSMwxp.inf")
	target_compile_options(half_broadcom PRIVATE ${DMODEM_M32} -g -O1)
	target_link_options(half_broadcom PRIVATE ${DMODEM_M32} -no-pie
		-Wl,-z,noexecstack)
	target_link_libraries(half_broadcom PRIVATE m)
	message(STATUS "tests: the Broadcom soft modem: ${DMODEM_BROADCOM_DIR}")
endif()

# the interop tests: calls between the open modem and each of the modems
# above (interop_test.cpp, through interop_call.cpp: what a call does is in
# its environment, NET for netsim's G.711 network, STREAM, EXPECT_B, ...).
# One test program a label; each call writes its modem logs to
# logs/<test>/.  The calls run as fast as they can (fax: ten times real
# time), on the line's clock, a little nicer (each takes a processor while
# it runs, but for the moments on hook in real time).  So ctest -j can be
# some four times the processors (-j16 on 4).  interop_call makes one by
# hand.
add_library(interop_driver STATIC interop_call.cpp)
target_compile_options(interop_driver PRIVATE -Wall -O2 -g)
target_link_libraries(interop_driver PUBLIC pthread)
dmodem_test_exe(interop_call interop_call_main.cpp)
target_link_libraries(interop_call PRIVATE interop_driver)
set(_progs "HALF_SMARTLINK=\"$<TARGET_FILE:half_smartlink>\""
	"HALF_OPEN=\"$<TARGET_FILE:half_open>\""
	"HALF_OPEN8=\"$<TARGET_FILE:half_open8>\"")
foreach(_p agere sm56 spandsp pctel hsf broadcom)
	if(TARGET half_${_p})
		string(TOUPPER ${_p} _u)
		list(APPEND _progs "HALF_${_u}=\"$<TARGET_FILE:half_${_p}>\"")
	endif()
endforeach()
function(interop_tests label)
	set(_n ${label}_test)
	dmodem_test_exe(${_n} interop_test.cpp)
	target_compile_definitions(${_n} PRIVATE ${_progs}
		"NETSIM=\"$<TARGET_FILE:netsim>\""
		"INTEROP_LOG_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/logs\""
		"INTEROP_LABEL=\"${label}\"")
	target_link_libraries(${_n} PRIVATE interop_driver)
	dmodem_gtest(${_n} LABELS ${label})
endfunction()
# Smart Link's original library (half_smartlink), and two open modems
interop_tests(smartlink)
interop_tests(open)
if(TARGET half_agere)
	interop_tests(agere)
endif()
# spandsp's modems (its own label: CI's "spandsp" are quick)
if(TARGET half_spandsp)
	interop_tests(spanpeer)
endif()
foreach(_l sm56 pctel hsf)
	if(TARGET half_${_l})
		interop_tests(${_l})
	endif()
endforeach()
if(TARGET half_broadcom)
	interop_tests(broadcom)
endif()
