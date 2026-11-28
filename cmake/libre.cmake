# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# libre (baresip's re) for dmodemd's SIP user agent, as the imported
# target `libre`.
#
# An installed libre is found with pkg-config (libre, this release or
# later).  Otherwise (DMODEM_LIBRE=auto or fetch, and always when cross
# compiling unless DMODEM_LIBRE=system) the pinned release is cloned and
# built once while configuring, into the build tree, with this build's
# compiler and toolchain file: its SIP and the parts that it needs, a
# static library, linked by its path (no pkg-config needed then).  Its
# digests (SIP's authentication) and AES (SRTP) are DMODEM_CRYPTO's:
# Mbed TLS's crypto library, fetched and built the same way (the
# default), or the system's OpenSSL.  libre has no AES of its own on
# Mbed TLS: cmake/libre-mbedtls-aes.patch adds it.
set(DMODEM_LIBRE_REPO "https://github.com/baresip/re.git"
	CACHE STRING "libre repository to fetch")
set(DMODEM_LIBRE_TAG "v4.12.0" CACHE STRING "libre release to fetch")
set(DMODEM_MBEDTLS_REPO "https://github.com/Mbed-TLS/mbedtls.git"
	CACHE STRING "Mbed TLS repository to fetch")
set(DMODEM_MBEDTLS_TAG "v3.6.7" CACHE STRING "Mbed TLS release to fetch")
set(_re_min 4.12.0)

set(_deps ${CMAKE_BINARY_DIR}/_deps)
set(_re_prefix ${_deps}/libre-install)
set(_re_src ${_deps}/libre-src)
set(_re_build ${_deps}/libre-build)
set(_re_log ${_deps}/libre-build.log)
set(_tls_src ${_deps}/mbedtls-src)
set(_tls_build ${_deps}/mbedtls-build)
set(_tls_log ${_deps}/mbedtls-build.log)

if(NOT DMODEM_CRYPTO MATCHES "^(openssl|mbedtls)$")
	message(FATAL_ERROR "DMODEM_CRYPTO: openssl or mbedtls, not "
		"`${DMODEM_CRYPTO}'")
endif()

# an installed one: not for another system than this one, unless asked
if(DMODEM_LIBRE STREQUAL "system" OR
   (DMODEM_LIBRE STREQUAL "auto" AND NOT CMAKE_CROSSCOMPILING))
	# (auto: none without pkg-config, as on MSVC's machines)
	if(DMODEM_LIBRE STREQUAL "system")
		find_package(PkgConfig REQUIRED)
	else()
		find_package(PkgConfig QUIET)
	endif()
	if(PKG_CONFIG_FOUND)
		pkg_check_modules(LIBRE QUIET libre>=${_re_min})
	endif()
	if(NOT LIBRE_FOUND AND DMODEM_LIBRE STREQUAL "system")
		message(FATAL_ERROR "libre ${_re_min} or later (pkg-config "
			"libre) not found; install it or use "
			"-DDMODEM_LIBRE=fetch")
	endif()
endif()

if(LIBRE_FOUND)
	message(STATUS "libre: ${LIBRE_VERSION} (installed)")
	# dmodemd links libre statically when it can: the private
	# libraries too
	add_library(libre INTERFACE IMPORTED)
	target_compile_options(libre INTERFACE ${LIBRE_STATIC_CFLAGS})
	target_link_libraries(libre INTERFACE ${LIBRE_STATIC_LDFLAGS})
	return()
endif()

# what a build of a fetched project gets of this one: the compilers, and
# the cross compiling toolchain and its settings
set(_cross -DCMAKE_BUILD_TYPE=Release
	-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
	-DCMAKE_INSTALL_PREFIX=${_re_prefix}
	-DCMAKE_INSTALL_LIBDIR=lib)
if(CMAKE_GENERATOR)
	list(APPEND _cross -G ${CMAKE_GENERATOR})
