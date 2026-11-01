# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# mingw-w64.cmake: a toolchain file for Windows (x86_64) with MinGW-w64,
# its POSIX thread model (winpthreads), from Linux:
#
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=platforms/windows/mingw-w64.cmake
#
# The compiler is Clang (--target=<triple>, linking with lld) where Clang
# and lld are installed, else MinGW-w64's GCC; MINGW_COMPILER=gcc asks for
# GCC.  Clang uses MinGW-w64's GCC installation all the same: its headers,
# C runtime, libstdc++ and winpthreads (Clang finds it by itself), so the
# GCC package is needed either way.
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
# Clang: the unversioned one, else the newest there is, with lld; Clang
# only if GCC's installation is there (its libgcc) for the rest
if(NOT MINGW_COMPILER)
	set(MINGW_COMPILER clang)
endif()
set(MINGW_COMPILER ${MINGW_COMPILER} CACHE STRING "clang or gcc")
set(_clang "")
if(MINGW_COMPILER STREQUAL "clang" AND _cc)
	foreach(_v "" -22 -21 -20 -19 -18 -17 -16)
		find_program(_c clang${_v})
		find_program(_cx clang++${_v})
		find_program(_lld ld.lld${_v} ld.lld)
		if(_c AND _cx AND _lld)
			set(_clang ${_c})
			set(_clangxx ${_cx})
		endif()
		unset(_c CACHE)
		unset(_cx CACHE)
		unset(_lld CACHE)
		if(_clang)
			break()
		endif()
	endforeach()
	execute_process(COMMAND ${_cc} -print-libgcc-file-name
		OUTPUT_VARIABLE _libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)
	get_filename_component(_gcc_dir "${_libgcc}" DIRECTORY)
	if(NOT EXISTS "${_gcc_dir}/libgcc.a")
		set(_clang "")
	endif()
endif()
if(_clang)
	set(CMAKE_C_COMPILER ${_clang})
	set(CMAKE_CXX_COMPILER ${_clangxx})
	set(CMAKE_C_COMPILER_TARGET ${MINGW_TRIPLE})
	set(CMAKE_CXX_COMPILER_TARGET ${MINGW_TRIPLE})
	# (thread-locals emulated as MinGW-w64's GCC built libstdc++ with
	# them: std::call_once's are libstdc++'s)
	set(CMAKE_C_FLAGS_INIT "-femulated-tls")
	set(CMAKE_CXX_FLAGS_INIT "-femulated-tls")
	# (winpthreads: libstdc++'s std::thread is on it)
	set(_mingw_link "-fuse-ld=lld -pthread")
else()
	set(MINGW_COMPILER gcc CACHE STRING "clang or gcc" FORCE)
	set(CMAKE_C_COMPILER ${_cc})
	set(CMAKE_CXX_COMPILER ${_cxx})
	set(_mingw_link "")
endif()
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
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static ${_mingw_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ ${_mingw_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ ${_mingw_link}")
find_program(_wine NAMES wine64 wine)
if(_wine)
	set(CMAKE_CROSSCOMPILING_EMULATOR ${_wine})
endif()
