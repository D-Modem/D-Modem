# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# clang-cl.cmake: a toolchain file for Windows (x86_64) with LLVM's
# clang-cl and lld-link, MSVC's ABI and C library, from Linux:
#
#   xwin --accept-license splat --output ~/.xwin
#   cmake -B build-msvc -G Ninja -DCMAKE_TOOLCHAIN_FILE=platforms/windows/clang-cl.cmake
#
# xwin (github.com/Jake-Shadle/xwin) downloads Microsoft's C runtime and
# Windows SDK headers and libraries (its --accept-license is Microsoft's
# Visual Studio and Windows SDK license); XWIN_DIR is where they are
# (default ~/.xwin).  The build is MSVC's: the same headers, C runtime
# (static, /MT) and ABI as cl.exe, so it stands in for it on Linux; on
# Windows the toolchain file isn't needed (a Developer Command Prompt,
# cmake -G Ninja).  The tests run through Wine where it is installed.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT XWIN_DIR)
	if(DEFINED ENV{XWIN_DIR})
		set(XWIN_DIR $ENV{XWIN_DIR})
	else()
		set(XWIN_DIR $ENV{HOME}/.xwin)
	endif()
endif()
set(XWIN_DIR ${XWIN_DIR} CACHE PATH "xwin's splat: MSVC's CRT, the SDK")
if(NOT EXISTS ${XWIN_DIR}/crt/include)
	message(FATAL_ERROR "clang-cl.cmake: no MSVC CRT in ${XWIN_DIR} "
		"(xwin --accept-license splat --output ${XWIN_DIR})")
endif()
# (for the projects built with this file, libre's and Mbed TLS's)
set(ENV{XWIN_DIR} ${XWIN_DIR})

# the LLVM tools, the newest first (MSVC's C++ library wants Clang 19
# or later)
foreach(_t clang-cl lld-link llvm-lib llvm-rc llvm-mt)
	string(REPLACE "-" "_" _v ${_t})
	find_program(_${_v} NAMES ${_t} ${_t}-20 ${_t}-19 ${_t}-18 ${_t}-17
		HINTS /usr/lib/llvm-20/bin /usr/lib/llvm-19/bin
		/usr/lib/llvm-18/bin /usr/lib/llvm-17/bin)
	if(NOT _${_v})
		message(FATAL_ERROR "clang-cl.cmake: no ${_t}")
	endif()
endforeach()
set(CMAKE_C_COMPILER ${_clang_cl})
set(CMAKE_CXX_COMPILER ${_clang_cl})
set(CMAKE_LINKER ${_lld_link})
set(CMAKE_AR ${_llvm_lib})
set(CMAKE_RC_COMPILER ${_llvm_rc})
set(CMAKE_MT ${_llvm_mt})
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# MSVC's headers and libraries from xwin
set(_inc "/imsvc ${XWIN_DIR}/crt/include")
foreach(_d ucrt um shared winrt)
	string(APPEND _inc " /imsvc ${XWIN_DIR}/sdk/include/${_d}")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_inc}")
set(CMAKE_CXX_FLAGS_INIT "${_inc}")
set(CMAKE_RC_FLAGS_INIT "/I ${XWIN_DIR}/sdk/include/um /I ${XWIN_DIR}/sdk/include/shared")
set(_lib "/libpath:${XWIN_DIR}/crt/lib/x86_64")
foreach(_d um ucrt)
	string(APPEND _lib " /libpath:${XWIN_DIR}/sdk/lib/${_d}/x86_64")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lib}")

# (CMakeLists.txt asks for the static C runtime, /MT: the policy for it
# in projects that don't ask for CMP0091 themselves)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
# (xwin leaves the debug C runtime out unless --include-debug-libs: the
# compiler's checks with the release one; a Debug build needs it)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(CMAKE_FIND_ROOT_PATH ${XWIN_DIR})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} ${XWIN_DIR}/pkgconfig-none)
set(ENV{PKG_CONFIG_PATH} "")

find_program(_wine NAMES wine64 wine)
if(_wine)
	set(CMAKE_CROSSCOMPILING_EMULATOR ${_wine})
endif()