endif()
if(CMAKE_MAKE_PROGRAM)
	list(APPEND _cross -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
endif()
# (MSVC: this build's C runtime, the static one, in theirs too)
if(MSVC)
	list(APPEND _cross -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
		"-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()
# (Apple's: the SDK, iphoneos or iphonesimulator, and the oldest
# system the build runs on)
# (MinGW-w64: the compiler and target this build's toolchain file took;
# Haiku: its packages, extracted, and cmake/rules.cmake's -fPIC)
foreach(_v CMAKE_TOOLCHAIN_FILE CMAKE_USER_MAKE_RULES_OVERRIDE MINGW_TRIPLE MINGW_COMPILER CLANG_DOS CLANG_RETRO_CONFIG HAIKU_SYSROOT HAIKU_TRIPLE
	CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION
	CMAKE_SYSROOT CMAKE_OSX_ARCHITECTURES CMAKE_OSX_SYSROOT
	CMAKE_OSX_DEPLOYMENT_TARGET ANDROID_ABI ANDROID_PLATFORM
	ANDROID_NDK ANDROID_STL)
	if(DEFINED ${_v} AND NOT "${${_v}}" STREQUAL "")
		if("${${_v}}" MATCHES ";")
			# (a list, "arm64;x86_64": in an initial cache file, as a
			# ; in an argument does not survive being passed on)
			string(APPEND _cross_cache
				"set(${_v} \"${${_v}}\" CACHE STRING \"\")\n")
		else()
			list(APPEND _cross "-D${_v}=${${_v}}")
		endif()
	endif()
endforeach()
if(_cross_cache)
	file(WRITE ${CMAKE_BINARY_DIR}/libre-cross.cmake "${_cross_cache}")
	list(APPEND _cross -C ${CMAKE_BINARY_DIR}/libre-cross.cmake)
endif()

include(ProcessorCount)
ProcessorCount(_jobs)
if(_jobs EQUAL 0)
	set(_jobs 1)
endif()
find_package(Git REQUIRED)

# dmodem_fetch_build(<name> <repo> <tag> <src> <build> <log> <args>...):
# <repo> at <tag> cloned into <src> (with its submodules), then
# configured into <build> with <args>, built and installed
function(dmodem_fetch_build name repo tag src build log)
	message(STATUS "${name}: fetching ${tag} from ${repo} (once)")
	if(NOT EXISTS ${src}/CMakeLists.txt)
		file(REMOVE_RECURSE ${src})
		execute_process(COMMAND ${GIT_EXECUTABLE} clone --depth 1
			--branch ${tag} --recurse-submodules
			--shallow-submodules ${repo} ${src}
			RESULT_VARIABLE _r)
		if(_r)
			message(FATAL_ERROR "${name}: git clone failed")
		endif()
	endif()
	# (<name>_PATCHES: patches for this build, each applied once)
	foreach(_patch ${${name}_PATCHES})
		execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check
			${_patch} WORKING_DIRECTORY ${src}
			RESULT_VARIABLE _applied OUTPUT_QUIET ERROR_QUIET)
		if(_applied)
			execute_process(COMMAND ${GIT_EXECUTABLE} apply
				${_patch} WORKING_DIRECTORY ${src}
				RESULT_VARIABLE _r)
			if(_r)
				message(FATAL_ERROR "${name}: ${_patch} "
					"does not apply")
			endif()
		endif()
	endforeach()
	file(REMOVE_RECURSE ${build})
	set(_configure ${CMAKE_COMMAND} -S ${src} -B ${build} ${ARGN})
	# (--config: a multi-config generator's, Visual Studio's, Release)
	set(_build ${CMAKE_COMMAND} --build ${build} --config Release
		--parallel ${_jobs})
	set(_install ${CMAKE_COMMAND} --install ${build} --config Release)
	foreach(_step configure build install)
		message(STATUS "${name}: ${_step}")
		execute_process(COMMAND ${_${_step}}
			OUTPUT_FILE ${log} ERROR_FILE ${log}
			RESULT_VARIABLE _r)
		if(_r)
			message(FATAL_ERROR "${name}: ${_step} failed, see "
				"${log}")
		endif()
	endforeach()
endfunction()

# the digests: Mbed TLS's crypto library into the same prefix, or the
# system's OpenSSL
if(DMODEM_CRYPTO STREQUAL "mbedtls")
	# (Windows before Vista: cmake/mbedtls-win32-legacy.h, no bcrypt.dll
	# and no vsnprintf_s())
	set(_tls_legacy "")
	if(WIN32 AND DMODEM_WINNT)
		math(EXPR _winnt "${DMODEM_WINNT}")
		if(_winnt LESS 1536)
			set(_tls_legacy "-DMBEDTLS_USER_CONFIG_FILE=${CMAKE_CURRENT_LIST_DIR}/mbedtls-win32-legacy.h")
		endif()
	endif()
	# (the classic Mac OS: cmake/mbedtls-macos9.h, no entropy from the
	# platform)
	if(CMAKE_SYSTEM_NAME STREQUAL "MacOS9")
		set(_tls_legacy "-DMBEDTLS_USER_CONFIG_FILE=${CMAKE_CURRENT_LIST_DIR}/mbedtls-macos9.h")
	endif()
	set(_tls_lib ${_re_prefix}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mbedcrypto${CMAKE_STATIC_LIBRARY_SUFFIX})
	if(NOT EXISTS ${_tls_lib})
		dmodem_fetch_build(mbedtls ${DMODEM_MBEDTLS_REPO}
			${DMODEM_MBEDTLS_TAG} ${_tls_src} ${_tls_build}
			${_tls_log} ${_cross} -DENABLE_PROGRAMS=OFF
			-DENABLE_TESTING=OFF
			# (its warnings its own: Clang for MinGW-w64 ignores
			# its gnu_printf format attribute, and says so)
			-DMBEDTLS_FATAL_WARNINGS=OFF ${_tls_legacy})
	endif()
	set(_re_crypto -DUSE_MBEDTLS=ON -DUSE_OPENSSL=OFF
		-DMBEDTLS_HINTS=${_re_prefix}
		-DCMAKE_FIND_ROOT_PATH=${_re_prefix})
	set(_crypto_libs ${_tls_lib})
else()
	find_package(OpenSSL REQUIRED)
	set(_re_crypto -DUSE_OPENSSL=ON -DUSE_MBEDTLS=OFF)
	set(_crypto_libs OpenSSL::SSL OpenSSL::Crypto)
endif()

# (MSVC's re-static.lib: re.lib would be the DLL's import library)
if(MSVC)
	set(_re_lib ${_re_prefix}/lib/re-static.lib)
else()
	set(_re_lib ${_re_prefix}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}re${CMAKE_STATIC_LIBRARY_SUFFIX})
