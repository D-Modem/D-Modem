# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# libre (baresip's re) for dmodemd's SIP user agent, as the imported
# target `libre`.
#
# An installed libre is found with pkg-config (libre, this release or
# later).  Otherwise (DMODEM_LIBRE=auto or fetch, and always when cross
# compiling unless DMODEM_LIBRE=system) the pinned release is cloned and
# built once while configuring, into the build tree, with this build's
# compiler and toolchain file: its SIP and the parts that it needs, a
# static library, linked by its path (no pkg-config needed then).  Its
# digests (SIP's authentication) are DMODEM_CRYPTO's: the system's
# OpenSSL, or Mbed TLS, fetched and built the same way (Android).
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
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(LIBRE QUIET libre>=${_re_min})
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
# (Apple's: the SDK, iphoneos or iphonesimulator, and the oldest
# system the build runs on)
foreach(_v CMAKE_TOOLCHAIN_FILE CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION
	CMAKE_SYSROOT CMAKE_OSX_ARCHITECTURES CMAKE_OSX_SYSROOT
	CMAKE_OSX_DEPLOYMENT_TARGET ANDROID_ABI ANDROID_PLATFORM
	ANDROID_NDK ANDROID_STL)
	if(DEFINED ${_v} AND NOT "${${_v}}" STREQUAL "")
		list(APPEND _cross -D${_v}=${${_v}})
	endif()
endforeach()

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
	file(REMOVE_RECURSE ${build})
	set(_configure ${CMAKE_COMMAND} -S ${src} -B ${build} ${ARGN})
	set(_build ${CMAKE_COMMAND} --build ${build} --parallel ${_jobs})
	set(_install ${CMAKE_COMMAND} --install ${build})
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
	set(_tls_lib ${_re_prefix}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mbedcrypto${CMAKE_STATIC_LIBRARY_SUFFIX})
	if(NOT EXISTS ${_tls_lib})
		dmodem_fetch_build(mbedtls ${DMODEM_MBEDTLS_REPO}
			${DMODEM_MBEDTLS_TAG} ${_tls_src} ${_tls_build}
			${_tls_log} ${_cross} -DENABLE_PROGRAMS=OFF
			-DENABLE_TESTING=OFF)
	endif()
	set(_re_crypto -DUSE_MBEDTLS=ON -DUSE_OPENSSL=OFF
		-DMBEDTLS_HINTS=${_re_prefix}
		-DCMAKE_FIND_ROOT_PATH=${_re_prefix}
		# (libre doesn't add Mbed TLS's headers to its own)
		-DCMAKE_C_FLAGS=-I${_re_prefix}/include)
	set(_crypto_libs ${_tls_lib})
else()
	find_package(OpenSSL REQUIRED)
	set(_re_crypto -DUSE_OPENSSL=ON -DUSE_MBEDTLS=OFF)
	set(_crypto_libs OpenSSL::SSL OpenSSL::Crypto)
endif()

set(_re_lib ${_re_prefix}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}re${CMAKE_STATIC_LIBRARY_SUFFIX})
# (built with other options before: again)
set(_re_stamp ${_re_prefix}/dmodem-${DMODEM_LIBRE_TAG}-${DMODEM_CRYPTO})
if(NOT EXISTS ${_re_lib} OR NOT EXISTS ${_re_stamp})
	dmodem_fetch_build(libre ${DMODEM_LIBRE_REPO} ${DMODEM_LIBRE_TAG}
		${_re_src} ${_re_build} ${_re_log} ${_cross} ${_re_crypto}
		-DLIBRE_BUILD_SHARED=OFF -DLIBRE_BUILD_STATIC=ON
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
	Threads::Threads m ${CMAKE_DL_LIBS})
if(WIN32)
	# (its headers choose Winsock's by WIN32; the system libraries its
	# .pc and Mbed TLS's name)
	target_compile_definitions(libre INTERFACE WIN32)
	target_link_libraries(libre INTERFACE qwave iphlpapi wsock32 ws2_32
		dbghelp bcrypt)
endif()
