# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# Test programs of the open data pumps, the library and the fax, and their
# ctest registrations.  Labels:
#   quick     offline, faster than real time (a few minutes in all)
#   spandsp   against spandsp's V.17 transmitter and T.4 encoder
#             (libspandsp-dev)
#   smartlink, agere, sm56, pctel, hsf, spanpeer   calls with the other
#             soft modems: Smart Link's (its original binary library),
#             the Agere, SM56, PCTel and HSF ones, spandsp's (needs -m32
#             on x86_64, Clang's or GCC's, and 32 bit libraries); long
#   open      calls between two open modems through the same driver
#   ring      netsim ringing the line, each country's way
#   adaptive  adaptive answer (+FAA=1): each modem's fax senders and data
#             callers into the open modem
#
#   ctest --test-dir build -L quick
set(S ${DMODEM_DSP_DIR})
set(_dsp ${DMODEM_DSP_SRCS})
set(_fax ${S}/fax_modems.cpp ${S}/qam.cpp ${S}/qam_tables.cpp ${S}/dsp_util.cpp
	${S}/dp_fsk.cpp)

# every test source here on the same C library (libc_compat.h): Windows'
# setenv, unsetenv and mkdir's mode, and glibc's rand() where the C
# library isn't glibc (empty with glibc)
if(MSVC)
	add_compile_options(/FI${CMAKE_CURRENT_SOURCE_DIR}/libc_compat.h)
else()
	add_compile_options(-include ${CMAKE_CURRENT_SOURCE_DIR}/libc_compat.h)
endif()

function(dmodem_test_exe name)
	add_executable(${name} ${ARGN})
	target_include_directories(${name} PRIVATE ${S} ${DMODEM_LINK_DIR}
		${DMODEM_CORE_NEW_DIR})
	dmodem_flags(${name})
	target_link_libraries(${name} PRIVATE ${DMODEM_LIBM} Threads::Threads)
endfunction()

# netsim's NET_CODEC, codec_probe and the loopback's codec: AMR and AMR-WB, a mobile network's
# speech codecs on the line (libopencore-amrnb-dev, libopencore-amrwb-dev,
# libvo-amrwbenc-dev); without the libraries netsim says so
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
	pkg_check_modules(AMR QUIET opencore-amrnb opencore-amrwb vo-amrwbenc)
endif()

# the data pumps back to back: pump_test, and dsp_loopback to try a line
# from the command line (with a speech codec on it where the AMR libraries
# are: codec_sim)
add_library(dmodem_loopback OBJECT loopback.cpp test_util.cpp codec_sim.cpp)
target_include_directories(dmodem_loopback PRIVATE ${S})
dmodem_flags(dmodem_loopback)
if(AMR_FOUND)
	target_compile_definitions(dmodem_loopback PRIVATE DMODEM_HAVE_AMR)
	target_include_directories(dmodem_loopback PRIVATE ${AMR_INCLUDE_DIRS})
	target_link_libraries(dmodem_loopback PUBLIC ${AMR_LDFLAGS})
