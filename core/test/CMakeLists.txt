# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# The core against slmodemd's old one: core_script runs a script with two
# modems back to back, and its transcript must be the script's .expect
# (core_equiv): the old core's, recorded before it left the tree, or the
# core's own where it differs on purpose.  core_test tests the core by
# itself.

add_executable(core_script script.cpp script_core.cpp)
dmodem_flags(core_script EXTRA)
target_link_libraries(core_script PRIVATE dmodem_core dmodem_dsp dmodem_t30
	dmodem_link ${DMODEM_LIBM})

add_executable(core_equiv core_equiv.cpp)
dmodem_flags(core_equiv EXTRA)
target_compile_definitions(core_equiv PRIVATE
	"SCRIPTS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/scripts\""
	"SCRIPT=\"$<TARGET_FILE:core_script>\"")
add_dependencies(core_equiv core_script)
dmodem_gtest(core_equiv LABELS quick)

add_executable(core_test core_test.cpp)
dmodem_flags(core_test EXTRA)
target_link_libraries(core_test PRIVATE dmodem_core dmodem_dsp dmodem_t30 dmodem_link ${DMODEM_LIBM})
dmodem_gtest(core_test LABELS quick)
