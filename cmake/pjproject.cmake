# pjproject for d-modem, as the imported target `pjproject`.
#
# An installed pjproject is found with pkg-config (libpjproject).  Otherwise
# (DMODEM_PJPROJECT=auto or fetch) the pinned release is cloned and built
# once while configuring, into the build tree, with the options d-modem
# needs.  Its static libraries carry the target triplet in their names, so
# they are linked through the .pc file it installs.
set(DMODEM_PJPROJECT_REPO "https://github.com/pjsip/pjproject.git"
	CACHE STRING "pjproject repository to fetch")
set(DMODEM_PJPROJECT_TAG "2.17" CACHE STRING "pjproject release to fetch")

find_package(PkgConfig REQUIRED)

set(_pj_prefix ${CMAKE_BINARY_DIR}/_deps/pjproject-install)
set(_pj_src ${CMAKE_BINARY_DIR}/_deps/pjproject-src)

if(NOT DMODEM_PJPROJECT STREQUAL "fetch")
	pkg_check_modules(PJPROJECT QUIET libpjproject)
endif()

if(NOT PJPROJECT_FOUND)
	if(DMODEM_PJPROJECT STREQUAL "system")
		message(FATAL_ERROR "pjproject (pkg-config libpjproject) not found; "
			"install it or use -DDMODEM_PJPROJECT=fetch")
	endif()
	if(NOT EXISTS ${_pj_prefix}/lib/pkgconfig/libpjproject.pc)
		find_package(Git REQUIRED)
		message(STATUS "pjproject: fetching ${DMODEM_PJPROJECT_TAG} from "
			"${DMODEM_PJPROJECT_REPO} (once, a few minutes)")
		if(NOT EXISTS ${_pj_src}/configure)
			file(REMOVE_RECURSE ${_pj_src})
			execute_process(COMMAND ${GIT_EXECUTABLE} clone --depth 1
				--branch ${DMODEM_PJPROJECT_TAG}
				${DMODEM_PJPROJECT_REPO} ${_pj_src}
				RESULT_VARIABLE _r)
			if(_r)
				message(FATAL_ERROR "pjproject: git clone failed")
			endif()
		endif()
		include(ProcessorCount)
		ProcessorCount(_jobs)
		if(_jobs EQUAL 0)
			set(_jobs 1)
		endif()
		foreach(_step
			"./configure --prefix=${_pj_prefix} --disable-video"
			"make -j${_jobs} dep"
			"make -j${_jobs}"
			"make install")
			message(STATUS "pjproject: ${_step}")
			separate_arguments(_cmd UNIX_COMMAND "${_step}")
			execute_process(COMMAND ${_cmd} WORKING_DIRECTORY ${_pj_src}
				OUTPUT_FILE ${CMAKE_BINARY_DIR}/_deps/pjproject-build.log
				ERROR_FILE ${CMAKE_BINARY_DIR}/_deps/pjproject-build.log
				RESULT_VARIABLE _r)
			if(_r)
				message(FATAL_ERROR "pjproject: '${_step}' failed, see "
					"${CMAKE_BINARY_DIR}/_deps/pjproject-build.log")
			endif()
		endforeach()
	endif()
	set(ENV{PKG_CONFIG_PATH}
		"${_pj_prefix}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
	pkg_check_modules(PJPROJECT REQUIRED libpjproject)
	message(STATUS "pjproject: ${PJPROJECT_VERSION} built in ${_pj_prefix}")
else()
	message(STATUS "pjproject: ${PJPROJECT_VERSION} (installed)")
endif()

# d-modem links pjproject statically: the private libraries too
add_library(pjproject INTERFACE IMPORTED)
target_compile_options(pjproject INTERFACE ${PJPROJECT_STATIC_CFLAGS})
# (as link libraries: after the objects, in the order the .pc gives)
target_link_libraries(pjproject INTERFACE ${PJPROJECT_STATIC_LDFLAGS})
