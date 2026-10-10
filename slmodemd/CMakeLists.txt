# slmodemd (the modem daemon behind a pty, on SIP calls), modem_test, and
# libdmodem (the modem as a library, dmodem.h).  They run the modem core
# (../core, dmodem_core) with the data pumps (../dsp, dmodem_dsp) and the
# data link layer (../link, dmodem_link).
#
# The old C core of slmodemd (modem.cpp, modem_at.cpp, ...) is still here
# for the tests that need it: Smart Link's original DSP library, a test
# peer, calls into it (../dsp/test), and core_script_old compares the new
# core with it (../core/test).
#
#   DMODEM_CORE_DIR, DMODEM_CORE_SRCS    the old core
#   DMODEM_FRONT_SRCS                    the front ends' parts (options,
#                                        pty, a modem on two descriptors)

set(DMODEM_CORE_SRCS
	modem.cpp modem_datafile.cpp modem_at.cpp modem_timer.cpp
	modem_param.cpp modem_debug.cpp sysdep_common.cpp)
list(TRANSFORM DMODEM_CORE_SRCS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
# the country parameters are the new core's (../core)
list(APPEND DMODEM_CORE_SRCS ${DMODEM_CORE_NEW_DIR}/homolog_data.cpp)
set(DMODEM_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
set(DMODEM_CORE_SRCS ${DMODEM_CORE_SRCS} PARENT_SCOPE)

set(_front options.cpp pty.cpp datafile.cpp pipe_modem.cpp)
list(TRANSFORM _front PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
set(DMODEM_FRONT_SRCS ${_front} PARENT_SCOPE)
add_library(dmodem_front OBJECT ${_front})
target_link_libraries(dmodem_front PUBLIC dmodem_core)
target_compile_options(dmodem_front PRIVATE -Wall -Wextra -g -O2)

if(DMODEM_SLMODEMD)
	# the modem daemon: its SIP user agent (sip.cpp) and a pty
	add_executable(slmodemd main.cpp sip.cpp)
	target_compile_options(slmodemd PRIVATE -Wall -g -O2)
	target_link_libraries(slmodemd PRIVATE dmodem_front dmodem_core
		dmodem_dsp dmodem_link pjproject m pthread)
	install(TARGETS slmodemd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
endif()

# modem_test: two modems back to back, each with a pty
add_executable(modem_test modem_test.cpp)
target_compile_options(modem_test PRIVATE -Wall -Wextra -g -O2)
target_link_libraries(modem_test PRIVATE dmodem_front dmodem_core dmodem_dsp
	dmodem_link m pthread)

# libdmodem: the objects of dmodem_core, dmodem_dsp, dmodem_t30 and
# dmodem_link go into libdmodem.a
add_library(dmodem STATIC dmodem.cpp dmodem_fax.cpp
	$<TARGET_OBJECTS:dmodem_core> $<TARGET_OBJECTS:dmodem_dsp>
	$<TARGET_OBJECTS:dmodem_t30> $<TARGET_OBJECTS:dmodem_link>)
target_include_directories(dmodem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE ${DMODEM_CORE_NEW_DIR} ${DMODEM_DSP_DIR} ${DMODEM_LINK_DIR})
target_compile_options(dmodem PRIVATE -Wall -g -O2)
set_target_properties(dmodem PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dmodem PUBLIC m pthread)
install(TARGETS dmodem ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES dmodem.h dmodem.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
