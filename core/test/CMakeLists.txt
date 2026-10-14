# The new core against the old one: core_script runs a script with two
# modems back to back on either core, and the transcripts must be the same
# (core_equiv), or the new core's that of the script's .expect where it
# differs on purpose.  core_test tests the new core by itself.

# the old core (../../dsp/test/legacy) on libdmodem's old timers
set(_old ${DMODEM_LEGACY_SRCS})
list(REMOVE_ITEM _old ${DMODEM_LEGACY_DIR}/modem_timer.cpp)
add_executable(core_script_old script.cpp script_old.cpp ${_old}
	${DMODEM_LEGACY_DIR}/dmodem_timer.cpp)
target_include_directories(core_script_old PRIVATE ${DMODEM_LEGACY_DIR}
	${DMODEM_DSP_DIR} ${DMODEM_LINK_DIR} ${DMODEM_CORE_NEW_DIR})
target_compile_definitions(core_script_old PRIVATE CONFIG_DEBUG_MODEM
	MODEM_RATE=8000)
target_compile_options(core_script_old PRIVATE -Wall -g -O2)
target_link_libraries(core_script_old PRIVATE dmodem_dsp dmodem_link m
	pthread)

add_executable(core_script_new script.cpp script_new.cpp)
target_compile_options(core_script_new PRIVATE -Wall -Wextra -g -O2)
target_link_libraries(core_script_new PRIVATE dmodem_core dmodem_dsp
	dmodem_link m)

add_executable(core_equiv core_equiv.cpp)
target_compile_options(core_equiv PRIVATE -Wall -Wextra -g -O2)
target_compile_definitions(core_equiv PRIVATE
	"SCRIPTS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/scripts\""
	"SCRIPT_OLD=\"$<TARGET_FILE:core_script_old>\""
	"SCRIPT_NEW=\"$<TARGET_FILE:core_script_new>\"")
add_dependencies(core_equiv core_script_old core_script_new)
dmodem_gtest(core_equiv LABELS quick)

add_executable(core_test core_test.cpp)
target_compile_options(core_test PRIVATE -Wall -Wextra -g -O2)
target_link_libraries(core_test PRIVATE dmodem_core dmodem_dsp dmodem_link m)
dmodem_gtest(core_test LABELS quick)