endif()
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
# V.34 half-duplex back to back: V.8 to the control channel, its data
# both ways, the primary channel's pages (its own core stubs)
dmodem_test_exe(v34_hdx_test v34_hdx_test.cpp test_util.cpp)
target_link_libraries(v34_hdx_test PRIVATE dmodem_dsp)
dmodem_gtest(v34_hdx_test LABELS quick)
# bits.h: fields, the V.34 and frame CRCs, the HDLC FCS
dmodem_test_exe(bits_test bits_test.cpp)
dmodem_gtest(bits_test LABELS quick)
# g711.h: the laws' octets, reference values, sign and magnitude
dmodem_test_exe(g711_test g711_test.cpp)
dmodem_gtest(g711_test LABELS quick)
# +VSM's sample formats (voice_codec.h)
dmodem_test_exe(voice_codec_test voice_codec_test.cpp)
dmodem_gtest(voice_codec_test LABELS quick)
# dsp.h's DTMF detector on the generator's digits, G.711, twist, length
dmodem_test_exe(dtmf_test dtmf_test.cpp)
target_link_libraries(dtmf_test PRIVATE dmodem_loopback dmodem_dsp)
dmodem_gtest(dtmf_test LABELS quick)
# the call progress tones (ringing, busy) a voice dial listens for
dmodem_test_exe(call_tone_test call_tone_test.cpp)
target_link_libraries(call_tone_test PRIVATE dmodem_loopback dmodem_dsp)
dmodem_gtest(call_tone_test LABELS quick)
# the ring signal a modem codec hears on hook (an ALSA line's rings)
dmodem_test_exe(ring_detect_test ring_detect_test.cpp)
target_link_libraries(ring_detect_test PRIVATE dmodem_loopback dmodem_dsp)
dmodem_gtest(ring_detect_test LABELS quick)
# the countries' tones (../tones.cpp, netsim's NET_RING); the ringing through the
# ring detector to the core: a RING each ring, the auto answer
dmodem_test_exe(ring_tones_test ring_tones_test.cpp)
target_link_libraries(ring_tones_test PRIVATE dmodem_core dmodem_dsp dmodem_t30
	dmodem_link)
dmodem_gtest(ring_tones_test LABELS quick)
# the emulated hardware's mixins (hw/) on a host of the test's own
dmodem_test_exe(hw_test hw_test.cpp)
dmodem_gtest(hw_test LABELS quick)
# (netsim: the interop calls' network, on POSIX's pipes and poll)
set(_amr_users codec_probe)
if(NOT WIN32)
	dmodem_test_exe(netsim netsim.cpp codec_sim.cpp test_util.cpp
		${S}/tones.cpp
		${S}/dsp_util.cpp ${S}/qam.cpp ${S}/v34_tables.cpp)
	list(APPEND _amr_users netsim)
endif()
dmodem_test_exe(codec_probe codec_probe.cpp codec_sim.cpp)
if(AMR_FOUND)
	foreach(_t ${_amr_users})
		target_compile_definitions(${_t} PRIVATE DMODEM_HAVE_AMR)
		target_include_directories(${_t} PRIVATE ${AMR_INCLUDE_DIRS})
		target_link_libraries(${_t} PRIVATE ${AMR_LDFLAGS})
	endforeach()
	message(STATUS "tests: netsim with AMR and AMR-WB (NET_CODEC)")
else()
	message(STATUS "tests: AMR libraries not found, netsim without NET_CODEC")