endif()
# (built with other options or patches before: again)
set(_re_stamp ${_re_prefix}/dmodem-${DMODEM_LIBRE_TAG}-${DMODEM_CRYPTO})
# libre's C flags: this build's (a toolchain's system headers among
# them), Mbed TLS's headers (libre doesn't add them to its own), and
# with MSVC no typed re_printf() arguments (RE_ARG), whose count of the
# arguments needs GNU's comma elision (C++, ours, never has them)
set(_re_cflags "${CMAKE_C_FLAGS}")
if(DMODEM_CRYPTO STREQUAL "mbedtls")
	string(APPEND _re_cflags " -I${_re_prefix}/include")
endif()
if(MSVC)
	string(APPEND _re_cflags " -DDISABLE_RE_ARG")
endif()
# (picolibc, clang-retro's win32-picolibc: cmake/libre-picolibc, the
# names of Microsoft's C library that libre uses on Windows)
if(WIN32 AND CLANG_RETRO_CONFIG MATCHES "picolibc")
	string(APPEND _re_cflags
		" -include ${CMAKE_CURRENT_LIST_DIR}/libre-picolibc/compat.h"
		" -I${CMAKE_CURRENT_LIST_DIR}/libre-picolibc")
	file(MD5 ${CMAKE_CURRENT_LIST_DIR}/libre-picolibc/compat.h _sum)
	string(SUBSTRING ${_sum} 0 8 _sum)
	set(_re_stamp ${_re_stamp}-picolibc-${_sum})
endif()
# (the classic Mac OS, D-Modem/gui's cmake/macos9.cmake:
# cmake/libre-macos9, the names of IPv6, multicast and the interfaces
# that the C library of clang-retro/mac-sdk does not have)
if(CMAKE_SYSTEM_NAME STREQUAL "MacOS9")
	string(APPEND _re_cflags
		" -include ${CMAKE_CURRENT_LIST_DIR}/libre-macos9/compat.h"
		" -I${CMAKE_CURRENT_LIST_DIR}/libre-macos9")
	foreach(_h compat.h net/if.h)
		file(MD5 ${CMAKE_CURRENT_LIST_DIR}/libre-macos9/${_h} _sum)
		string(SUBSTRING ${_sum} 0 8 _sum)
		set(_re_stamp ${_re_stamp}-${_sum})
	endforeach()
endif()
# (Haiku: platforms/haiku/libre-haiku.patch, for its interface ioctls
# and its lack of a routing table listing)
# (Mbed TLS: cmake/libre-mbedtls-aes.patch, its AES for SRTP)
set(libre_PATCHES)
if(DMODEM_CRYPTO STREQUAL "mbedtls")
	list(APPEND libre_PATCHES ${CMAKE_CURRENT_LIST_DIR}/libre-mbedtls-aes.patch)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
	list(APPEND libre_PATCHES ${CMAKE_CURRENT_LIST_DIR}/../platforms/haiku/libre-haiku.patch)
