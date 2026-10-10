# slmodemd (the modem daemon behind a pty, on SIP calls), modem_test, and
# libdmodem (the modem core and the open data pumps at 8000 Hz as a
# library, dmodem.h).  Smart Link's original DSP library, dsplibs.o, is
# only in the interop tests (dsp/test).

set(DMODEM_CORE_SRCS
	modem.cpp modem_datafile.cpp modem_at.cpp modem_timer.cpp modem_pack.cpp
	modem_ec.cpp modem_comp.cpp modem_param.cpp modem_debug.cpp homolog_data.cpp
	dp_sinus.cpp dp_dummy.cpp sysdep_common.cpp)
set(DMODEM_DSP_SRCS
	dsp/dsp_glue.cpp dsp/dsp_util.cpp dsp/dp_callprog.cpp dsp/dp_fsk.cpp
	dsp/dp_v22.cpp dsp/dp_v32.cpp dsp/qam.cpp dsp/qam_tables.cpp dsp/fax_modems.cpp
	dsp/fax_class1.cpp dsp/dp_v34.cpp dsp/v34_tables.cpp dsp/v34_modem.cpp
	dsp/v34_data.cpp dsp/v90.cpp dsp/v91.cpp)
# for the test programs in dsp/test
set(DMODEM_CORE_SRCS ${DMODEM_CORE_SRCS} PARENT_SCOPE)
set(DMODEM_DSP_SRCS ${DMODEM_DSP_SRCS} PARENT_SCOPE)

if(DMODEM_SLMODEMD)
	# the modem daemon: the core and the open data pumps at the rate of
	# the call (8000 Hz), its SIP user agent (sip.cpp) and a pty
	add_executable(slmodemd modem_main.cpp modem_cmdline.cpp sip.cpp
		${DMODEM_CORE_SRCS} ${DMODEM_DSP_SRCS})
	target_include_directories(slmodemd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_compile_definitions(slmodemd PRIVATE CONFIG_DEBUG_MODEM
		MODEM_RATE=8000)
	target_compile_options(slmodemd PRIVATE -Wall -g -O2)
	target_link_libraries(slmodemd PRIVATE pjproject m pthread)
	install(TARGETS slmodemd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
endif()

# modem_test: the modem simulator of the original slmodemd sources
add_executable(modem_test modem_test.cpp modem_cmdline.cpp
	${DMODEM_CORE_SRCS} ${DMODEM_DSP_SRCS})
target_include_directories(modem_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(modem_test PRIVATE CONFIG_DEBUG_MODEM)
target_compile_options(modem_test PRIVATE -Wall -g -O)
target_link_libraries(modem_test PRIVATE m pthread)

# the library brings its own timers (dmodem_timer.cpp, no signals)
set(_lib_core ${DMODEM_CORE_SRCS})
list(REMOVE_ITEM _lib_core modem_timer.cpp)
add_library(dmodem STATIC
	${_lib_core} dmodem.cpp dmodem_timer.cpp dmodem_fax.cpp
	dsp/t30.cpp dsp/t4.cpp dsp/tiff.cpp ${DMODEM_DSP_SRCS})
target_include_directories(dmodem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(dmodem PRIVATE CONFIG_DEBUG_MODEM MODEM_RATE=8000)
target_compile_options(dmodem PRIVATE -Wall -g -O2)
set_target_properties(dmodem PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dmodem PUBLIC m pthread)
install(TARGETS dmodem ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES dmodem.h dmodem.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