endif()
# V.cvm's signal set back to back and through the codecs (codec_sim, where
# the AMR libraries are)
dmodem_test_exe(vcvm_test vcvm_test.cpp codec_sim.cpp ${S}/vcvm.cpp)
# its start-up: the capabilities, the messages and their message mode, the
# probing and the loading, and two DCEs' start-ups through the codecs
dmodem_test_exe(vcvm_startup_test vcvm_startup_test.cpp)
target_link_libraries(vcvm_startup_test PRIVATE dmodem_loopback dmodem_dsp)
# its data mode, and the Reed-Solomon code under it
dmodem_test_exe(vcvm_data_test vcvm_data_test.cpp)
target_link_libraries(vcvm_data_test PRIVATE dmodem_loopback dmodem_dsp)
dmodem_test_exe(rs_test rs_test.cpp ${S}/rs.cpp)
dmodem_gtest(rs_test LABELS quick)
# V.wb, the wideband mode: its parts, and caller and answerer on a
# simulated bare pair (vwb_line.cpp: cable, noise, interferers, the
# hybrid's echo, clocks apart, a dropout)
dmodem_test_exe(vwb_test vwb_test.cpp vwb_line.cpp ${S}/vwb.cpp ${S}/rs.cpp)
dmodem_gtest(vwb_test LABELS quick)
# and under libdmodem: AT&L1, V.42 and V.44 on top
dmodem_test_exe(vwb_call_test vwb_call_test.cpp vwb_line.cpp)
target_include_directories(vwb_call_test PRIVATE ${PROJECT_SOURCE_DIR}/dmodemd)
target_link_libraries(vwb_call_test PRIVATE dmodem)
dmodem_gtest(vwb_call_test LABELS quick)
# leased lines and dry pairs in the voice band: AT&L1 and AT&L2, the
# 4-wire pumps, V.54's loops, V.13
dmodem_test_exe(leased_call_test leased_call_test.cpp)
target_include_directories(leased_call_test PRIVATE ${PROJECT_SOURCE_DIR}/dmodemd)
target_link_libraries(leased_call_test PRIVATE dmodem)
dmodem_gtest(leased_call_test LABELS quick)
# the clear 64 kbit/s channel (AT+MS=CLEAR): two libdmodem modems on a
# simulated digital channel
dmodem_test_exe(clear_call_test clear_call_test.cpp)
target_include_directories(clear_call_test PRIVATE ${PROJECT_SOURCE_DIR}/dmodemd)
target_link_libraries(clear_call_test PRIVATE dmodem)
dmodem_gtest(clear_call_test LABELS quick)
# V.110 against libosmocore's (OSMOCORE_SRC: a libosmocore source tree,
# 1.14 or later): its src/isdn/v110.c alone, built into this test, with
# the headers its build generates made here and talloc (which v110.c
# does not use) stubbed
set(OSMOCORE_SRC "" CACHE PATH "libosmocore's source tree, for v110_osmo_test")
if(OSMOCORE_SRC AND EXISTS ${OSMOCORE_SRC}/src/isdn/v110.c)
	set(_osmo_inc ${CMAKE_CURRENT_BINARY_DIR}/osmo_inc)
	file(READ ${OSMOCORE_SRC}/include/osmocom/core/bitXXgen.h.tpl _osmo_tpl)
	foreach(_n 16 32 64)
		string(REPLACE "XX" "${_n}" _osmo_h "${_osmo_tpl}")
		file(WRITE ${_osmo_inc}/osmocom/core/bit${_n}gen.h "${_osmo_h}")
	endforeach()
	file(WRITE ${_osmo_inc}/talloc.h
		"/* (talloc: not used by v110.c) */\n"
		"typedef void TALLOC_CTX;\n"
		"#define talloc_free(p) ((void)(p), 0)\n"
		"#define talloc_strdup(ctx, s) ((void)(ctx), (char *)(s))\n")
	add_library(osmo_v110_peer STATIC ${OSMOCORE_SRC}/src/isdn/v110.c
		osmo_v110_peer.c)
	target_include_directories(osmo_v110_peer PRIVATE ${_osmo_inc}
		${OSMOCORE_SRC}/include)
	set_target_properties(osmo_v110_peer PROPERTIES C_STANDARD 11
		C_EXTENSIONS ON)
	# (libosmocore's code, its warnings not ours)
	target_compile_options(osmo_v110_peer PRIVATE -w)
	dmodem_test_exe(v110_osmo_test v110_osmo_test.cpp)
	target_include_directories(v110_osmo_test PRIVATE
		${PROJECT_SOURCE_DIR}/dmodemd)
	target_link_libraries(v110_osmo_test PRIVATE dmodem osmo_v110_peer)
	dmodem_gtest(v110_osmo_test LABELS quick osmocom)
endif()
foreach(_t vcvm_test vcvm_startup_test vcvm_data_test)
	if(AMR_FOUND)
		target_compile_definitions(${_t} PRIVATE DMODEM_HAVE_AMR)
		target_include_directories(${_t} PRIVATE ${AMR_INCLUDE_DIRS})
		target_link_libraries(${_t} PRIVATE ${AMR_LDFLAGS})
	endif()
	dmodem_gtest(${_t} LABELS quick)
endforeach()
# the fax without a core: the modulations, T.4 and TIFF, two T.30 machines
# (libtiff's tiffcp, run by the test: not the build host's from a cross
# build's program)
if(CMAKE_CROSSCOMPILING)
	set(TIFFCP "")