endif()
# (Windows: cmake/libre-win32-legacy.patch, for the Windows versions
# before 10 that DMODEM_WINNT asks for: no qWAVE before Vista, its own
# inet_pton(), condition variables and call_once() before Vista,
# getaddrinfo() from wspiapi.h before XP; Mbed TLS's digests, not
# CryptoAPI's SHA-256, which 9x, NT 4 and 2000 lack)
if(WIN32)
	list(APPEND libre_PATCHES ${CMAKE_CURRENT_LIST_DIR}/libre-win32-legacy.patch)
	set(_re_winnt -DRE_WIN32_WINNT=${DMODEM_WINNT})
	set(_re_stamp ${_re_stamp}-${DMODEM_WINNT})
endif()
# (Mac OS X 10.4 to 10.6, cmake/macosx-legacy.cmake of D-Modem/gui:
# cmake/libre-macosx-legacy.patch, Mbed TLS's digests, because
# CommonCrypto starts in 10.5; no arc4random_buf() before 10.7 and no
# pthread_setname_np() before 10.6)
if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET
		AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 10.7)
	if(NOT DMODEM_CRYPTO STREQUAL "mbedtls")
		message(FATAL_ERROR "Mac OS X before 10.7: DMODEM_CRYPTO=mbedtls")
	endif()
	list(APPEND libre_PATCHES ${CMAKE_CURRENT_LIST_DIR}/libre-macosx-legacy.patch)
endif()
# (the classic Mac OS: cmake/libre-macos9.patch, no routing table
# listing)
if(CMAKE_SYSTEM_NAME STREQUAL "MacOS9")
	list(APPEND libre_PATCHES ${CMAKE_CURRENT_LIST_DIR}/libre-macos9.patch)
endif()
# (picolibc, the Windows 95 build: cmake/libre-picolibc.patch,
# GetAdaptersInfo() found at run time, because Windows 95 has no
# iphlpapi.dll)
if(WIN32 AND CLANG_RETRO_CONFIG MATCHES "picolibc")
	list(APPEND libre_PATCHES ${CMAKE_CURRENT_LIST_DIR}/libre-picolibc.patch)
endif()
foreach(_patch ${libre_PATCHES})
	file(MD5 ${_patch} _sum)
	string(SUBSTRING ${_sum} 0 8 _sum)
	set(_re_stamp ${_re_stamp}-${_sum})
endforeach()
if(NOT EXISTS ${_re_lib} OR NOT EXISTS ${_re_stamp})
	dmodem_fetch_build(libre ${DMODEM_LIBRE_REPO} ${DMODEM_LIBRE_TAG}
		${_re_src} ${_re_build} ${_re_log} ${_cross} ${_re_crypto}
		"-DCMAKE_C_FLAGS=${_re_cflags}"
		-DLIBRE_BUILD_SHARED=OFF -DLIBRE_BUILD_STATIC=ON ${_re_winnt}
		-DUSE_REM=OFF -DUSE_BFCP=OFF -DUSE_PCP=OFF -DUSE_RTMP=OFF
		-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON)
	file(TOUCH ${_re_stamp})
endif()
message(STATUS "libre: ${DMODEM_LIBRE_TAG} (${DMODEM_CRYPTO}) built in "
	"${_re_prefix}")

# the static library by its path, with what it needs: the digests, the
# threads, and the libraries its .pc names (libm, libdl)
add_library(libre INTERFACE IMPORTED)
target_include_directories(libre INTERFACE ${_re_prefix}/include/re)
target_link_libraries(libre INTERFACE ${_re_lib} ${_crypto_libs}
	Threads::Threads ${DMODEM_LIBM} ${CMAKE_DL_LIBS})
# (Apple's systems: its DNS servers from SystemConfiguration, as libre's
# own .pc says, src/dns/darwin/srv.c, and resolv.conf's through libresolv's
# res_ninit(), src/dns/res.c)
if(APPLE)
	target_link_libraries(libre INTERFACE "-framework SystemConfiguration"
		"-framework CoreFoundation" resolv)
endif()
# (Haiku: the sockets and getifaddrs() in libnetwork)
if(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
	target_link_libraries(libre INTERFACE network)
endif()
if(WIN32)
	# (its headers choose Winsock's by WIN32; the system libraries its
	# .pc and Mbed TLS's name)
	target_compile_definitions(libre INTERFACE WIN32)
	target_link_libraries(libre INTERFACE qwave iphlpapi wsock32 ws2_32
		dbghelp bcrypt)
endif()
