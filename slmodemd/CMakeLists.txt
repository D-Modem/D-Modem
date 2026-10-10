# slmodemd (the modem daemon behind a pty, on SIP calls), modem_test, and
# libdmodem (the modem core and the open data pumps at 8000 Hz as a
# library, dmodem.h).  The data pumps are in ../dsp (DMODEM_DSP_SRCS);
# Smart Link's original DSP library, dsplibs.o, is only in the interop
# tests (../dsp/test).

set(DMODEM_CORE_SRCS
	modem.cpp modem_datafile.cpp modem_at.cpp modem_timer.cpp
	modem_param.cpp modem_debug.cpp homolog_data.cpp sysdep_common.cpp)
list(TRANSFORM DMODEM_CORE_SRCS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
# the data link layer (../link) comes with the core
list(APPEND DMODEM_CORE_SRCS ${DMODEM_LINK_SRCS})
# for the test programs in ../dsp/test
set(DMODEM_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
set(DMODEM_CORE_SRCS ${DMODEM_CORE_SRCS} PARENT_SCOPE)

if(DMODEM_SLMODEMD)
	# the modem daemon: the core and the open data pumps at the rate of
	# the call (8000 Hz), its SIP user agent (sip.cpp) and a pty
	add_executable(slmodemd modem_main.cpp modem_cmdline.cpp sip.cpp
		${DMODEM_CORE_SRCS} ${DMODEM_DSP_SRCS})
	target_include_directories(slmodemd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
		${DMODEM_DSP_DIR} ${DMODEM_LINK_DIR})
	target_compile_definitions(slmodemd PRIVATE CONFIG_DEBUG_MODEM
		MODEM_RATE=8000)
	target_compile_options(slmodemd PRIVATE -Wall -g -O2)
	target_link_libraries(slmodemd PRIVATE pjproject m pthread)
	install(TARGETS slmodemd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
endif()

# modem_test: the modem simulator of the original slmodemd sources
add_executable(modem_test modem_test.cpp modem_cmdline.cpp
	${DMODEM_CORE_SRCS} ${DMODEM_DSP_SRCS})
target_include_directories(modem_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
	${DMODEM_DSP_DIR} ${DMODEM_LINK_DIR})
target_compile_definitions(modem_test PRIVATE CONFIG_DEBUG_MODEM)
target_compile_options(modem_test PRIVATE -Wall -g -O)
target_link_libraries(modem_test PRIVATE m pthread)

# the library brings its own timers (dmodem_timer.cpp, no signals)
set(_lib_core ${DMODEM_CORE_SRCS})
list(REMOVE_ITEM _lib_core ${CMAKE_CURRENT_SOURCE_DIR}/modem_timer.cpp)
add_library(dmodem STATIC
	${_lib_core} dmodem.cpp dmodem_timer.cpp dmodem_fax.cpp
	${DMODEM_T30_SRCS} ${DMODEM_DSP_SRCS})
target_include_directories(dmodem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE ${DMODEM_DSP_DIR} ${DMODEM_LINK_DIR})
target_compile_definitions(dmodem PRIVATE CONFIG_DEBUG_MODEM MODEM_RATE=8000)
target_compile_options(dmodem PRIVATE -Wall -g -O2)
set_target_properties(dmodem PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dmodem PUBLIC m pthread)
install(TARGETS dmodem ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES dmodem.h dmodem.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