else()
	find_program(TIFFCP tiffcp)
endif()
# (none found: "" for the test to skip, not TIFFCP-NOTFOUND)
set(_tiffcp "${TIFFCP}")
if(NOT TIFFCP)
	set(_tiffcp "")
endif()
dmodem_test_exe(fax_test fax_test.cpp fax_docs.cpp fax_stubs.cpp test_util.cpp ${S}/t30.cpp ${S}/t38.cpp
	${S}/t4.cpp ${S}/tiff.cpp ${_fax})
target_compile_definitions(fax_test PRIVATE "TIFFCP=\"${_tiffcp}\"")
dmodem_gtest(fax_test LABELS quick)
# T.38: its packets, UDPTL, and two T.30 machines over it
dmodem_test_exe(t38_test t38_test.cpp fax_docs.cpp fax_stubs.cpp test_util.cpp
	${S}/t30.cpp ${S}/t38.cpp ${S}/t4.cpp ${S}/tiff.cpp ${_fax})
dmodem_gtest(t38_test LABELS quick)
# T.30 Annex F: two T.30 machines on V.34 half-duplex, and with one
# without it
dmodem_test_exe(t30_v34_test t30_v34_test.cpp fax_docs.cpp test_util.cpp
	${S}/t30.cpp ${S}/t38.cpp ${S}/t4.cpp ${S}/tiff.cpp)
target_link_libraries(t30_v34_test PRIVATE dmodem_dsp)
dmodem_gtest(t30_v34_test LABELS quick)
# class 1 over T.38: a DTE of its own through T.30 on the engine, our
# T.30 machine at the other end
dmodem_test_exe(class1_t38_test class1_t38_test.cpp fax_docs.cpp fax_stubs.cpp
	test_util.cpp ${S}/fax_class1.cpp ${S}/t30.cpp ${S}/t38.cpp ${S}/t4.cpp
	${S}/tiff.cpp ${_fax})
dmodem_gtest(class1_t38_test LABELS quick)
# the test document for the Asterisk and FreeSWITCH fax scripts
dmodem_test_exe(fax_doc fax_doc.cpp fax_docs.cpp ${S}/t4.cpp ${S}/tiff.cpp)
target_link_libraries(fax_doc PRIVATE GTest::gtest)
# libdmodem: pairs of its modems back to back
dmodem_test_exe(dmodem_test dmodem_test.cpp
	${PROJECT_SOURCE_DIR}/dmodemd/rtp_events.cpp)
target_include_directories(dmodem_test PRIVATE ${PROJECT_SOURCE_DIR}/dmodemd)
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
		t30_spandsp.cpp t38_spandsp.cpp fax_docs.cpp fax_stubs.cpp test_util.cpp ${S}/t30.cpp ${S}/t38.cpp
		${S}/t4.cpp ${S}/tiff.cpp ${_fax})
	target_include_directories(spandsp_test PRIVATE ${SPANDSP_INCLUDE_DIRS})
	target_link_libraries(spandsp_test PRIVATE ${SPANDSP_LDFLAGS})
	dmodem_gtest(spandsp_test LABELS spandsp)
else()
	message(STATUS "tests: spandsp not found, its tests skipped")
endif()

