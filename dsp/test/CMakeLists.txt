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

dmodem_test_exe(dsp_loopback dsp_loopback.cpp)
target_link_libraries(dsp_loopback PRIVATE dmodem_dsp)
dmodem_test_exe(fax_loopback fax_loopback.cpp ${_fax})
dmodem_test_exe(netsim netsim.cpp ${S}/dsp_util.cpp)
dmodem_test_exe(t4_test t4_test.cpp ${S}/t4.cpp ${S}/tiff.cpp)
dmodem_test_exe(fax_pair fax_pair.cpp ${S}/t30.cpp ${S}/t4.cpp ${S}/tiff.cpp ${_fax})
dmodem_test_exe(dmodem_pair dmodem_pair.cpp)
target_link_libraries(dmodem_pair PRIVATE dmodem)
dmodem_test_exe(dmodem_cxx dmodem_cxx.cpp)
target_link_libraries(dmodem_cxx PRIVATE dmodem)
dmodem_test_exe(dmodem_fax_test dmodem_fax_test.cpp)
target_link_libraries(dmodem_fax_test PRIVATE dmodem)

# --- quick: the data pumps back to back (dsp_loopback <dp> [snr]
#     [seconds] [srate] [gain] [ppm] [echo dB] [far echo dB])
set(_loopback
	"132 40 20" "132 25 20" "132 30 20 8000" "132 30 30 9600 0.5 300"
	"132 30 20 9600 0.5 0 -6" "132 11 20" "32 30 20"
	"122 40 20" "122 15 20" "122 30 20 8000" "122 30 30 9600 2 -300"
	"122 30 30 9600 0.05 300" "8 30 20" "8 40 22 9600 0.5 0 -10 -15"
	"8 40 22 8000 0.5 0 -10 -15" "22 15 20" "212 15 20" "21 8 20"
	"23 12 30" "103 8 20 9600 0.5 300")
foreach(_args ${_loopback})
	separate_arguments(_a UNIX_COMMAND "${_args}")
	string(REPLACE " " "_" _n "${_args}")
	add_test(NAME loopback_${_n} COMMAND dsp_loopback ${_a})
	set_tests_properties(loopback_${_n} PROPERTIES LABELS quick)
endforeach()
# a far echo over VoIP; the automode cases (CALLER_DP: the caller's pump)
add_test(NAME loopback_far_echo_voip COMMAND dsp_loopback 8 40 22 9600 0.5 0 0 -15)
set_tests_properties(loopback_far_echo_voip PROPERTIES LABELS quick
	ENVIRONMENT FAR_DELAY=1920)
foreach(_c "122 8 25" "8 22 25" "8 21 25" "8 23 25" "8 103 30" "8 212 25"
	"21 8 45" "23 8 60" "103 8 45")
	# caller's pump, answerer's pump, seconds
	separate_arguments(_a UNIX_COMMAND "${_c}")
	list(POP_FRONT _a _caller)
	list(POP_BACK _a _secs)
	string(REPLACE " " "_" _n "${_c}")
	add_test(NAME automode_${_n} COMMAND dsp_loopback ${_a} 30 ${_secs})
	set_tests_properties(automode_${_n} PROPERTIES LABELS quick
		ENVIRONMENT CALLER_DP=${_caller})
endforeach()
foreach(_m 24 48 72 73 74 96 97 98 121 122 145 146)
	add_test(NAME fax_loopback_${_m} COMMAND fax_loopback ${_m} 30)
	set_tests_properties(fax_loopback_${_m} PROPERTIES LABELS quick)
endforeach()
add_test(NAME fax_loopback_48_snr20 COMMAND fax_loopback 48 20)
add_test(NAME fax_loopback_96_snr25 COMMAND fax_loopback 96 25)
add_test(NAME fax_loopback_98_8000 COMMAND fax_loopback 98 25 20000 8000)
set_tests_properties(fax_loopback_48_snr20 fax_loopback_96_snr25
	fax_loopback_98_8000 PROPERTIES LABELS quick)

# --- quick: libdmodem, pairs of its modems back to back
foreach(_ms 22 122 32 132 34)
	add_test(NAME dmodem_pair_${_ms} COMMAND dmodem_pair +MS=${_ms} 3 30)
	set_tests_properties(dmodem_pair_${_ms} PROPERTIES LABELS quick)
