# The core against slmodemd's old one: core_script runs a script with two
# modems back to back, and its transcript must be the script's .expect
# (core_equiv): the old core's, recorded before it left the tree, or the
# core's own where it differs on purpose.  core_test tests the core by
# itself.

add_executable(core_script script.cpp script_core.cpp)
target_compile_options(core_script PRIVATE -Wall -Wextra -g -O2)
target_link_libraries(core_script PRIVATE dmodem_core dmodem_dsp
	dmodem_link m)

add_executable(core_equiv core_equiv.cpp)
target_compile_options(core_equiv PRIVATE -Wall -Wextra -g -O2)
target_compile_definitions(core_equiv PRIVATE
	"SCRIPTS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/scripts\""
	"SCRIPT=\"$<TARGET_FILE:core_script>\"")
add_dependencies(core_equiv core_script)
dmodem_gtest(core_equiv LABELS quick)

add_executable(core_test core_test.cpp)
target_compile_options(core_test PRIVATE -Wall -Wextra -g -O2)
target_link_libraries(core_test PRIVATE dmodem_core dmodem_dsp dmodem_link m)
dmodem_gtest(core_test LABELS quick)
