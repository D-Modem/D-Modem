# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# mod_dmodem.so: the dmodem applications for FreeSWITCH, built against an
# installed FreeSWITCH (its freeswitch.pc: pkg-config, or the
# libfreeswitch-dev package); skipped when it is missing.
#
#   -DFREESWITCH_PKGCONFIG=...     the directory of freeswitch.pc, when
#                                  pkg-config does not find it (a source
#                                  install: /usr/local/freeswitch/lib/pkgconfig)
#   -DFREESWITCH_MODULES_DIR=...   where `cmake --install` puts the module
#   -DFREESWITCH_CONF_DIR=...      where the sample dmodem.conf.xml goes

find_package(PkgConfig QUIET)
if(NOT PKG_CONFIG_FOUND)
	message(STATUS "mod_dmodem: no pkg-config, skipped")
	return()
endif()
# (cross compiling: not the build host's source install)
if(CMAKE_CROSSCOMPILING)
	set(_fs_pc "")
else()
	set(_fs_pc "/usr/local/freeswitch/lib/pkgconfig")
endif()
set(FREESWITCH_PKGCONFIG "${_fs_pc}" CACHE PATH
	"the directory of freeswitch.pc")
# only for this (FreeSWITCH's prefix may have its own spandsp, which the
# tests' must not find)
set(_pc_path "$ENV{PKG_CONFIG_PATH}")
set(ENV{PKG_CONFIG_PATH} "${FREESWITCH_PKGCONFIG}:${_pc_path}")
pkg_check_modules(FREESWITCH QUIET freeswitch)
if(FREESWITCH_FOUND)
	pkg_get_variable(_fs_modules freeswitch modulesdir)
	pkg_get_variable(_fs_conf freeswitch confdir)
endif()
set(ENV{PKG_CONFIG_PATH} "${_pc_path}")
if(NOT FREESWITCH_FOUND)
	message(STATUS "mod_dmodem: FreeSWITCH (freeswitch.pc) not found, skipped")
	return()
endif()
message(STATUS "mod_dmodem: FreeSWITCH ${FREESWITCH_VERSION} in ${FREESWITCH_PREFIX}")

if(NOT _fs_modules)
	set(_fs_modules ${FREESWITCH_PREFIX}/mod)
endif()
if(NOT _fs_conf)
	set(_fs_conf ${FREESWITCH_PREFIX}/conf)
endif()
set(FREESWITCH_MODULES_DIR ${_fs_modules} CACHE PATH "FreeSWITCH modules")
set(FREESWITCH_CONF_DIR ${_fs_conf} CACHE PATH "FreeSWITCH configuration")

add_library(mod_dmodem MODULE mod_dmodem.c ${DMODEM_PBX_DIR}/dmodem_bridge.c)
set_target_properties(mod_dmodem PROPERTIES PREFIX "")
target_include_directories(mod_dmodem PRIVATE ${FREESWITCH_INCLUDE_DIRS}
	${DMODEM_PBX_DIR})
target_compile_definitions(mod_dmodem PRIVATE _GNU_SOURCE)
target_compile_options(mod_dmodem PRIVATE -Wall -g -O2 -pthread
	${FREESWITCH_CFLAGS_OTHER})
# the library's symbols stay inside the module; FreeSWITCH's own come from
# the process that loads it
target_link_libraries(mod_dmodem PRIVATE dmodem)
target_link_options(mod_dmodem PRIVATE -Wl,--exclude-libs,ALL)

install(TARGETS mod_dmodem LIBRARY DESTINATION ${FREESWITCH_MODULES_DIR})
install(FILES dmodem.conf.xml
	DESTINATION ${FREESWITCH_CONF_DIR}/autoload_configs)
