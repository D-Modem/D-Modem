# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# app_dmodem.so: the DModem() dialplan application, built against the
# headers of an installed Asterisk (make install-headers, or the
# asterisk-dev package of a distribution); skipped when they are missing.
#
#   -DASTERISK_INCLUDE_DIR=...     headers elsewhere than the usual places
#   -DASTERISK_MODULES_DIR=...     where `cmake --install` puts the module
#   -DASTERISK_CONF_DIR=...        where the sample dmodem.conf goes

find_path(ASTERISK_INCLUDE_DIR asterisk.h
	PATHS /usr/include /usr/local/include)
if(NOT ASTERISK_INCLUDE_DIR OR
   NOT EXISTS ${ASTERISK_INCLUDE_DIR}/asterisk/module.h)
	message(STATUS "app_dmodem: Asterisk headers not found, skipped")
	return()
endif()
message(STATUS "app_dmodem: Asterisk headers in ${ASTERISK_INCLUDE_DIR}")

find_package(PkgConfig QUIET)
set(_modules_dir /usr/lib/asterisk/modules)
if(CMAKE_LIBRARY_ARCHITECTURE AND
   EXISTS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/asterisk/modules)
	# Debian's and Ubuntu's packages
	set(_modules_dir /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/asterisk/modules)
endif()
if(PKG_CONFIG_FOUND)
	pkg_get_variable(_pc_modules asterisk modulesdir)
	if(_pc_modules)
		set(_modules_dir ${_pc_modules})
	endif()
endif()
set(ASTERISK_MODULES_DIR ${_modules_dir} CACHE PATH "Asterisk modules")
set(ASTERISK_CONF_DIR /etc/asterisk CACHE PATH "Asterisk configuration")

add_library(app_dmodem MODULE app_dmodem.c ${DMODEM_PBX_DIR}/dmodem_bridge.c)
set_target_properties(app_dmodem PROPERTIES PREFIX "")
target_include_directories(app_dmodem PRIVATE ${ASTERISK_INCLUDE_DIR}
	${DMODEM_PBX_DIR})
target_compile_definitions(app_dmodem PRIVATE _GNU_SOURCE
	AST_MODULE="app_dmodem"
	AST_MODULE_SELF_SYM=__internal_app_dmodem_self)
target_compile_options(app_dmodem PRIVATE -Wall -g -O2 -pthread)
# the library's symbols stay inside the module
target_link_libraries(app_dmodem PRIVATE dmodem)
target_link_options(app_dmodem PRIVATE -Wl,--exclude-libs,ALL)

install(TARGETS app_dmodem LIBRARY DESTINATION ${ASTERISK_MODULES_DIR})
install(FILES dmodem.conf.sample DESTINATION ${ASTERISK_CONF_DIR})