# --- the interop calls: the open data pumps on the modem core
#     (half_open, half_open8 at 8000 Hz; 32 bit like the library) and
#     Smart Link's original library on the same core in their place
#     (half_smartlink), and the other modems below (classes on
#     modem_half.cpp), through interop_test.cpp (interop_call.cpp), with
#     netsim for the G.711 network.
#     The original library (smartlink/dsplibs.o, Smart Link's binary-only
#     DSP library) is a 32 bit i386 object: -m32 on x86_64.
set(DMODEM_M32 "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
	set(DMODEM_M32 -m32)
endif()
# (our modem in the 32 bit programs, half_open and the like: its maths in
# SSE, as the 64 bit dmodemd's, not in x87's wider registers, whose
# rounding differs from compiler to compiler: Clang's and GCC's x87 code
# make different calls of it with a marginal peer)
set(DMODEM_M32_FP -msse2 -mfpmath=sse)
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "${DMODEM_M32}")
set(CMAKE_REQUIRED_LINK_OPTIONS "${DMODEM_M32}")
check_cxx_source_compiles("int main() { return 0; }" DMODEM_HAVE_M32)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(WIN32)
	# (half modems on POSIX's ptys, pipes and processes)
	message(STATUS "tests: Windows, interop tests skipped")
	return()
endif()
if(NOT DMODEM_HAVE_M32 OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i.86)$")
	message(STATUS "tests: no 32 bit i386 compiler, interop tests skipped")
	return()
endif()
foreach(_n half_open half_open8 half_smartlink)
	set(_rate 9600)
	set(_l "")
	if(_n STREQUAL "half_open8")
		set(_rate 8000)
	endif()
	if(_n STREQUAL "half_smartlink")
		# the library in place of the open data pumps; its runtime and
		# its V.92 pump come through smartlink_glue.cpp first (and
		# the ring detector of the line on hook, dsp_util.cpp's)
		set(_pumps smartlink_glue.cpp ${S}/dsp_util.cpp ${S}/tones.cpp
			${CMAKE_CURRENT_SOURCE_DIR}/smartlink/dsplibs.o)
		set(_l -no-pie -Wl,-z,noexecstack -Wl,--wrap=dp_runtime_create
			-Wl,--wrap=modem_dp_register)
	else()
		set(_pumps ${_dsp})
	endif()
	add_executable(${_n} half_open.cpp ${DMODEM_FRONT_SRCS}
		${DMODEM_CORE_NEW_SRCS} ${DMODEM_LINK_SRCS} ${_pumps})
	target_include_directories(${_n} PRIVATE ${DMODEM_FRONT_DIR} ${S}
		${DMODEM_LINK_DIR} ${DMODEM_CORE_NEW_DIR})
	target_compile_definitions(${_n} PRIVATE CONFIG_DEBUG_MODEM
		MODEM_RATE=${_rate})
	target_compile_options(${_n} PRIVATE ${DMODEM_M32} ${DMODEM_M32_FP}
		-w -g -O)
	target_link_options(${_n} PRIVATE ${DMODEM_M32} ${_l})
	target_link_libraries(${_n} PRIVATE ${DMODEM_LIBM} Threads::Threads)
endforeach()

# the open modem on an emulated HD-audio modem codec (half_hda.cpp): an
# Intel HD Audio controller (hw/hda.hpp) and a Silicon Labs Si3054
# (hw/si3054_hda.hpp), driven as Linux's snd-hda-codec-si3054 drives it
add_executable(half_hda half_hda.cpp modem_half.cpp ${DMODEM_FRONT_SRCS}
	${DMODEM_CORE_NEW_SRCS} ${DMODEM_LINK_SRCS} ${_dsp})
target_include_directories(half_hda PRIVATE ${DMODEM_FRONT_DIR} ${S}
	${DMODEM_LINK_DIR} ${DMODEM_CORE_NEW_DIR})
target_compile_definitions(half_hda PRIVATE CONFIG_DEBUG_MODEM)
target_compile_options(half_hda PRIVATE ${DMODEM_M32} ${DMODEM_M32_FP} -g -O)
target_link_options(half_hda PRIVATE ${DMODEM_M32})
target_link_libraries(half_hda PRIVATE ${DMODEM_LIBM} Threads::Threads)

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
	target_link_libraries(half_agere PRIVATE ${DMODEM_LIBM})
	message(STATUS "tests: the Agere soft modem: ${DMODEM_AGERE_LIB}")
endif()

