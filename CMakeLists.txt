# D-Modem: a software modem on SIP calls (slmodemd + d-modem), its modem
# core as a library (libdmodem) and the Asterisk module built on it.
#
#   cmake -B build [-DDMODEM_DSPLIBS=open] && cmake --build build
#   ctest --test-dir build -L quick           the fast tests
#
# Options:
#   DMODEM_DSPLIBS     blob: slmodemd links the original binary-only
#                      dsplibs.o (i386, built with -m32 on x86_64);
#                      open: the C data pumps in slmodemd/dsp (any arch)
#   DMODEM_PJPROJECT   auto: an installed pjproject (pkg-config
#                      libpjproject), else fetched; system: installed only;
#                      fetch: always build the pinned release
#   DMODEM_D_MODEM     build d-modem (needs pjproject)
#   DMODEM_ASTERISK    build app_dmodem when Asterisk's headers are found
#   DMODEM_TESTS       build the test programs and register them with ctest
cmake_minimum_required(VERSION 3.16)
project(dmodem C)

set(DMODEM_DSPLIBS "blob" CACHE STRING "slmodemd's data pumps: blob or open")
set_property(CACHE DMODEM_DSPLIBS PROPERTY STRINGS blob open)
set(DMODEM_PJPROJECT "auto" CACHE STRING "pjproject: auto, system or fetch")
set_property(CACHE DMODEM_PJPROJECT PROPERTY STRINGS auto system fetch)
option(DMODEM_D_MODEM "Build d-modem (SIP, needs pjproject)" ON)
option(DMODEM_ASTERISK "Build app_dmodem when Asterisk's headers are found" ON)
option(DMODEM_TESTS "Build the test programs (ctest)" ON)

include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

add_subdirectory(slmodemd)

if(DMODEM_D_MODEM)
	include(pjproject)
	add_executable(d-modem d-modem.c)
	target_link_libraries(d-modem PRIVATE pjproject m)
	install(TARGETS d-modem RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(DMODEM_ASTERISK)
	add_subdirectory(asterisk)
endif()

if(DMODEM_TESTS)
	enable_testing()
	add_subdirectory(slmodemd/dsp/test)
endif()