endforeach()
# a caller that tries V.34 and V.32bis first against an answerer of
# +MS=122: it must find the caller's 100 ms of S1 and connect at 2400 (two
# sample alignments of the two modems)
foreach(_d 0 5)
	add_test(NAME dmodem_pair_v8_to_122_d${_d} COMMAND dmodem_pair "" 1 30)
	set_tests_properties(dmodem_pair_v8_to_122_d${_d} PROPERTIES
		LABELS quick ENVIRONMENT
		"DMODEM_MS_B=+MS=122;DMODEM_RATE=2400;DMODEM_DELAY=${_d}")
endforeach()
# the same through the C++ classes of dmodem.hpp
add_test(NAME dmodem_cxx COMMAND dmodem_cxx +MS=132 20)
set_tests_properties(dmodem_cxx PROPERTIES LABELS quick)
# a V.32bis retrain in the middle of the data (V.42 carries on)
add_test(NAME dmodem_pair_v32_retrain COMMAND dmodem_pair +MS=132 1 30)
set_tests_properties(dmodem_pair_v32_retrain PROPERTIES LABELS quick
	ENVIRONMENT DSP_V32_RETRAIN=1500)
# V.34 retrains in the middle of the data: started by both modems, by the
# answerer or the caller alone (the other one follows)
foreach(_side both a c)
	if(_side STREQUAL "both")
		set(_t 3000)
	else()
		set(_t 3000${_side})
	endif()
	add_test(NAME dmodem_pair_v34_retrain_${_side}
		COMMAND dmodem_pair +MS=34 1 60)
	set_tests_properties(dmodem_pair_v34_retrain_${_side} PROPERTIES
		LABELS quick ENVIRONMENT DSP_V34_RETRAIN=${_t})
endforeach()

# --- quick: V.91 between two of them on a G.711 network (DMODEM_NET): as
#     it is, with robbed bits, a 3 dB pad, mu-law to A-law; the DIL shows
#     the receiver which codes come through
foreach(_net g711 rbs pad xlaw)
	add_test(NAME dmodem_pair_v91_${_net} COMMAND dmodem_pair +MS=91 1 25)
	set_tests_properties(dmodem_pair_v91_${_net} PROPERTIES LABELS quick
		ENVIRONMENT "DMODEM_NET=${_net};DMODEM_RATE=60000")
endforeach()
add_test(NAME dmodem_pair_v91_alaw COMMAND dmodem_pair +MS=91 1 25)
set_tests_properties(dmodem_pair_v91_alaw PROPERTIES LABELS quick
	ENVIRONMENT "DMODEM_NET=g711;DMODEM_LAW=alaw;DMODEM_RATE=62666")
# the default AT+MS=92, and K56flex's AT+MS=56 (V.90), find V.91 as well;
# 200 kB that do not compress
add_test(NAME dmodem_pair_v91_bulk COMMAND dmodem_pair +MS=92 1 60)
set_tests_properties(dmodem_pair_v91_bulk PROPERTIES LABELS quick
	ENVIRONMENT "DMODEM_NET=g711;DMODEM_RATE=62666;DMODEM_BULK=200000")
add_test(NAME dmodem_pair_ms56 COMMAND dmodem_pair +MS=56 1 25)
set_tests_properties(dmodem_pair_ms56 PROPERTIES LABELS quick
	ENVIRONMENT "DMODEM_NET=g711")
# a retrain by the caller in the data (V.42 carries on); the control
# channel; one modem silent in V.91: both escape to V.34
add_test(NAME dmodem_pair_v91_retrain COMMAND dmodem_pair +MS=91 1 30)
set_tests_properties(dmodem_pair_v91_retrain PROPERTIES LABELS quick
	ENVIRONMENT "DMODEM_NET=g711;DSP_V91_RETRAIN=3000c")
add_test(NAME dmodem_pair_v91_cc COMMAND dmodem_pair +MS=91 1 25)
set_tests_properties(dmodem_pair_v91_cc PROPERTIES LABELS quick
	ENVIRONMENT "DMODEM_NET=rbs;DSP_V91_CC=1;DMODEM_RATE=60000")
add_test(NAME dmodem_pair_v91_escape COMMAND dmodem_pair +MS=91 1 35)
set_tests_properties(dmodem_pair_v91_escape PROPERTIES LABELS quick
	ENVIRONMENT "DMODEM_NET=g711;DSP_V91_MUTE=a;DMODEM_RATE=33600")