# spandsp's data modems as a peer (half_spandsp.cpp): its V.21, V.23,
# Bell 103 and V.22/V.22bis pumps (libspandsp 0.0.6, as Asterisk and
# FreeSWITCH have it) in a modem of their own
if(SPANDSP_FOUND)
	add_executable(half_spandsp half_spandsp.cpp modem_half.cpp)
	target_include_directories(half_spandsp PRIVATE ${SPANDSP_INCLUDE_DIRS})
	target_link_libraries(half_spandsp PRIVATE ${SPANDSP_LDFLAGS} ${DMODEM_LIBM})
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
	target_link_libraries(half_sm56 PRIVATE ${DMODEM_LIBM})
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
	target_link_libraries(half_pctel PRIVATE ${DMODEM_LIBM})
	message(STATUS "tests: the PCTel HSP soft modem: ${DMODEM_PCTEL_DIR}")
endif()

# Conexant's HSF soft modem as a peer (half_hsf.cpp): hsfmodem-7.80.02.06oem's
# engine (hsfengine), its AC'97 ICH codec module (hsfmc97ich) and its
# soft DAA module (hsfsoar), their sections renamed per object (and the
# ICH module's microsecond busy wait, hsfmc97ich41_, and its empty print
# function for the DAA module, hsfmc97ich57_, half_hsf.cpp's), and the
# modem's .inf and country file for its NVM
set(DMODEM_HSF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/hsf" CACHE
	PATH "objects and NVM files of the HSF soft modem (interop tests)")
if(EXISTS "${DMODEM_HSF_DIR}/hsfengine.o")
	set(_hsfo "")
	set(_weak_hsfmc97ich hsfmc97ich41_,hsfmc97ich57_)
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
	target_link_libraries(half_hsf PRIVATE ${DMODEM_LIBM})
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
	target_link_libraries(half_broadcom PRIVATE ${DMODEM_LIBM})
	message(STATUS "tests: the Broadcom soft modem: ${DMODEM_BROADCOM_DIR}")
endif()

# ESS's V.92 soft modem for Windows as a peer (half_ess.cpp): its WDM
# driver (es56hpi.sys 5.43.070, a PE image) and its INF, on
# interop_windows.cpp as Broadcom's
set(DMODEM_ESS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ess" CACHE
	PATH "es56hpi.sys and mdmzn.inf of the ESS soft modem (interop tests)")
if(EXISTS "${DMODEM_ESS_DIR}/es56hpi.sys")
	add_executable(half_ess half_ess.cpp interop_windows.cpp modem_half.cpp)
	target_compile_definitions(half_ess PRIVATE
		ESS_SYS="${DMODEM_ESS_DIR}/es56hpi.sys"
		ESS_INF="${DMODEM_ESS_DIR}/mdmzn.inf")
	target_compile_options(half_ess PRIVATE ${DMODEM_M32} -g -O1)
	target_link_options(half_ess PRIVATE ${DMODEM_M32} -no-pie
		-Wl,-z,noexecstack)
	target_link_libraries(half_ess PRIVATE ${DMODEM_LIBM})
	message(STATUS "tests: the ESS soft modem: ${DMODEM_ESS_DIR}")
endif()

# Intel's 537EP soft modem as a peer (half_intel.cpp): the core of Intel's
# Intel537EP 2.37.50.1 driver for Fedora Core 1 (537core.lib) with its
# country tables (wwh_dflt.c); its own exit, index and pow renamed, and
# IPP's CPU type and cycle counter made weak (half_intel.cpp's)
set(DMODEM_INTEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/intel" CACHE
	PATH "537core.lib and wwh_dflt.c of the Intel 537EP soft modem (interop tests)")
