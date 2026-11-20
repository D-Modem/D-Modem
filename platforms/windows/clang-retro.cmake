# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# clang-retro.cmake: a toolchain file for Windows with clang-retro's
# toolchain (the clang-dos package of github.com/clang-retro/dos-sdk),
# from Linux or Windows:
#
#   packaging/get-toolchain.sh /opt/clang-dos      (in a dos-sdk checkout)
#   cmake -B build-win -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=platforms/windows/clang-retro.cmake \
#       -DCLANG_RETRO_CONFIG=win98
#
# CLANG_DOS (a CMake or environment variable) is the unpacked package
# (default /opt/clang-dos); CI gets it with dos-sdk's setup-toolchain
# action.  CLANG_RETRO_CONFIG is the target: the name of one of the
# package's bin/<name>.cfg files, which give the triple, the sysroot
# (mingw-w64's headers, msvcrt.dll, libc++) and lld:
#
#   win98       Windows 98 and later, i486 (the default)
#   winme, win2000, winxp, win7, win10      32 bit
#   win7-x64, win10-x64                     64 bit
#
# The programs need only msvcrt.dll and the system's DLLs: libc++ and its
# threads (on Win32's own, not winpthreads) are linked in.  The oldest
# Windows of the target is _WIN32_WINNT (DMODEM_WINNT) too, so that
# windows.h declares nothing newer.
set(CMAKE_SYSTEM_NAME Windows)

if(NOT CLANG_DOS)
	if(DEFINED ENV{CLANG_DOS})
		set(CLANG_DOS $ENV{CLANG_DOS})
	else()
		set(CLANG_DOS /opt/clang-dos)
	endif()
endif()
set(CLANG_DOS ${CLANG_DOS} CACHE PATH "clang-retro's clang-dos package")
if(NOT CLANG_RETRO_CONFIG)
	set(CLANG_RETRO_CONFIG win98)
endif()
set(CLANG_RETRO_CONFIG ${CLANG_RETRO_CONFIG} CACHE STRING
	"clang-retro's target: bin/<name>.cfg (win98, winxp, win10-x64, ...)")
set(_cfg ${CLANG_DOS}/bin/${CLANG_RETRO_CONFIG}.cfg)
if(NOT EXISTS ${_cfg})
	message(FATAL_ERROR "clang-retro.cmake: no ${_cfg} (CLANG_DOS: "
		"clang-retro's clang-dos package, packaging/get-toolchain.sh)")
endif()
# (for the projects built with this file: libre's, Mbed TLS's, libusb's;
# and for CMake's own test builds)
set(ENV{CLANG_DOS} ${CLANG_DOS})
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CLANG_DOS CLANG_RETRO_CONFIG)

# the triple and the sysroot from the .cfg
file(STRINGS ${_cfg} _lines)
set(_triple "")
set(_sysroot "")
set(_cfg_winnt "")
foreach(_l ${_lines})
	if(_l MATCHES "^--target=(.*)$")
		set(_triple ${CMAKE_MATCH_1})
	elseif(_l MATCHES "^-D_WIN32_WINNT=(.*)$")
		set(_cfg_winnt ${CMAKE_MATCH_1})
	elseif(_l MATCHES "^--sysroot=<CFGDIR>/(.*)$")
		get_filename_component(_sysroot ${CLANG_DOS}/bin/${CMAKE_MATCH_1}
			ABSOLUTE)
	endif()
endforeach()
if(_triple MATCHES "^x86_64")
	set(CMAKE_SYSTEM_PROCESSOR AMD64)
else()
	set(CMAKE_SYSTEM_PROCESSOR x86)
endif()
# (the oldest Windows: the triple's windows<major>.<minor>)
if(_triple MATCHES "windows([0-9]+)\\.([0-9]+)")
	set(CMAKE_SYSTEM_VERSION ${CMAKE_MATCH_1}.${CMAKE_MATCH_2})
	# (Windows 9x and NT 4.0 are all 4.x: _WIN32_WINNT 0x0400)
	if(CMAKE_MATCH_1 EQUAL 4)
		set(_minor 0)
	else()
		set(_minor ${CMAKE_MATCH_2})
	endif()
	math(EXPR _winnt "${CMAKE_MATCH_1} * 256 + ${_minor}"
		OUTPUT_FORMAT HEXADECIMAL)
	string(REGEX REPLACE "^0x" "" _winnt ${_winnt})
	string(LENGTH ${_winnt} _n)
	while(_n LESS 4)
		set(_winnt 0${_winnt})
		math(EXPR _n "${_n} + 1")
	endwhile()
	set(CLANG_RETRO_WINNT 0x${_winnt})
	# (the .cfg's own, as it is written, where it has one: the same
	# macro twice, written otherwise, is a warning)
	if(_cfg_winnt)
		set(CLANG_RETRO_WINNT ${_cfg_winnt})
	endif()
	if(NOT DMODEM_WINNT)
		set(DMODEM_WINNT ${CLANG_RETRO_WINNT} CACHE STRING
			"_WIN32_WINNT: the oldest Windows to run on")
	endif()
endif()

set(_ext "")
if(CMAKE_HOST_WIN32)
	set(_ext .exe)
endif()
set(CMAKE_C_COMPILER ${CLANG_DOS}/bin/clang${_ext})
set(CMAKE_CXX_COMPILER ${CLANG_DOS}/bin/clang++${_ext})
set(CMAKE_RC_COMPILER ${CLANG_DOS}/bin/llvm-windres${_ext})
set(CMAKE_AR ${CLANG_DOS}/bin/llvm-ar${_ext} CACHE FILEPATH "")
set(CMAKE_RANLIB ${CLANG_DOS}/bin/llvm-ranlib${_ext} CACHE FILEPATH "")
# (dmodem_retro.h: clock_gettime(), which the sysroots lack)
set(_inc "-include ${CMAKE_CURRENT_LIST_DIR}/clang-retro/dmodem_retro.h")
set(CMAKE_C_FLAGS_INIT "--config=${_cfg}")
set(CMAKE_CXX_FLAGS_INIT "--config=${_cfg} ${_inc}")
# (windres: the target's headers, for windows.h in a .rc)
set(CMAKE_RC_FLAGS_INIT "--target=${_triple} -I${_sysroot}/include")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_SYSROOT ${_sysroot})
set(CMAKE_FIND_ROOT_PATH ${_sysroot})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} ${_sysroot}/lib/pkgconfig)
set(ENV{PKG_CONFIG_PATH} "")
find_program(_wine NAMES wine wine64)
if(_wine)
	set(CMAKE_CROSSCOMPILING_EMULATOR ${_wine})
endif()
