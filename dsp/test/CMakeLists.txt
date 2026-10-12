# Test programs of the open data pumps, the library and the fax, and their
# ctest registrations.  Labels:
#   quick     offline, faster than real time (a few minutes in all)
#   spandsp   against spandsp's V.17 transmitter and T.4 encoder
#             (libspandsp-dev)
#   interop   with the original binary library, in real time (needs
#             gcc -m32 on x86_64); long
#
#   ctest --test-dir build -L quick
set(S ${DMODEM_DSP_DIR})
set(_dsp ${DMODEM_DSP_SRCS})
# slmodemd's old C core (legacy/): Smart Link's original library calls
# into it (half_blob), and core_script_old compares the new core with it
set(DMODEM_LEGACY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/legacy)
set(DMODEM_LEGACY_SRCS modem.cpp modem_datafile.cpp modem_at.cpp
	modem_timer.cpp modem_param.cpp modem_debug.cpp sysdep_common.cpp)
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
add_library(dmodem_loopback OBJECT loopback.cpp)
target_include_directories(dmodem_loopback PRIVATE ${S})
target_compile_options(dmodem_loopback PRIVATE -Wall -O2 -g)
dmodem_test_exe(dsp_loopback dsp_loopback.cpp)
target_link_libraries(dsp_loopback PRIVATE dmodem_loopback dmodem_dsp)
dmodem_test_exe(pump_test pump_test.cpp)
target_link_libraries(pump_test PRIVATE dmodem_loopback dmodem_dsp)
dmodem_gtest(pump_test LABELS quick)
dmodem_test_exe(netsim netsim.cpp ${S}/dsp_util.cpp ${S}/qam.cpp
	${S}/v34_tables.cpp)
# the fax without a core: the modulations, T.4 and TIFF, two T.30 machines
find_program(TIFFCP tiffcp)
dmodem_test_exe(fax_test fax_test.cpp fax_docs.cpp fax_stubs.cpp ${S}/t30.cpp
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
		t30_spandsp.cpp fax_docs.cpp fax_stubs.cpp ${S}/t30.cpp
		${S}/t4.cpp ${S}/tiff.cpp ${_fax})
	target_include_directories(spandsp_test PRIVATE ${SPANDSP_INCLUDE_DIRS})
	target_link_libraries(spandsp_test PRIVATE ${SPANDSP_LDFLAGS})
	dmodem_gtest(spandsp_test LABELS spandsp)
else()
	message(STATUS "tests: spandsp not found, its tests skipped")
endif()

