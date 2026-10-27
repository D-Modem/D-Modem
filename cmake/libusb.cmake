# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# libusb-1.0 for dmodemd's --usb on Windows and macOS, as the static
# library `dmodem_libusb` (Linux takes the system's, pkg-config
# libusb-1.0).
#
# The pinned release's tarball is downloaded once while configuring (its
# SHA-256 checked) into the build tree, and its core and backend are
# built here with this build's compiler:
# - Windows (WinUSB, and UsbDk where it is installed): MinGW-w64, MSVC or
#   clang-cl, with our config.h (libusb_config.h.in), not its autotools'
#   or its Visual Studio one;
# - macOS (IOKit): Apple's Clang, with the release's own Xcode/config.h,
#   for every architecture of CMAKE_OSX_ARCHITECTURES (a universal
#   dmodemd needs a universal libusb, which Homebrew's is not).
# DMODEM_LIBUSB_DIR: an unpacked libusb source tree to use instead.
#
# libusb is LGPL-2.1-or-later (LICENSING.md).
set(DMODEM_LIBUSB_VERSION "1.0.27" CACHE STRING "libusb release to fetch")
set(DMODEM_LIBUSB_URL
	"https://github.com/libusb/libusb/releases/download/v${DMODEM_LIBUSB_VERSION}/libusb-${DMODEM_LIBUSB_VERSION}.tar.bz2"
	CACHE STRING "libusb release tarball")
set(DMODEM_LIBUSB_SHA256
	"ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575"
	CACHE STRING "the tarball's SHA-256")
set(DMODEM_LIBUSB_DIR "" CACHE PATH "a libusb source tree (else fetched)")

set(_usb_src ${DMODEM_LIBUSB_DIR})
if(NOT _usb_src)
	set(_deps ${CMAKE_BINARY_DIR}/_deps)
	set(_usb_tar ${_deps}/libusb-${DMODEM_LIBUSB_VERSION}.tar.bz2)
	set(_usb_src ${_deps}/libusb-${DMODEM_LIBUSB_VERSION})
	if(NOT EXISTS ${_usb_src}/libusb/libusb.h)
		message(STATUS "libusb: fetching ${DMODEM_LIBUSB_VERSION}")
		file(DOWNLOAD ${DMODEM_LIBUSB_URL} ${_usb_tar}
			EXPECTED_HASH SHA256=${DMODEM_LIBUSB_SHA256}
			STATUS _st TLS_VERIFY ON)
		list(GET _st 0 _code)
		if(NOT _code EQUAL 0)
			message(FATAL_ERROR "libusb: can't fetch ${DMODEM_LIBUSB_URL}: ${_st} "
				"(or give -DDMODEM_LIBUSB_DIR=<source tree>)")
		endif()
		file(ARCHIVE_EXTRACT INPUT ${_usb_tar} DESTINATION ${_deps})
	endif()
endif()
if(NOT EXISTS ${_usb_src}/libusb/core.c)
	message(FATAL_ERROR "libusb: no source tree in ${_usb_src}")
endif()
message(STATUS "libusb: ${_usb_src}")

set(_usb_cfg ${CMAKE_BINARY_DIR}/libusb-config)
if(APPLE)
	configure_file(${_usb_src}/Xcode/config.h ${_usb_cfg}/config.h
		COPYONLY)
	set(_usb_os
		${_usb_src}/libusb/os/events_posix.c
		${_usb_src}/libusb/os/threads_posix.c
		${_usb_src}/libusb/os/darwin_usb.c)
elseif(WIN32)
	configure_file(${CMAKE_CURRENT_LIST_DIR}/libusb_config.h.in
		${_usb_cfg}/config.h COPYONLY)
	set(_usb_os
		${_usb_src}/libusb/os/events_windows.c
		${_usb_src}/libusb/os/threads_windows.c
		${_usb_src}/libusb/os/windows_common.c
		${_usb_src}/libusb/os/windows_usbdk.c
		${_usb_src}/libusb/os/windows_winusb.c)
else()
	message(FATAL_ERROR "libusb: built here for Windows and macOS only "
		"(elsewhere the system's: pkg-config libusb-1.0)")
endif()
add_library(dmodem_libusb STATIC
	${_usb_src}/libusb/core.c
	${_usb_src}/libusb/descriptor.c
	${_usb_src}/libusb/hotplug.c
	${_usb_src}/libusb/io.c
	${_usb_src}/libusb/strerror.c
	${_usb_src}/libusb/sync.c
	${_usb_os})
target_include_directories(dmodem_libusb PRIVATE ${_usb_cfg}
	${_usb_src}/libusb/os)
target_include_directories(dmodem_libusb SYSTEM PUBLIC ${_usb_src}/libusb)
# (its own code, as its own builds compile it: no warnings of ours, and
# windows.h's min and max, which the project's NOMINMAX takes away)
if(MSVC)
	target_compile_options(dmodem_libusb PRIVATE /W0 /UNOMINMAX)
else()
	target_compile_options(dmodem_libusb PRIVATE -w -UNOMINMAX)
endif()
# (nor the project's POSIX for MSVC, compat/msvc/dmodem_msvc.h, forced
# into every file: libusb has its own Windows code)
get_target_property(_usb_opts dmodem_libusb COMPILE_OPTIONS)
if(_usb_opts)
	list(FILTER _usb_opts EXCLUDE REGEX "^/FI")
	set_target_properties(dmodem_libusb PROPERTIES COMPILE_OPTIONS
		"${_usb_opts}")
endif()
set_target_properties(dmodem_libusb PROPERTIES C_STANDARD 11)
target_link_libraries(dmodem_libusb PUBLIC Threads::Threads)
# (macOS: IOKit's USB, as libusb's configure links it)
if(APPLE)
	target_link_libraries(dmodem_libusb PUBLIC objc "-framework IOKit"
		"-framework CoreFoundation" "-framework Security")
endif()
