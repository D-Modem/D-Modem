# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# mingw-w64.cmake: a toolchain file for Windows (x86_64) with MinGW-w64's
# GCC, its POSIX thread model (winpthreads), from Linux:
#
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
#
# The tests then run through Wine where it is installed (ctest
# --test-dir build-win).  MINGW_TRIPLE picks another target
# (i686-w64-mingw32 for 32 bit Windows).
set(CMAKE_SYSTEM_NAME Windows)
if(NOT MINGW_TRIPLE)
	set(MINGW_TRIPLE x86_64-w64-mingw32)
endif()
set(CMAKE_SYSTEM_PROCESSOR ${CMAKE_HOST_SYSTEM_PROCESSOR})
if(MINGW_TRIPLE MATCHES "^i686")
	set(CMAKE_SYSTEM_PROCESSOR x86)
else()
	set(CMAKE_SYSTEM_PROCESSOR AMD64)
endif()
# (the -posix compilers: std::thread and pthreads through winpthreads)
foreach(_s -posix "")
	find_program(_cc ${MINGW_TRIPLE}-gcc${_s})
	find_program(_cxx ${MINGW_TRIPLE}-g++${_s})
	if(_cc AND _cxx)
		break()
	endif()
	unset(_cc CACHE)
	unset(_cxx CACHE)
endforeach()
set(CMAKE_C_COMPILER ${_cc})
set(CMAKE_CXX_COMPILER ${_cxx})
set(CMAKE_RC_COMPILER ${MINGW_TRIPLE}-windres)
set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# pkg-config: the target's packages, not the build host's
set(ENV{PKG_CONFIG_LIBDIR} ${CMAKE_FIND_ROOT_PATH}/lib/pkgconfig)
set(ENV{PKG_CONFIG_PATH} "")
# the programs self-contained: no libgcc, libstdc++ or winpthread DLLs
# to ship beside them
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
find_program(_wine NAMES wine64 wine)
if(_wine)
	set(CMAKE_CROSSCOMPILING_EMULATOR ${_wine})
endif()