# --- interop: modem_half with the original library and with the open
#     data pumps (32 bit like the blob), in real time through
#     interop.py / fax_interop.py, with netsim for the G.711 network.
#     The original library (dsplibs.o here, Smart Link's binary-only
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
	foreach(_kind blob open)
		set(_n half_${_kind})
		if(_rate EQUAL 8000)
			set(_n ${_n}8)
		endif()
		if(_kind STREQUAL "blob")
			# the original library calls into slmodemd's old core
			add_executable(${_n} ${C}/modem_half.cpp
				${C}/modem_cmdline.cpp ${DMODEM_LEGACY_SRCS}
				${DMODEM_LINK_SRCS}
				${CMAKE_CURRENT_SOURCE_DIR}/dsplibs.o)
			target_include_directories(${_n} PRIVATE ${C} ${S}
				${DMODEM_LINK_DIR} ${DMODEM_CORE_NEW_DIR})
			set(_l -no-pie -Wl,-z,noexecstack)
		else()
			# the open data pumps on the modem core
			add_executable(${_n} modem_half.cpp ${DMODEM_FRONT_SRCS}
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

# the Agere Systems soft modem as another peer (half_agere.c): its library,
# agrsm_core.o of agrsm-20070804
set(DMODEM_AGERE_LIB "${CMAKE_CURRENT_SOURCE_DIR}/agrsm_core.o" CACHE
	FILEPATH "agrsm_core.o of the Agere soft modem (interop tests)")
if(EXISTS "${DMODEM_AGERE_LIB}")
	add_executable(half_agere half_agere.c ${DMODEM_AGERE_LIB})
	target_compile_options(half_agere PRIVATE ${DMODEM_M32} -g -O1)
	target_link_options(half_agere PRIVATE ${DMODEM_M32} -no-pie
		-Wl,-z,noexecstack)
	target_link_libraries(half_agere PRIVATE m)
	message(STATUS "tests: the Agere soft modem: ${DMODEM_AGERE_LIB}")
endif()

# spandsp's data modems as a peer (half_spandsp.c): its V.21, V.23,
# Bell 103 and V.22/V.22bis pumps (libspandsp 0.0.6, as Asterisk and
# FreeSWITCH have it) in a modem of their own
if(SPANDSP_FOUND)
	add_executable(half_spandsp half_spandsp.c)
	target_include_directories(half_spandsp PRIVATE ${SPANDSP_INCLUDE_DIRS})
	target_link_libraries(half_spandsp PRIVATE ${SPANDSP_LDFLAGS} m)
endif()

# Motorola's SM56 soft modem as a peer (half_sm56.c): sm56.ko of
# sm56-06.09.05 for Fedora Core 3's kernel 2.6.9, its imports renamed
set(DMODEM_SM56_KO "${CMAKE_CURRENT_SOURCE_DIR}/sm56.ko" CACHE
	FILEPATH "sm56.ko of the Motorola SM56 soft modem (interop tests)")
if(EXISTS "${DMODEM_SM56_KO}")
	set(_sm56o ${CMAKE_CURRENT_BINARY_DIR}/sm56k.o)
	add_custom_command(OUTPUT ${_sm56o}
		COMMAND ${CMAKE_COMMAND} -DKO=${DMODEM_SM56_KO} -DOUT=${_sm56o}
			-DNM=${CMAKE_NM} -DOBJCOPY=${CMAKE_OBJCOPY}
			-P ${CMAKE_CURRENT_SOURCE_DIR}/sm56_obj.cmake
		DEPENDS ${DMODEM_SM56_KO} sm56_obj.cmake
		COMMENT "sm56.ko's imports renamed")
	add_executable(half_sm56 half_sm56.c ${_sm56o})
	set_source_files_properties(${_sm56o} PROPERTIES EXTERNAL_OBJECT TRUE
		GENERATED TRUE)
	target_compile_options(half_sm56 PRIVATE ${DMODEM_M32} -g -O1)
	target_link_options(half_sm56 PRIVATE ${DMODEM_M32} -no-pie
		-Wl,-z,noexecstack)
	target_link_libraries(half_sm56 PRIVATE m)
	message(STATUS "tests: the Motorola SM56 soft modem: ${DMODEM_SM56_KO}")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_FOUND)
	return()
endif()
set(_env "NETSIM=$<TARGET_FILE:netsim>;MODEM_REALTIME=1")
set(_py ${CMAKE_CURRENT_SOURCE_DIR})
foreach(_ms 34 132 122 22 212 103 23)
	add_test(NAME interop_${_ms}_blob_calls COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open>
		"ATX3;AT+MS=${_ms}")
	add_test(NAME interop_${_ms}_open_calls COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_open> $<TARGET_FILE:half_blob>
		"ATX3;AT+MS=${_ms}")
	set_tests_properties(interop_${_ms}_blob_calls interop_${_ms}_open_calls
		PROPERTIES LABELS interop ENVIRONMENT "${_env}")
endforeach()
# V.34 through a line hit (300 ms of silence both ways) in the middle of
# the data: both modems retrain, V.42 carries on
add_test(NAME interop_34_hit_blob_calls COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open>
	"ATX3;AT+MS=34")
add_test(NAME interop_34_hit_open_calls COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_open> $<TARGET_FILE:half_blob>
	"ATX3;AT+MS=34")
set_tests_properties(interop_34_hit_blob_calls interop_34_hit_open_calls
	PROPERTIES LABELS interop ENVIRONMENT
	"${_env};NET=9600:9600:ulaw;STREAM=25;NET_DROP=20:300")
# V.34 rate renegotiation: the open modem starts one (and later goes back
# up), the original library answers it; and two open modems on a noisy
# line that gets 3 dB worse 10 s after CONNECT (NET_SYNC), each lowers
# its receiving rate
add_test(NAME interop_34_reneg_blob_calls COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open>
	"ATX3;AT+MS=34")
add_test(NAME interop_34_reneg_open_calls COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_open> $<TARGET_FILE:half_blob>
	"ATX3;AT+MS=34")
set_tests_properties(interop_34_reneg_blob_calls interop_34_reneg_open_calls
	PROPERTIES LABELS interop ENVIRONMENT
	"${_env};NET=9600:9600:ulaw;NET_SYNC=1;STREAM=25;DSP_V34_RENEG=5000:8")
add_test(NAME interop_34_noise COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_open> $<TARGET_FILE:half_open>
	"ATX3;AT+MS=34")
