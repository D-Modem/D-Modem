# The new core against the old one: core_script runs a script with two
# modems back to back on either core, and the transcripts must be the same
# (core_equiv_*).  core_test tests the new core by itself.

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

file(GLOB _scripts ${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.txt)
foreach(_s ${_scripts})
	get_filename_component(_n ${_s} NAME_WE)
	add_test(NAME core_equiv_${_n} COMMAND sh -c
		"$<TARGET_FILE:core_script_old> ${_s} > ${_n}.old && \
		 $<TARGET_FILE:core_script_new> ${_s} > ${_n}.new && \
		 diff ${_n}.old ${_n}.new")
	set_tests_properties(core_equiv_${_n} PROPERTIES LABELS quick)
endforeach()

add_executable(core_test core_test.cpp)
target_compile_options(core_test PRIVATE -Wall -Wextra -g -O2)
target_link_libraries(core_test PRIVATE dmodem_core dmodem_dsp dmodem_link m)
add_test(NAME core_test COMMAND core_test)
set_tests_properties(core_test PROPERTIES LABELS quick)
