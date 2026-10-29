# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# D-Modem: a software modem on SIP calls (dmodemd, with its SIP user
# agent built in), its modem core as a library (libdmodem) and the
# Asterisk module built on it.
#
#   cmake -B build && cmake --build build
#   ctest --test-dir build -L quick           the fast tests
#
# Options:
#   DMODEM_LIBRE       auto: an installed libre (pkg-config libre), else
#                      fetched; system: installed only; fetch: always
#                      build the pinned release
#   DMODEM_CRYPTO      the fetched libre's digests: openssl (the system's),
#                      or mbedtls (fetched and built with it; the default
#                      for Android, Windows and Apple's systems, which have
#                      no OpenSSL of their own)
#   DMODEM_DMODEMD     build dmodemd (its SIP on libre)
#   DMODEM_ASTERISK    build app_dmodem when Asterisk's headers are found
#   DMODEM_TESTS       build the test programs and register them with ctest
#                      (with a 32 bit compiler also the interop tests
#                      against Smart Link's original DSP library)
#   DMODEM_USB         Windows and macOS: dmodemd's --usb (the SmartUSB56,
#                      the pico-serial Line and the Agere modem through
#                      libusb, fetched and built)
#   DMODEM_WERROR      warnings are errors (as in CI)
#   DMODEM_PREFER_CLANG  Clang where it is installed, not GCC (a first
#                      configure without CC, CXX or a toolchain file)
cmake_minimum_required(VERSION 3.16)
# (cmake/rules.cmake: after CMake's settings for each compiler)
if(NOT CMAKE_USER_MAKE_RULES_OVERRIDE)
	set(CMAKE_USER_MAKE_RULES_OVERRIDE ${CMAKE_CURRENT_LIST_DIR}/cmake/rules.cmake)
endif()
# the compilers: Clang where it is installed (clang and clang++, else the
# newest clang-<n>), GCC otherwise.  Only when nothing else chose them:
# CC and CXX, -DCMAKE_C_COMPILER, a toolchain file (cross builds: their
# own, cmake/mingw-w64.cmake takes Clang too), or an existing build
# directory's cache; and not on Windows hosts (MSVC's cl.exe)
option(DMODEM_PREFER_CLANG "Clang where it is installed, not GCC" ON)
if(DMODEM_PREFER_CLANG AND NOT CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_HOST_WIN32
   AND NOT CMAKE_C_COMPILER AND NOT CMAKE_CXX_COMPILER
   AND NOT DEFINED ENV{CC} AND NOT DEFINED ENV{CXX})
	foreach(_v "" -22 -21 -20 -19 -18 -17 -16 -15)
		find_program(_dmodem_clang clang${_v})
		find_program(_dmodem_clangxx clang++${_v})
		set(_c ${_dmodem_clang})
		set(_cxx ${_dmodem_clangxx})
		unset(_dmodem_clang CACHE)
		unset(_dmodem_clangxx CACHE)
		if(_c AND _cxx)
			set(CMAKE_C_COMPILER ${_c})
			set(CMAKE_CXX_COMPILER ${_cxx})
			break()
		endif()
	endforeach()
endif()
# macOS: 11 (Big Sur, the first for Apple silicon) and later, unless
# asked otherwise; CMAKE_OSX_ARCHITECTURES="arm64;x86_64" builds both
# (docs/macos.md)
if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE AND
   NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING
		"the oldest macOS to run on")
endif()
# MSVC: the static C runtime (/MT), here and in the projects built for
# it (libre, Mbed TLS): one dmodemd.exe, no vcruntime DLL beside it
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# (no build type given: MSVC's RelWithDebInfo below, not its Debug)
if(NOT DEFINED CMAKE_BUILD_TYPE)
	set(_dmodem_no_build_type TRUE)
endif()
project(dmodem VERSION 1.0.0 LANGUAGES C CXX)

# the version the modem gives (ATI, AT+GMR, dmodemd -v), in every program
# and test built here
add_compile_definitions("DMODEM_VERSION=\"${PROJECT_VERSION}\"")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DMODEM_LIBRE "auto" CACHE STRING "libre: auto, system or fetch")
set_property(CACHE DMODEM_LIBRE PROPERTY STRINGS auto system fetch)
if(ANDROID OR APPLE OR WIN32)
	# (no system OpenSSL to link: Mbed TLS's, built with the modem;
	# macOS's own libcrypto is not for programs to link)
	set(_crypto mbedtls)
