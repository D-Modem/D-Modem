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
project(dmodem VERSION 1.0.0 LANGUAGES C CXX)

# the version the modem gives (ATI, AT+GMR, dmodemd -v), in every program
# and test built here
add_compile_definitions("DMODEM_VERSION=\"${PROJECT_VERSION}\"")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DMODEM_LIBRE "auto" CACHE STRING "libre: auto, system or fetch")
set_property(CACHE DMODEM_LIBRE PROPERTY STRINGS auto system fetch)
if(ANDROID)
	set(_crypto mbedtls)
else()
	set(_crypto openssl)
endif()
set(DMODEM_CRYPTO ${_crypto} CACHE STRING "libre's crypto: openssl or mbedtls")
set_property(CACHE DMODEM_CRYPTO PROPERTY STRINGS openssl mbedtls)
option(DMODEM_DMODEMD "Build dmodemd (SIP, needs libre)" ON)
option(DMODEM_ASTERISK "Build app_dmodem when Asterisk's headers are found" ON)
option(DMODEM_FREESWITCH "Build mod_dmodem when FreeSWITCH is found" ON)
option(DMODEM_ARI "Build dmodem-ari (D-Modem as an Asterisk ARI application)" ON)
option(DMODEM_TESTS "Build the test programs (ctest)" ON)
option(DMODEM_WERROR "Treat compiler warnings as errors (CI)" OFF)

if(DMODEM_WERROR)
	add_compile_options(-Werror)
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
