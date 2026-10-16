# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# libre (baresip's re) for dmodemd's SIP user agent, as the imported
# target `libre`.
#
# An installed libre is found with pkg-config (libre, this release or
# later).  Otherwise (DMODEM_LIBRE=auto or fetch) the pinned release is
# cloned and built once while configuring, into the build tree: its SIP
# and the parts that it needs, a static library.  Its digests (SIP's
# authentication) are OpenSSL's.
set(DMODEM_LIBRE_REPO "https://github.com/baresip/re.git"
	CACHE STRING "libre repository to fetch")
set(DMODEM_LIBRE_TAG "v4.12.0" CACHE STRING "libre release to fetch")
set(_re_min 4.12.0)

find_package(PkgConfig REQUIRED)

set(_re_prefix ${CMAKE_BINARY_DIR}/_deps/libre-install)
set(_re_src ${CMAKE_BINARY_DIR}/_deps/libre-src)
set(_re_build ${CMAKE_BINARY_DIR}/_deps/libre-build)
set(_re_log ${CMAKE_BINARY_DIR}/_deps/libre-build.log)

if(NOT DMODEM_LIBRE STREQUAL "fetch")
	pkg_check_modules(LIBRE QUIET libre>=${_re_min})
endif()

if(NOT LIBRE_FOUND)
	if(DMODEM_LIBRE STREQUAL "system")
		message(FATAL_ERROR "libre ${_re_min} or later (pkg-config "
			"libre) not found; install it or use "
			"-DDMODEM_LIBRE=fetch")
	endif()
	if(NOT EXISTS ${_re_prefix}/lib/pkgconfig/libre.pc)
		find_package(Git REQUIRED)
		message(STATUS "libre: fetching ${DMODEM_LIBRE_TAG} from "
			"${DMODEM_LIBRE_REPO} (once)")
		if(NOT EXISTS ${_re_src}/CMakeLists.txt)
			file(REMOVE_RECURSE ${_re_src})
			execute_process(COMMAND ${GIT_EXECUTABLE} clone --depth 1
				--branch ${DMODEM_LIBRE_TAG}
				${DMODEM_LIBRE_REPO} ${_re_src}
				RESULT_VARIABLE _r)
			if(_r)
				message(FATAL_ERROR "libre: git clone failed")
			endif()
		endif()
		include(ProcessorCount)
		ProcessorCount(_jobs)
		if(_jobs EQUAL 0)
			set(_jobs 1)
		endif()
		file(REMOVE_RECURSE ${_re_build})
		set(_steps configure build install)
		set(_configure ${CMAKE_COMMAND} -S ${_re_src} -B ${_re_build}
			-DCMAKE_BUILD_TYPE=Release
			-DCMAKE_INSTALL_PREFIX=${_re_prefix}
			-DCMAKE_INSTALL_LIBDIR=lib
			-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
			-DLIBRE_BUILD_SHARED=OFF -DLIBRE_BUILD_STATIC=ON
			-DUSE_OPENSSL=ON -DUSE_REM=OFF -DUSE_BFCP=OFF
			-DUSE_PCP=OFF -DUSE_RTMP=OFF
			-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON)
		set(_build ${CMAKE_COMMAND} --build ${_re_build}
			--parallel ${_jobs})
		set(_install ${CMAKE_COMMAND} --install ${_re_build})
		foreach(_step ${_steps})
			message(STATUS "libre: ${_step}")
			execute_process(COMMAND ${_${_step}}
				OUTPUT_FILE ${_re_log} ERROR_FILE ${_re_log}
				RESULT_VARIABLE _r)
			if(_r)
				message(FATAL_ERROR "libre: ${_step} failed, see "
					"${_re_log}")
			endif()
		endforeach()
	endif()
	set(ENV{PKG_CONFIG_PATH}
		"${_re_prefix}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
	pkg_check_modules(LIBRE REQUIRED libre>=${_re_min})
	message(STATUS "libre: ${LIBRE_VERSION} built in ${_re_prefix}")
else()
	message(STATUS "libre: ${LIBRE_VERSION} (installed)")
endif()

# dmodemd links libre statically when it can: the private libraries too
add_library(libre INTERFACE IMPORTED)
target_compile_options(libre INTERFACE ${LIBRE_STATIC_CFLAGS})
target_link_libraries(libre INTERFACE ${LIBRE_STATIC_LDFLAGS})