if(EXISTS "${DMODEM_INTEL_DIR}/537core.lib")
	set(_io ${CMAKE_CURRENT_BINARY_DIR}/537core_r.o)
	add_custom_command(OUTPUT ${_io}
		COMMAND ${CMAKE_OBJCOPY} --redefine-sym exit=intel_core_exit
			--redefine-sym index=intel_core_index
			--redefine-sym pow=intel_core_pow
			--weaken-symbol=ippCoreGetCpuType
			--weaken-symbol=ipp_get_pentium_counter
			${DMODEM_INTEL_DIR}/537core.lib ${_io}
		DEPENDS ${DMODEM_INTEL_DIR}/537core.lib
		COMMENT "537core.lib's symbols renamed")
	add_executable(half_intel half_intel.cpp modem_half.cpp
		${DMODEM_INTEL_DIR}/wwh_dflt.c ${_io})
	set_source_files_properties(${_io} PROPERTIES EXTERNAL_OBJECT TRUE
		GENERATED TRUE)
	set_source_files_properties(${DMODEM_INTEL_DIR}/wwh_dflt.c PROPERTIES
		COMPILE_OPTIONS "-w")
	target_compile_options(half_intel PRIVATE ${DMODEM_M32} -g -O1)
	target_link_options(half_intel PRIVATE ${DMODEM_M32} -no-pie
		-Wl,-z,noexecstack)
	target_link_libraries(half_intel PRIVATE ${DMODEM_LIBM})
	message(STATUS "tests: the Intel 537EP soft modem: ${DMODEM_INTEL_DIR}")
endif()

# the interop tests: calls between the open modem and each of the modems
# above (interop_test.cpp, through interop_call.cpp: what a call does is in
# its environment, NET for netsim's G.711 network, STREAM, EXPECT_B, ...).
# One test program a label; each call writes its modem logs to its
# test's directory of the run's test logs (test-logs/<run>/<test>/), or
# run by hand, logs/<test>/.  The calls run as fast as they can (fax: ten times real
# time), on the line's clock, a little nicer (each takes a processor while
# it runs, but for the moments on hook in real time).  So ctest -j can be
# some four times the processors (-j16 on 4).  interop_call makes one by
# hand.
add_library(interop_driver STATIC interop_call.cpp)
dmodem_flags(interop_driver)
target_link_libraries(interop_driver PUBLIC Threads::Threads)
dmodem_test_exe(interop_call interop_call_main.cpp)
target_link_libraries(interop_call PRIVATE interop_driver)
set(_progs "HALF_SMARTLINK=\"$<TARGET_FILE:half_smartlink>\""
	"HALF_OPEN=\"$<TARGET_FILE:half_open>\""
	"HALF_OPEN8=\"$<TARGET_FILE:half_open8>\"")
foreach(_p agere sm56 spandsp pctel hsf broadcom intel ess hda)
	if(TARGET half_${_p})
		string(TOUPPER ${_p} _u)
		list(APPEND _progs "HALF_${_u}=\"$<TARGET_FILE:half_${_p}>\"")
	endif()
endforeach()
function(interop_tests label)
	set(_n ${label}_test)
	dmodem_test_exe(${_n} interop_test.cpp ${S}/tones.cpp)
	target_compile_definitions(${_n} PRIVATE ${_progs}
		"NETSIM=\"$<TARGET_FILE:netsim>\""
		"INTEROP_LOG_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/logs\""
		"INTEROP_LABEL=\"${label}\"")
	target_link_libraries(${_n} PRIVATE interop_driver)
	dmodem_gtest(${_n} LABELS ${label})
endfunction()
# Smart Link's original library (half_smartlink), and two open modems
# (through the AMR codecs too, when netsim has them)
interop_tests(smartlink)
interop_tests(open)
if(AMR_FOUND)
	target_compile_definitions(open_test PRIVATE INTEROP_HAVE_AMR)
endif()
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
if(TARGET half_intel)
	interop_tests(intel)
endif()
if(TARGET half_ess)
	interop_tests(ess)
endif()
# the open modem on the emulated HD-audio modem codec (half_hda)
interop_tests(hda)
# netsim ringing the line (NET_RING) as each country's rings
interop_tests(ring)
# adaptive answer (+FAA=1): fax senders and data callers of each modem
# into the open modem, which finds out which the call is
interop_tests(adaptive)
