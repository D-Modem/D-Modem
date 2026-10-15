# D-Modem: a software modem on SIP calls (slmodemd, with its SIP user
# agent built in), its modem core as a library (libdmodem) and the
# Asterisk module built on it.
#
#   cmake -B build && cmake --build build
#   ctest --test-dir build -L quick           the fast tests
#
# Options:
#   DMODEM_PJPROJECT   auto: an installed pjproject (pkg-config
#                      libpjproject), else fetched; system: installed only;
#                      fetch: always build the pinned release
#   DMODEM_SLMODEMD    build slmodemd (needs pjproject)
#   DMODEM_ASTERISK    build app_dmodem when Asterisk's headers are found
#   DMODEM_TESTS       build the test programs and register them with ctest
#                      (with a 32 bit compiler also the interop tests
#                      against Smart Link's original DSP library)
#   DMODEM_WERROR      warnings are errors (as in CI)
cmake_minimum_required(VERSION 3.16)
project(dmodem VERSION 1.0.0 LANGUAGES C CXX)

# the version the modem gives (ATI, AT+GMR, slmodemd -v), in every program
# and test built here
add_compile_definitions("DMODEM_VERSION=\"${PROJECT_VERSION}\"")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DMODEM_PJPROJECT "auto" CACHE STRING "pjproject: auto, system or fetch")
set_property(CACHE DMODEM_PJPROJECT PROPERTY STRINGS auto system fetch)
option(DMODEM_SLMODEMD "Build slmodemd (SIP, needs pjproject)" ON)
option(DMODEM_ASTERISK "Build app_dmodem when Asterisk's headers are found" ON)
option(DMODEM_FREESWITCH "Build mod_dmodem when FreeSWITCH is found" ON)
option(DMODEM_TESTS "Build the test programs (ctest)" ON)
option(DMODEM_WERROR "Treat compiler warnings as errors (CI)" OFF)

if(DMODEM_WERROR)
	add_compile_options(-Werror)
endif()

include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

if(DMODEM_SLMODEMD)
	include(pjproject)
endif()
if(DMODEM_TESTS)
	enable_testing()
	include(gtest)
endif()
add_subdirectory(link)
add_subdirectory(dsp)
add_subdirectory(core)
add_subdirectory(slmodemd)

# the PBX modules' call on libdmodem (pbx/dmodem_bridge.c), built into each
set(DMODEM_PBX_DIR ${CMAKE_SOURCE_DIR}/pbx)
if(DMODEM_ASTERISK)
	add_subdirectory(asterisk)
endif()
if(DMODEM_FREESWITCH)
	add_subdirectory(freeswitch)
endif()

if(DMODEM_TESTS)
	add_subdirectory(dsp/test)
	add_subdirectory(core/test)
endif()