# --- quick: fax, the page coding against libtiff (tiffcp), two T.30
#     machines back to back at each rate, libdmodem's fax (CNG hand-over)
set(_ft ${CMAKE_CURRENT_BINARY_DIR}/fax)
find_program(TIFFCP tiffcp)
add_test(NAME fax_documents COMMAND sh -c
	"mkdir -p ${_ft} && $<TARGET_FILE:t4_test> write ${_ft}/doc.tif && \
	 $<TARGET_FILE:t4_test> light ${_ft}/light.tif")
set_tests_properties(fax_documents PROPERTIES LABELS quick
	FIXTURES_SETUP fax_docs)
if(TIFFCP)
	foreach(_c none g3:1d g3:2d g4)
		string(REPLACE ":" "_" _n ${_c})
		add_test(NAME t4_libtiff_${_n} COMMAND sh -c
			"${TIFFCP} -c ${_c} ${_ft}/doc.tif ${_ft}/lib_${_n}.tif && \
			 $<TARGET_FILE:t4_test> compare ${_ft}/doc.tif ${_ft}/lib_${_n}.tif")
		set_tests_properties(t4_libtiff_${_n} PROPERTIES LABELS quick
			FIXTURES_REQUIRED fax_docs)
	endforeach()
endif()
foreach(_r 14400 12000 9600 7200 4800 2400)
	add_test(NAME fax_pair_${_r} COMMAND sh -c
		"$<TARGET_FILE:fax_pair> ${_ft}/light.tif ${_ft}/rx_${_r}.tif ${_r} && \
		 $<TARGET_FILE:t4_test> compare ${_ft}/light.tif ${_ft}/rx_${_r}.tif")
	set_tests_properties(fax_pair_${_r} PROPERTIES LABELS quick
		FIXTURES_REQUIRED fax_docs)
endforeach()
# error correction mode: the page in blocks of HDLC frames; on a line of
# 19 dB the frames errors spoil are sent again (without ECM the page
# comes with bad rows)
foreach(_r 14400 9600)
	add_test(NAME fax_pair_ecm_${_r} COMMAND sh -c
		"$<TARGET_FILE:fax_pair> ${_ft}/doc.tif ${_ft}/rx_ecm_${_r}.tif ${_r} && \
		 $<TARGET_FILE:t4_test> compare ${_ft}/doc.tif ${_ft}/rx_ecm_${_r}.tif")
	set_tests_properties(fax_pair_ecm_${_r} PROPERTIES LABELS quick
		FIXTURES_REQUIRED fax_docs ENVIRONMENT FAX_ECM=1)
endforeach()
add_test(NAME fax_pair_ecm_snr19 COMMAND sh -c
	"$<TARGET_FILE:fax_pair> ${_ft}/light.tif ${_ft}/rx_ecm_19.tif 14400 19 && \
	 $<TARGET_FILE:t4_test> compare ${_ft}/light.tif ${_ft}/rx_ecm_19.tif")
set_tests_properties(fax_pair_ecm_snr19 PROPERTIES LABELS quick
	FIXTURES_REQUIRED fax_docs ENVIRONMENT FAX_ECM=1)
foreach(_law ulaw alaw)
	add_test(NAME dmodem_fax_${_law} COMMAND sh -c
		"$<TARGET_FILE:dmodem_fax_test> ${_ft}/light.tif ${_ft}/rx_${_law}.tif ${_law} && \
		 $<TARGET_FILE:t4_test> compare ${_ft}/light.tif ${_ft}/rx_${_law}.tif")
	set_tests_properties(dmodem_fax_${_law} PROPERTIES LABELS quick
		FIXTURES_REQUIRED fax_docs)
endforeach()

# --- spandsp: its V.17 transmitter and T.4 encoder (as Asterisk's SendFAX
#     uses them) into our receivers
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
	pkg_check_modules(SPANDSP QUIET spandsp)
