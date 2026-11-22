# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# freepbx/: the FreePBX module (dmodem/, PHP: nothing to build).  The
# freepbx_module target packs it as FreePBX installs a module from,
# build/freepbx/dmodem-<version>.tgz (fwconsole ma installlocal after
# unpacking it into the modules directory, or Module Admin's upload);
# with PHP, freepbx_unit tests it (test/unit.php, no FreePBX needed).

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/dmodem/module.xml _xml)
string(REGEX MATCH "<version>([0-9.]+)</version>" _v "${_xml}")
set(DMODEM_FREEPBX_VERSION ${CMAKE_MATCH_1})
set(_tgz ${CMAKE_CURRENT_BINARY_DIR}/dmodem-${DMODEM_FREEPBX_VERSION}.tgz)
file(GLOB_RECURSE _files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dmodem/*)
add_custom_command(OUTPUT ${_tgz}
	COMMAND ${CMAKE_COMMAND} -E tar czf ${_tgz} dmodem
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	DEPENDS ${_files}
	COMMENT "FreePBX module dmodem ${DMODEM_FREEPBX_VERSION}")
add_custom_target(freepbx_module DEPENDS ${_tgz})

# the freepbx component: dmodem/ in the modules directory of FreePBX, for
# fwconsole ma installlocal.  It is not in a plain cmake --install (a
# machine without FreePBX has no such directory):
#   sudo cmake --install build --component freepbx
set(DMODEM_FREEPBX_MODULES_DIR /var/www/html/admin/modules CACHE PATH
	"FreePBX's modules directory (cmake --install --component freepbx)")
install(DIRECTORY dmodem DESTINATION ${DMODEM_FREEPBX_MODULES_DIR}
	COMPONENT freepbx EXCLUDE_FROM_ALL)

if(DMODEM_TESTS)
	find_program(DMODEM_PHP php)
	if(DMODEM_PHP)
		dmodem_add_test(freepbx_unit ${DMODEM_PHP}
			${CMAKE_CURRENT_SOURCE_DIR}/test/unit.php)
		set_tests_properties(freepbx_unit PROPERTIES LABELS quick
			TIMEOUT 60)
	else()
		message(STATUS "tests: no php, the FreePBX module's skipped")
	endif()
endif()
