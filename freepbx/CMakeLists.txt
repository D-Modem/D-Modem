# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
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
