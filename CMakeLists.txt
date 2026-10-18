# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
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
#                      for Android, which has no OpenSSL of its own)
#   DMODEM_DMODEMD     build dmodemd (its SIP on libre)
#   DMODEM_ASTERISK    build app_dmodem when Asterisk's headers are found
#   DMODEM_TESTS       build the test programs and register them with ctest
#                      (with a 32 bit compiler also the interop tests
#                      against Smart Link's original DSP library)
#   DMODEM_WERROR      warnings are errors (as in CI)
cmake_minimum_required(VERSION 3.16)
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
if(ANDROID OR IOS OR WIN32)
	# (no system OpenSSL to link: Mbed TLS's, built with the modem)
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
	if(DMODEM_WERROR)
		add_compile_options(-Werror)
	endif()
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
endif()
if(MINGW)
	# MinGW's own C99 printf and scanf: msvcrt's know no %zu or %lld.
	# (GCC checks format(printf) functions against msvcrt's printf
	# there all the same: the POSIX builds check the formats)
	add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
	add_compile_options(-Wno-format)
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
add_subdirectory(link)
add_subdirectory(dsp)
add_subdirectory(core)
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

if(DMODEM_TESTS)
	add_subdirectory(dsp/test)
	add_subdirectory(core/test)
endif()