set_tests_properties(interop_34_noise PROPERTIES LABELS interop ENVIRONMENT
	"${_env};NET=9600:9600:ulaw;NET_SYNC=1;STREAM=30;NET_FLOOR=250;NET_NOISE=10:250")
# V.90: the original library (analogue modem) calls the open one (digital
# modem, 8000 Hz) through the simulated G.711 network.  NET_FLOOR: on a
# line without any noise its phase 4 check (the error before and after
# TRN2d corrects its levels from the DIL by a unit or two) fails, and it
# retrains once for nothing; a floor of -81 dBFS keeps the check to what
# it is for on a real line
add_test(NAME interop_v90_ulaw COMMAND ${Python3_EXECUTABLE} ${_py}/interop.py
	$<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8> "ATX3;AT+MS=90")
set_tests_properties(interop_v90_ulaw PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;NET_FLOOR=3")
add_test(NAME interop_v90_alaw COMMAND ${Python3_EXECUTABLE} ${_py}/interop.py
	$<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8> "ATX3;AT+MS=90")
set_tests_properties(interop_v90_alaw PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:alaw;DSP_G711=alaw")
# V.90 rate renegotiation (9.6/V.90): ours after 5 s of data, to 24 000
# bit/s upstream; the library's own when noise on the PCM codes from 40 s
# on worsens the downstream: it asks for a silent period (CPs), and the
# noise stops with our silence (else it may pick 56000 again and the data
# goes wrong)
add_test(NAME interop_v90_reneg COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
	"ATX3;AT+MS=90")
set_tests_properties(interop_v90_reneg PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;NET_FLOOR=3;STREAM=15;DSP_V34_RENEG=5000:10;EXPECT_B=V.90 renegotiated [0-9]+/24000")
add_test(NAME interop_v90_reneg_silence COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
	"ATX3;AT+MS=90")
set_tests_properties(interop_v90_reneg_silence PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;NET_FLOOR=3;STREAM=50;NET_NOISE=40:20:10;NET_NOISE_DIR=bacodes;NET_NOISE_QUIET=50;EXPECT_B=(?s)CPs'.*V.90 renegotiated")
# V.92 quick connect (short phase 1): the original library, a V.92
# analogue modem, starts with QC1a; then V.90
add_test(NAME interop_v92_qc COMMAND ${Python3_EXECUTABLE} ${_py}/interop.py
	$<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8> "ATX3;AT+MS=92")
set_tests_properties(interop_v92_qc PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;EXPECT_B=V.92 quick connect")
# V.92 PCM upstream: the original library is a "V.92
# Lite" build, which retrains when INFO1d offers PCM upstream; BLOB_V92FULL
# (legacy/modem.cpp) sets its full V.92 bit, and it then goes through
# phases 3 and 4 of V.92 with us.  Lite: we answer its retrain without the
# offer, V.90.  A rate the line cannot carry (44 000): B1u does not come
# through, we retrain to V.90.  With the prefilter we design (6.4.2/V.92)
# the rate is up to 38 666 (mu-law) and 40 000 (A-law); without it
# (DSP_V92_PREFILTER=0), the precoder alone, 34 000 to 36 000.
foreach(_law ulaw alaw)
	add_test(NAME interop_v92_pcmup_${_law} COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
		"ATX3;AT+MS=92")
	set_tests_properties(interop_v92_pcmup_${_law} PROPERTIES LABELS interop
		ENVIRONMENT "${_env};NET=9600:8000:${_law};STREAM=10;BLOB_V92FULL=1;EXPECT_B=(?s)prefilter of 64 taps.*V.92 connect 56000/(3[89]|4[0-2])")
endforeach()
add_test(NAME interop_v92_pcmup_noprefilter COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
	"ATX3;AT+MS=92")
set_tests_properties(interop_v92_pcmup_noprefilter PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;STREAM=10;BLOB_V92FULL=1;DSP_V92_PREFILTER=0;EXPECT_B=V.92 connect 56000/3[4-9]")
add_test(NAME interop_v92_pcmup_calls COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
	"ATX3;AT+MS=92")
# (quick connect's timing does not take a busy machine: alone)
set_tests_properties(interop_v92_pcmup_calls PROPERTIES LABELS interop
	RUN_SERIAL TRUE ENVIRONMENT "${_env};NET=9600:8000:ulaw;CALLS=2;STREAM=3;BLOB_V92FULL=1;EXPECT_B=(?s)V.92 connect.*V.92 connect")
add_test(NAME interop_v92_pcmup_lite COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
	"ATX3;AT+MS=92")
set_tests_properties(interop_v92_pcmup_lite PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;STREAM=3;EXPECT_B=(?s)no PCM upstream offer.*V.90 connect")
add_test(NAME interop_v92_pcmup_fallback COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
	"ATX3;AT+MS=92")
set_tests_properties(interop_v92_pcmup_fallback PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;STREAM=3;BLOB_V92FULL=1;DSP_V92_UP_DRN=16;EXPECT_B=(?s)B1u does not come through.*V.90 connect")
# rate renegotiation (9.8/V.92): ours after 3 s of data, to 30 666 bit/s
# upstream (DSP_V92_RENEG=<ms>:<drn>); and on its own when noise on the
# way up eats the margin of our rate
add_test(NAME interop_v92_reneg COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
	"ATX3;AT+MS=92")
set_tests_properties(interop_v92_reneg PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;STREAM=10;BLOB_V92FULL=1;DSP_V92_RENEG=3000:6;EXPECT_B=V.92 renegotiated 56000/30666")
add_test(NAME interop_v92_reneg_noise COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
	"ATX3;AT+MS=92")
set_tests_properties(interop_v92_reneg_noise PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;STREAM=15;BLOB_V92FULL=1;NET_NOISE=26:60;NET_NOISE_DIR=ab;EXPECT_B=V.92 renegotiated 56000/[23]")
# fast parameter exchange (9.9/V.92): ours after 3 s of data, to 30 666
# bit/s upstream (DSP_V92_FPE=<ms>:<drn>), in the data mode modulation.
# Without the prefilter (below with it)
add_test(NAME interop_v92_fpe COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
	"ATX3;AT+MS=92")
set_tests_properties(interop_v92_fpe PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;STREAM=10;BLOB_V92FULL=1;DSP_V92_PREFILTER=0;DSP_V92_FPE=3000:6;EXPECT_B=(?s)fast parameter exchange 1 done.*V.92 renegotiated 56000/30666")
# modem-on-hold asked for by us (AT+PMHR, 9.10.2.1/V.92) in the middle of
# PCM upstream data: the original library answers our tone B with a
# retrain, which we follow, and the data goes on
add_test(NAME interop_v92_moh_request COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
	"ATX3;AT+MS=92")
set_tests_properties(interop_v92_moh_request PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;STREAM=30;BLOB_V92FULL=1;MIDCALL_B=8:AT+PMHR\;WAIT 6;EXPECT_B=(?s)modem-on-hold request.*retrains instead.*V.92 retrained")
# and with an analogue modem that answers it (netsim's NET_MOH stands in
# for one from 22 s on: the original library does not): MHack, the remote
# on hold for 5 s, ANSam; MHnack, MHfrr and ANSam. The original library
# is cut off meanwhile and hangs up, so nothing comes back to ANSam and we
# disconnect after T1 (10 s)
foreach(_m ack nack)
	add_test(NAME interop_v92_moh_request_${_m} COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
		"ATX3;AT+MS=92")
endforeach()
set_tests_properties(interop_v92_moh_request_ack PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;STREAM=40;BLOB_V92FULL=1;NET_MOH=ack:1@22;MIDCALL_B=12:AT+PMHR=5\;WAIT 8;EXPECT_DROP=1;EXPECT_B=(?s)MHack, T1 10 s: the remote on hold for 5 s.*on hold, ANSam for 10 s.*on hold for T1: disconnect")
set_tests_properties(interop_v92_moh_request_nack PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;STREAM=35;BLOB_V92FULL=1;NET_MOH=nack@22;MIDCALL_B=12:AT+PMHR\;WAIT 3;EXPECT_DROP=1;EXPECT_B=(?s)MHnack: MHfrr.*on hold \\(reconnect\\), ANSam.*on hold for T1: disconnect")
# and with the prefilter: not down (the original library sends SUVu' to
# FB1u on the new points with the old moduli), so at the rate it has; the
# library's sequence through to B1u, and the data goes on
add_test(NAME interop_v92_fpe_prefilter COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
	"ATX3;AT+MS=92")
set_tests_properties(interop_v92_fpe_prefilter PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;STREAM=10;BLOB_V92FULL=1;DSP_V92_FPE=3000:6;EXPECT_B=(?s)prefilter not down.*SUVu'.*fast parameter exchange 1 done.*V.92 renegotiated 56000/3[89]")
# without the offer (DSP_V92_PCMUP=0): V.90, V.34 upstream
add_test(NAME interop_v92_nopcmup COMMAND ${Python3_EXECUTABLE}
	${_py}/interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8>
	"ATX3;AT+MS=92")
set_tests_properties(interop_v92_nopcmup PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw;STREAM=3;BLOB_V92FULL=1;DSP_V92_PCMUP=0;EXPECT_B=V.90 connect")
# class 1 fax calls both ways
foreach(_m 24 48 72 96 73 97 121 122 145)
	add_test(NAME fax_interop_${_m}_blob_sends COMMAND ${Python3_EXECUTABLE}
		${_py}/fax_interop.py $<TARGET_FILE:half_blob> $<TARGET_FILE:half_open> ${_m})
	add_test(NAME fax_interop_${_m}_open_sends COMMAND ${Python3_EXECUTABLE}
		${_py}/fax_interop.py $<TARGET_FILE:half_open> $<TARGET_FILE:half_blob> ${_m})
	set_tests_properties(fax_interop_${_m}_blob_sends fax_interop_${_m}_open_sends
		PROPERTIES LABELS interop ENVIRONMENT "${_env}")
endforeach()
# the Agere soft modem (8000 Hz) and the open modem (9600 Hz) call each
# other; AT+MS takes names there (B103, V21, ..., V34)
if(TARGET half_agere)
	foreach(_p B103:103 V21:21 V23C:23 B212:212 V22:22 V22B:122 V32:32
		   V32B:132 V34:34)
		string(REPLACE ":" ";" _p ${_p})
		list(GET _p 0 _a)
		list(GET _p 1 _ms)
		add_test(NAME agere_${_ms}_agere_calls COMMAND
			${Python3_EXECUTABLE} ${_py}/interop.py
			$<TARGET_FILE:half_agere> $<TARGET_FILE:half_open>
			"ATX3;AT+MS=${_a}")
		set_tests_properties(agere_${_ms}_agere_calls PROPERTIES
			LABELS agere ENVIRONMENT
			"${_env};NET=8000:9600:ulaw;PROTO_B=ATX3\;AT+MS=${_ms}")
		add_test(NAME agere_${_ms}_open_calls COMMAND
			${Python3_EXECUTABLE} ${_py}/interop.py
			$<TARGET_FILE:half_open> $<TARGET_FILE:half_agere>
			"ATX3;AT+MS=${_ms}")
		set_tests_properties(agere_${_ms}_open_calls PROPERTIES
			LABELS agere ENVIRONMENT
			"${_env};NET=9600:8000:ulaw;PROTO_B=ATX3\;AT+MS=${_a}")
	endforeach()
	# V.44 both ways (the Agere modem offers it): 60000 octets each
	# way of text, binary, repeats and noise, which it sends in
	# transparent mode now and then
	add_test(NAME agere_v44_agere_calls COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_agere>
		$<TARGET_FILE:half_open> "ATX3;AT+MS=V34")
	set_tests_properties(agere_v44_agere_calls PROPERTIES LABELS agere
		ENVIRONMENT
		"${_env};NET=8000:9600:ulaw;PROTO_B=ATX3\;AT+MS=34;BULK=60000")
	add_test(NAME agere_v44_open_calls COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_open>
		$<TARGET_FILE:half_agere> "ATX3;AT+MS=34")
	set_tests_properties(agere_v44_open_calls PROPERTIES LABELS agere
		ENVIRONMENT
		"${_env};NET=9600:8000:ulaw;PROTO_B=ATX3\;AT+MS=V34;BULK=60000")
	# V.34 rate renegotiation started by the open modem, both ways;
	# and the line much noisier 10 s after CONNECT: a retrain
	add_test(NAME agere_34_reneg_agere_calls COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_agere>
		$<TARGET_FILE:half_open> "ATX3;AT+MS=V34")
	set_tests_properties(agere_34_reneg_agere_calls PROPERTIES LABELS agere
		ENVIRONMENT
		"${_env};NET=8000:9600:ulaw;NET_SYNC=1;STREAM=30;DSP_V34_RENEG=5000:8;PROTO_B=ATX3\;AT+MS=34")
	add_test(NAME agere_34_reneg_open_calls COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_open>
		$<TARGET_FILE:half_agere> "ATX3;AT+MS=34")
	set_tests_properties(agere_34_reneg_open_calls PROPERTIES LABELS agere
		ENVIRONMENT
		"${_env};NET=9600:8000:ulaw;NET_SYNC=1;STREAM=30;DSP_V34_RENEG=5000:8;PROTO_B=ATX3\;AT+MS=V34")
	add_test(NAME agere_34_noise COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_agere>
		$<TARGET_FILE:half_open> "ATX3;AT+MS=V34")
	set_tests_properties(agere_34_noise PROPERTIES LABELS agere
		ENVIRONMENT
		"${_env};NET=8000:9600:ulaw;NET_SYNC=1;STREAM=40;NET_NOISE=10:300;PROTO_B=ATX3\;AT+MS=34")
	# V.92 quick connect: the Agere modem (+PQC=0) remembers the line of
	# the first call and starts the second with QC1a, and short phase 2.
	# NET_LOOP: our PCM signal reaches it through an analogue loop (on an
	# ideal line, the same samples as ours, its V.90 training fails at
	# some delays, every 48 samples); not half a sample: then our sample
	# instants fall exactly between its samples, and at some alignments
	# (AGERE_SYNC_PHASE 96, 288) it takes 33333 bit/s downstream for
	# 53333
	add_test(NAME agere_v92_qc COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_agere>
		$<TARGET_FILE:half_open8> "ATX3;AT+MS=V92;AT+PQC=0")
	set_tests_properties(agere_v92_qc PROPERTIES LABELS agere
		RUN_SERIAL TRUE ENVIRONMENT
		"${_env};NET_LOOP=0.45;NET=8000:8000:ulaw;DSP_G711=ulaw;CALLS=2;DIAL=5551234;STREAM=3;EXPECT_B=(?s)V.92 quick connect.*V.92 short phase 2;PROTO_B=ATX3\;AT+MS=92")
	# V.90: the Agere modem as the analogue modem, the open one as the
	# digital modem (both at 8000 Hz)
	foreach(_law ulaw alaw)
		add_test(NAME agere_v90_${_law} COMMAND ${Python3_EXECUTABLE}
			${_py}/interop.py $<TARGET_FILE:half_agere>
			$<TARGET_FILE:half_open8> "ATX3;AT+MS=V90")
		set_tests_properties(agere_v90_${_law} PROPERTIES LABELS agere
			ENVIRONMENT
			"${_env};NET_LOOP=0.45;NET=8000:8000:${_law};DSP_G711=${_law};PROTO_B=ATX3\;AT+MS=90")
	endforeach()
	# V.92 modem-on-hold: in the middle of the data the Agere modem asks
	# for it (AT+PMHR), we grant 1 min (MHack), it comes back after 10 s
	# (ATO): V.8, phase 2 to 4, and the data goes on (V.42 knows nothing
	# of it); denied (MHnack), it asks for a fast reconnect (MHfrr)
	foreach(_m grant deny)
		add_test(NAME agere_v92_moh_${_m} COMMAND ${Python3_EXECUTABLE}
			${_py}/interop.py $<TARGET_FILE:half_agere>
			$<TARGET_FILE:half_open8> "ATX3;AT+MS=V92")
	endforeach()
	set_tests_properties(agere_v92_moh_grant PROPERTIES LABELS agere
		ENVIRONMENT
		"${_env};NET_LOOP=0.45;NET=8000:8000:ulaw;DSP_G711=ulaw;STREAM=50;MIDCALL_A=8:AT+PMHR\;WAIT 10;EXPECT_B=(?s)MHack.*V.90 back from hold;PROTO_B=ATX3\;AT+MS=92")
	set_tests_properties(agere_v92_moh_deny PROPERTIES LABELS agere
		ENVIRONMENT
		"${_env};NET_LOOP=0.45;NET=8000:8000:ulaw;DSP_G711=ulaw;STREAM=40;DSP_V92_MOH=0;MIDCALL_A=8:AT+PMHR\;WAIT 5;EXPECT_B=(?s)MHnack.*MHfrr.*V.90 back from hold;PROTO_B=ATX3\;AT+MS=92")
	# and ours (AT+PMHR on our side): the Agere modem takes our tone B
	# for a retrain and reverses its tone A; we follow, and the data
	# goes on after the retrain
	add_test(NAME agere_v92_moh_request COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_agere>
		$<TARGET_FILE:half_open8> "ATX3;AT+MS=V92")
	set_tests_properties(agere_v92_moh_request PROPERTIES LABELS agere
		ENVIRONMENT
		"${_env};NET_LOOP=0.45;NET=8000:8000:ulaw;DSP_G711=ulaw;STREAM=40;MIDCALL_B=8:AT+PMHR\;WAIT 6;EXPECT_B=(?s)modem-on-hold request.*retrains instead.*V.90 retrained;PROTO_B=ATX3\;AT+MS=92")
	# at the alignment that took 33333 bit/s with half a sample of delay
	add_test(NAME agere_v90_phase96 COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_agere>
		$<TARGET_FILE:half_open8> "ATX3;AT+MS=V90")
	set_tests_properties(agere_v90_phase96 PROPERTIES LABELS agere
		ENVIRONMENT
		"${_env};NET_LOOP=0.45;AGERE_SYNC_PHASE=96;NET=8000:8000:ulaw;DSP_G711=ulaw;EXPECT_B=V.90 connect 5[0-9]{4}/;PROTO_B=ATX3\;AT+MS=90")
	# V.90 rate renegotiation: ours to 19 200 bit/s upstream (the Agere
	# modem's own, as noise on the PCM codes rises, come too late for a
	# test, if at all)
	add_test(NAME agere_v90_reneg_open COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_agere>
		$<TARGET_FILE:half_open8> "ATX3;AT+MS=V90")
	set_tests_properties(agere_v90_reneg_open PROPERTIES LABELS agere
		ENVIRONMENT
		"${_env};NET_LOOP=0.45;NET=8000:8000:ulaw;DSP_G711=ulaw;STREAM=15;DSP_V34_RENEG=5000:8;EXPECT_B=V.90 renegotiated [0-9]+/19200;PROTO_B=ATX3\;AT+MS=90")
	# class 1 fax both ways at 8000 Hz
	foreach(_m 24 48 72 96 73 97 121 122 145)
		add_test(NAME agere_fax_${_m}_agere_sends COMMAND
			${Python3_EXECUTABLE} ${_py}/fax_interop.py
			$<TARGET_FILE:half_agere> $<TARGET_FILE:half_open8> ${_m})
		add_test(NAME agere_fax_${_m}_open_sends COMMAND
			${Python3_EXECUTABLE} ${_py}/fax_interop.py
			$<TARGET_FILE:half_open8> $<TARGET_FILE:half_agere> ${_m})
		set_tests_properties(agere_fax_${_m}_agere_sends
			agere_fax_${_m}_open_sends PROPERTIES LABELS agere
			ENVIRONMENT "${_env}")
	endforeach()
endif()

# spandsp's modem (8000 Hz) and the open modem (9600 Hz) call each other
# at each modulation it has (its own label: CI's "spandsp" are quick)
if(TARGET half_spandsp)
	foreach(_m 21 22 122 23 103)
		add_test(NAME spandsp_${_m}_spandsp_calls COMMAND
			${Python3_EXECUTABLE} ${_py}/interop.py
			$<TARGET_FILE:half_spandsp> $<TARGET_FILE:half_open>
			"ATX3;AT+MS=${_m}")
		set_tests_properties(spandsp_${_m}_spandsp_calls PROPERTIES
			LABELS spanpeer ENVIRONMENT
			"${_env};NET=8000:9600:ulaw;PROTO_B=ATX3\;AT+MS=${_m}")
		add_test(NAME spandsp_${_m}_open_calls COMMAND
			${Python3_EXECUTABLE} ${_py}/interop.py
			$<TARGET_FILE:half_open> $<TARGET_FILE:half_spandsp>
			"ATX3;AT+MS=${_m}")
		set_tests_properties(spandsp_${_m}_open_calls PROPERTIES
			LABELS spanpeer ENVIRONMENT
			"${_env};NET=9600:8000:ulaw;PROTO_B=ATX3\;AT+MS=${_m}")
	endforeach()
endif()

# the Motorola SM56 soft modem (8000 Hz) and the open modem (9600 Hz)
# call each other, as the Agere modem's tests
if(TARGET half_sm56)
	# (its automode off below V.34: +MS=V22 alone is an error.  Calling
	# at V.22bis it is in automode: forced to V.22bis it takes the
	# answer tone for unscrambled ones, see the README)
	foreach(_p V21,0:21 V22,0:22 V22B,0:122 V32B,0:132 V34:34)
		string(REPLACE ":" ";" _p ${_p})
		list(GET _p 0 _a)
		list(GET _p 1 _ms)
		set(_c ${_a})
		if(_ms STREQUAL "122")
			set(_c V92)
		endif()
		add_test(NAME sm56_${_ms}_sm56_calls COMMAND
			${Python3_EXECUTABLE} ${_py}/interop.py
			$<TARGET_FILE:half_sm56> $<TARGET_FILE:half_open>
			"ATX3;AT+MS=${_c}")
		set_tests_properties(sm56_${_ms}_sm56_calls PROPERTIES
			LABELS sm56 ENVIRONMENT
			"${_env};NET=8000:9600:ulaw;PROTO_B=ATX3\;AT+MS=${_ms}")
		add_test(NAME sm56_${_ms}_open_calls COMMAND
			${Python3_EXECUTABLE} ${_py}/interop.py
			$<TARGET_FILE:half_open> $<TARGET_FILE:half_sm56>
			"ATX3;AT+MS=${_ms}")
		set_tests_properties(sm56_${_ms}_open_calls PROPERTIES
			LABELS sm56 ENVIRONMENT
			"${_env};NET=9600:8000:ulaw;PROTO_B=ATX3\;AT+MS=${_a}")
	endforeach()
	# V.90: the SM56 modem as the analogue modem, the open one as the
	# digital modem (both at 8000 Hz; the SM56 modem's codec runs at
	# 9600 Hz for it)
	foreach(_law ulaw alaw)
		add_test(NAME sm56_v90_${_law} COMMAND ${Python3_EXECUTABLE}
			${_py}/interop.py $<TARGET_FILE:half_sm56>
			$<TARGET_FILE:half_open8> "ATX3;AT+MS=V90")
		set_tests_properties(sm56_v90_${_law} PROPERTIES LABELS sm56
			ENVIRONMENT
			"${_env};NET_LOOP=0.45;NET=8000:8000:${_law};DSP_G711=${_law};EXPECT_B=V.90 connect 5[0-9]{4}/;PROTO_B=ATX3\;AT+MS=90")
	endforeach()
	# V.92: its QC1a on the first call already; on the second, quick
	# connect with short phase 2.  (Its upstream stays V.34: the
	# module chooses PCM upstream only in quick connect's INFO1a, from
	# an INFO1d flag its full phase 2 has cleared)
	add_test(NAME sm56_v92_qc COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_sm56>
		$<TARGET_FILE:half_open8> "ATX3;AT+MS=V92")
	set_tests_properties(sm56_v92_qc PROPERTIES LABELS sm56
		ENVIRONMENT
		"${_env};NET_LOOP=0.45;NET=8000:8000:ulaw;DSP_G711=ulaw;CALLS=2;DIAL=5551234;STREAM=3;EXPECT_B=(?s)V.92 quick connect.*V.92 short phase 2.*V.90 connect;PROTO_B=ATX3\;AT+MS=92")
	# V.92 modem-on-hold: it asks (AT+PMHR), we grant it; it comes back
	# with quick connect (a while after its ATO)
	add_test(NAME sm56_v92_moh_grant COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_sm56>
		$<TARGET_FILE:half_open8> "ATX3;AT+MS=V92")
	set_tests_properties(sm56_v92_moh_grant PROPERTIES LABELS sm56
		ENVIRONMENT
		"${_env};NET_LOOP=0.45;NET=8000:8000:ulaw;DSP_G711=ulaw;STREAM=60;MIDCALL_A=8:AT+PMHR\;WAIT 10;EXPECT_B=(?s)MHreq: MHack.*V.92 quick connect.*V.90 back from hold;PROTO_B=ATX3\;AT+MS=92")
	# and ours: as the Agere modem, it takes our tone B for a retrain's
	# (it answers MHreq only where its phase 2 begins, in place of
	# INFO0d, and one that knows nothing of it is no different until
	# then)
	add_test(NAME sm56_v92_moh_request COMMAND ${Python3_EXECUTABLE}
		${_py}/interop.py $<TARGET_FILE:half_sm56>
		$<TARGET_FILE:half_open8> "ATX3;AT+MS=V92")
	set_tests_properties(sm56_v92_moh_request PROPERTIES LABELS sm56
		ENVIRONMENT
		"${_env};NET_LOOP=0.45;NET=8000:8000:ulaw;DSP_G711=ulaw;STREAM=40;MIDCALL_B=8:AT+PMHR\;WAIT 6;EXPECT_B=(?s)modem-on-hold request.*retrains instead.*V.90 retrained;PROTO_B=ATX3\;AT+MS=92")
endif()

# the calls run in real time and mostly wait, so they can run in parallel
# (ctest -L interop -j8): each writes its modem logs to logs/<test>/
get_property(_tests DIRECTORY PROPERTY TESTS)
foreach(_t ${_tests})
	get_test_property(${_t} LABELS _l)
	if(_l MATCHES "interop|agere|sm56|spanpeer")
		set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT
			"LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/logs/${_t}")
	endif()
endforeach()
