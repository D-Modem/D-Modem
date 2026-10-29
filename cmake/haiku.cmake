# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# haiku.cmake: a toolchain file for Haiku (x86_64) from Linux, with Clang
# and lld:
#
#   cmake -B build-haiku -DCMAKE_TOOLCHAIN_FILE=cmake/haiku.cmake \
#         -DHAIKU_SYSROOT=/path/to/haiku-sysroot
#
# HAIKU_SYSROOT (or the environment's HAIKU_SYSROOT) is a directory with
# Haiku's packages extracted under boot/system: haiku, haiku_devel, gcc,
# gcc_syslibs, gcc_syslibs_devel (libstdc++), and for --usb libusb and
# libusb_devel, for libre's digests openssl3 and openssl3_devel.
# cmake/haiku_sysroot.py makes one from Haiku's package repositories.
#
# On Haiku itself no toolchain file is needed: cmake -B build there.
# The programs built here run on Haiku only; the tests are built but not
# run (ctest runs them on Haiku).
set(CMAKE_SYSTEM_NAME Haiku)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT HAIKU_TRIPLE)
	set(HAIKU_TRIPLE x86_64-unknown-haiku)
endif()
if(NOT HAIKU_SYSROOT)
	set(HAIKU_SYSROOT "$ENV{HAIKU_SYSROOT}")
endif()
if(NOT HAIKU_SYSROOT OR NOT EXISTS "${HAIKU_SYSROOT}/boot/system/develop/headers/posix")
	message(FATAL_ERROR "HAIKU_SYSROOT: a directory with Haiku's packages "
		"extracted under boot/system (cmake/haiku_sysroot.py makes "
		"one), not `${HAIKU_SYSROOT}'")
endif()
set(HAIKU_SYSROOT "${HAIKU_SYSROOT}" CACHE PATH "Haiku's packages, extracted")
set(HAIKU_TRIPLE "${HAIKU_TRIPLE}" CACHE STRING "Haiku's target triple")
# (the toolchain file runs again for each try_compile: pass these on)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES HAIKU_SYSROOT HAIKU_TRIPLE)

# Clang: the unversioned one, else the newest there is, with lld
foreach(_v "" -22 -21 -20 -19 -18 -17)
	find_program(_c clang${_v})
	find_program(_cx clang++${_v})
	find_program(_lld ld.lld${_v} ld.lld)
	if(_c AND _cx AND _lld)
		set(CMAKE_C_COMPILER ${_c})
		set(CMAKE_CXX_COMPILER ${_cx})
	endif()
	unset(_c CACHE)
	unset(_cx CACHE)
	unset(_lld CACHE)
	if(CMAKE_C_COMPILER)
		break()
	endif()
endforeach()
if(NOT CMAKE_C_COMPILER)
	message(FATAL_ERROR "Haiku: Clang and lld (17 or later) not found")
endif()
set(CMAKE_C_COMPILER_TARGET ${HAIKU_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${HAIKU_TRIPLE})
set(CMAKE_SYSROOT ${HAIKU_SYSROOT})
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_FIND_ROOT_PATH ${HAIKU_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# pkg-config: the target's packages, not the build host's
set(ENV{PKG_CONFIG_LIBDIR} ${HAIKU_SYSROOT}/boot/system/develop/lib/pkgconfig)
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${HAIKU_SYSROOT})