endif()
if(SPANDSP_FOUND)
	dmodem_test_exe(spandsp_v17 spandsp_v17.cpp spandsp_tx.cpp ${_fax})
	dmodem_test_exe(t4_spandsp t4_spandsp.cpp spandsp_tx.cpp ${S}/t4.cpp ${S}/tiff.cpp)
	# our T.30 against spandsp's (fax_state_t), both ways, with and
	# without error correction mode, ECM also with line hits
	dmodem_test_exe(t30_spandsp t30_spandsp.cpp ${S}/t30.cpp ${S}/t4.cpp
		${S}/tiff.cpp ${_fax})
	foreach(_t spandsp_v17 t4_spandsp t30_spandsp)
		target_include_directories(${_t} PRIVATE ${SPANDSP_INCLUDE_DIRS})
		target_link_libraries(${_t} PRIVATE ${SPANDSP_LDFLAGS})
	endforeach()
	foreach(_r 14400 12000 9600 7200)
		add_test(NAME spandsp_v17_${_r} COMMAND spandsp_v17 ${_r})
		set_tests_properties(spandsp_v17_${_r} PROPERTIES LABELS spandsp)
	endforeach()
	foreach(_e 1 2)
		foreach(_m 0 144)
			add_test(NAME t4_spandsp_${_e}_${_m} COMMAND t4_spandsp
				${_ft}/light.tif ${_e} ${_m})
			set_tests_properties(t4_spandsp_${_e}_${_m} PROPERTIES
				LABELS spandsp FIXTURES_REQUIRED fax_docs)
		endforeach()
	endforeach()
	foreach(_d to from)
		foreach(_e 0 1)
			add_test(NAME t30_spandsp_${_d}_ecm${_e} COMMAND sh -c
				"$<TARGET_FILE:t30_spandsp> ${_d} ${_ft}/light.tif ${_ft}/sp_${_d}_${_e}.tif ${_e} && \
				 $<TARGET_FILE:t4_test> compare ${_ft}/light.tif ${_ft}/sp_${_d}_${_e}.tif")
			set_tests_properties(t30_spandsp_${_d}_ecm${_e} PROPERTIES
				LABELS spandsp FIXTURES_REQUIRED fax_docs)
		endforeach()
		# line hits: ECM sends the frames they spoil again
		add_test(NAME t30_spandsp_${_d}_ecm_hits COMMAND sh -c
			"$<TARGET_FILE:t30_spandsp> ${_d} ${_ft}/light.tif ${_ft}/sp_${_d}_h.tif 1 30 && \
			 $<TARGET_FILE:t4_test> compare ${_ft}/light.tif ${_ft}/sp_${_d}_h.tif")
		set_tests_properties(t30_spandsp_${_d}_ecm_hits PROPERTIES
			LABELS spandsp FIXTURES_REQUIRED fax_docs)
	endforeach()
	foreach(_m 0 144)
		add_test(NAME spandsp_v17_page_${_m} COMMAND spandsp_v17 14400)
		set_tests_properties(spandsp_v17_page_${_m} PROPERTIES
			LABELS spandsp FIXTURES_REQUIRED fax_docs
			ENVIRONMENT "T4_FILE=${_ft}/light.tif;T4_MIN=${_m}")
	endforeach()
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
# V.90: the original library (analogue modem) calls the open one (digital
# modem, 8000 Hz) through the simulated G.711 network
add_test(NAME interop_v90_ulaw COMMAND ${Python3_EXECUTABLE} ${_py}/interop.py
	$<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8> "ATX3;AT+MS=90")
set_tests_properties(interop_v90_ulaw PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:ulaw")
add_test(NAME interop_v90_alaw COMMAND ${Python3_EXECUTABLE} ${_py}/interop.py
	$<TARGET_FILE:half_blob> $<TARGET_FILE:half_open8> "ATX3;AT+MS=90")
set_tests_properties(interop_v90_alaw PROPERTIES LABELS interop
	ENVIRONMENT "${_env};NET=9600:8000:alaw;DSP_G711=alaw")
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
	# V.90: the Agere modem as the analogue modem, the open one as the
	# digital modem (both at 8000 Hz)
	foreach(_law ulaw alaw)
		add_test(NAME agere_v90_${_law} COMMAND ${Python3_EXECUTABLE}
			${_py}/interop.py $<TARGET_FILE:half_agere>
			$<TARGET_FILE:half_open8> "ATX3;AT+MS=V90")
		set_tests_properties(agere_v90_${_law} PROPERTIES LABELS agere
			ENVIRONMENT
			"${_env};NET=8000:8000:${_law};DSP_G711=${_law};PROTO_B=ATX3\;AT+MS=90")
	endforeach()
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

# the calls run in real time and mostly wait, so they can run in parallel
# (ctest -L interop -j8): each writes its modem logs to logs/<test>/
get_property(_tests DIRECTORY PROPERTY TESTS)
foreach(_t ${_tests})
	get_test_property(${_t} LABELS _l)
	if(_l MATCHES "interop|agere")
		set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT
			"LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/logs/${_t}")
	endif()
endforeach()