else()
	set(_crypto openssl)
endif()
set(DMODEM_CRYPTO ${_crypto} CACHE STRING "libre's crypto: openssl or mbedtls")
set_property(CACHE DMODEM_CRYPTO PROPERTY STRINGS openssl mbedtls)
option(DMODEM_DMODEMD "Build dmodemd (SIP, needs libre)" ON)
# (the PBX parts are POSIX's alone: off on Windows unless asked for)
if(WIN32)
	set(_dmodem_pbx_default OFF)
else()
	set(_dmodem_pbx_default ON)
endif()
option(DMODEM_ASTERISK "Build app_dmodem when Asterisk's headers are found"
	${_dmodem_pbx_default})
option(DMODEM_FREESWITCH "Build mod_dmodem when FreeSWITCH is found"
	${_dmodem_pbx_default})
option(DMODEM_ARI "Build dmodem-ari (D-Modem as an Asterisk ARI application)"
	${_dmodem_pbx_default})
option(DMODEM_TESTS "Build the test programs (ctest)" ON)
# (Windows and macOS: libusb is fetched and built for it,
# cmake/libusb.cmake; elsewhere the system's, pkg-config's libusb-1.0)
option(DMODEM_USB "dmodemd's --usb on Windows and macOS (libusb, fetched)" ON)
option(DMODEM_WERROR "Treat compiler warnings as errors (CI)" OFF)

# the compilers' flags: GCC's and Clang's, or MSVC's (cl.exe, clang-cl).
# dmodem_flags(<target> [EXTRA] [NOOPT] [NODEBUG]) gives a target the
# warnings (-Wall, with -Wextra for EXTRA), and with GCC and Clang the
# debug information (-g) and optimisation (-O2) its own, whatever the
# build type; MSVC's come from the build type (RelWithDebInfo: Debug's
# unoptimised modem can't keep up).  DMODEM_LIBM is the maths library
# to link (none with MSVC).
if(MSVC)
	if(_dmodem_no_build_type AND NOT CMAKE_CONFIGURATION_TYPES)
		set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
			"Build type (Debug, Release, RelWithDebInfo)" FORCE)
	endif()
	set(DMODEM_LIBM "")
	# (the C library's POSIX names and its "unsafe" functions as they
	# are, sources as UTF-8, __cplusplus as the standard's, and the
	# conversion warnings GCC's -Wall doesn't give either)
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS
		_CRT_NONSTDC_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
	add_compile_options(/utf-8
		$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
		/wd4244 /wd4267 /wd4305 /wd4146 /wd4018 /wd4996)
	# (cl.exe: the standard's preprocessor and rules, not its old ones;
	# clang-cl has them already)
	add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
		$<$<CXX_COMPILER_ID:MSVC>:/permissive->)
	if(DMODEM_WERROR)
		add_compile_options(/WX)
	endif()
	# what of POSIX the C library lacks (compat/msvc/dmodem_msvc.h),
	# and unistd.h, strings.h, sys/time.h on it
	include_directories(BEFORE ${PROJECT_SOURCE_DIR}/compat/msvc)
	add_compile_options(/FI${PROJECT_SOURCE_DIR}/compat/msvc/dmodem_msvc.h)
else()
	set(DMODEM_LIBM m)
	# (Haiku: the sockets are libnetwork's, not the C library's)
	if(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
		link_libraries(network)
	endif()
	if(DMODEM_WERROR)
		add_compile_options(-Werror)
	endif()
	# a * b + c rounded twice everywhere, never fused into one FMA: the
	# pumps' arithmetic then comes out the same on every architecture
	# (Clang fuses by default where the CPU has FMA, as on Apple
	# silicon and arm64 Linux; GCC outside ISO mode too), and with it
	# the tests' recorded transcripts (core_equiv)
	add_compile_options(-ffp-contract=off)
endif()
function(dmodem_flags target)
	cmake_parse_arguments(F "EXTRA;NOOPT;NODEBUG" "" "" ${ARGN})
	if(MSVC)
		target_compile_options(${target} PRIVATE /W3)
		return()
	endif()
	target_compile_options(${target} PRIVATE -Wall)
	if(F_EXTRA)
		target_compile_options(${target} PRIVATE -Wextra)
	endif()
	if(NOT F_NODEBUG)
		target_compile_options(${target} PRIVATE -g)
	endif()
	if(NOT F_NOOPT)
		target_compile_options(${target} PRIVATE -O2)
	endif()
endfunction()

if(WIN32)
	# (M_PI and the like from math.h, which Windows' C library has
	# only on request) and no min/max macros from windows.h
	add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
	# the oldest Windows the programs run on: Windows 7 (0x0601), so
	# windows.h declares nothing newer.  win_imports.py (ctest
	# win7_imports) checks the programs' imports against it
	set(DMODEM_WINNT 0x0601 CACHE STRING
		"_WIN32_WINNT: the oldest Windows to run on (0x0601: Windows 7)")
	add_compile_definitions(_WIN32_WINNT=${DMODEM_WINNT}
		WINVER=${DMODEM_WINNT})
endif()
if(MINGW)
	# MinGW's own C99 printf and scanf: msvcrt's know no %zu or %lld.
	# (GCC checks format(printf) functions against msvcrt's printf
	# there all the same: the POSIX builds check the formats)
	add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
	add_compile_options(-Wno-format)
endif()

# Haiku: /boot/system is its packages' (read only); what isn't packaged
# goes in /boot/system/non-packaged, its data in data/, not share/
if(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
	if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
		set(CMAKE_INSTALL_PREFIX "/boot/system/non-packaged" CACHE PATH
			"Install path prefix" FORCE)
	endif()
	set(CMAKE_INSTALL_DATAROOTDIR "data" CACHE PATH
		"read-only architecture-independent data root")
	# (and no sbin: the programs in bin, which is on the PATH)
	set(CMAKE_INSTALL_SBINDIR "bin" CACHE PATH "system admin executables")
endif()
include(GNUInstallDirs)
# (PROJECT_SOURCE_DIR: also as another project's subdirectory)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
# the threads: -pthread, or nothing where libc has them (Android)
find_package(Threads REQUIRED)

if(DMODEM_DMODEMD)
	include(libre)
endif()
if(DMODEM_TESTS)
	enable_testing()
	include(test_logs)
	include(gtest)
endif()
# u8 .. s64 (include/dm_types.h), for every part of the tree
include_directories(${PROJECT_SOURCE_DIR}/include)

add_subdirectory(link)
add_subdirectory(dsp)
add_subdirectory(core)
add_subdirectory(winmodem)
add_subdirectory(dmodemd)

# the PBX modules' call on libdmodem (pbx/dmodem_bridge.c), built into each
set(DMODEM_PBX_DIR ${PROJECT_SOURCE_DIR}/pbx)
if(DMODEM_ASTERISK)
	add_subdirectory(asterisk)
endif()
if(DMODEM_FREESWITCH)
	add_subdirectory(freeswitch)
endif()
if(DMODEM_ARI)
	add_subdirectory(ari)
endif()
# the FreePBX module (PHP): its package and its tests
add_subdirectory(freepbx)

if(DMODEM_TESTS)
	add_subdirectory(dsp/test)
	add_subdirectory(core/test)

	# every function in the sources has a comment above it
	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(Python3_Interpreter_FOUND)
		dmodem_add_test(doc_check ${Python3_EXECUTABLE}
			${PROJECT_SOURCE_DIR}/cmake/doc_check.py
			${PROJECT_SOURCE_DIR})
		set_tests_properties(doc_check PROPERTIES LABELS quick
			TIMEOUT 120)
		# V.cvm's preset dictionary for V.44: the code's text is
		# the Recommendation's Annex E
		dmodem_add_test(v44_preset_check ${Python3_EXECUTABLE}
			${PROJECT_SOURCE_DIR}/cmake/v44_preset_check.py
			${PROJECT_SOURCE_DIR})
		set_tests_properties(v44_preset_check PROPERTIES LABELS quick
			TIMEOUT 60)
		# Windows: the programs and drivers load on Windows 7
		# (DMODEM_WINNT): no imports of Windows 8's or later
		if(WIN32)
			set(_win7)
			foreach(_t dmodemd dmodem-driver dmodemvm dmodemvm7)
				if(TARGET ${_t})
					get_target_property(_type ${_t} TYPE)
					if(_type MATCHES "EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY")
						list(APPEND _win7 $<TARGET_FILE:${_t}>)
					endif()
				endif()
			endforeach()
			if(_win7)
				dmodem_add_test(win7_imports ${Python3_EXECUTABLE}
					${PROJECT_SOURCE_DIR}/cmake/win_imports.py
					${_win7})
				set_tests_properties(win7_imports PROPERTIES
					LABELS quick TIMEOUT 60)
			endif()
		endif()
	endif()
endif()
