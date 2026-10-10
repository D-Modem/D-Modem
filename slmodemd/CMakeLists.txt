# slmodemd (the modem daemon behind a pty), modem_test, and libdmodem (the
# modem core and the open data pumps at 8000 Hz as a library, dmodem.h)

set(DMODEM_CORE_SRCS
	modem.cpp modem_datafile.cpp modem_at.cpp modem_timer.cpp modem_pack.cpp
	modem_ec.cpp modem_comp.cpp modem_param.cpp modem_debug.cpp homolog_data.cpp
	dp_sinus.cpp dp_dummy.cpp sysdep_common.cpp)
set(DMODEM_DSP_SRCS
	dsp/dsp_glue.cpp dsp/dsp_util.cpp dsp/dp_callprog.cpp dsp/dp_fsk.cpp
	dsp/dp_v22.cpp dsp/dp_v32.cpp dsp/qam.cpp dsp/qam_tables.cpp dsp/fax_modems.cpp
	dsp/fax_class1.cpp dsp/dp_v34.cpp dsp/v34_tables.cpp dsp/v34_modem.cpp
	dsp/v34_data.cpp dsp/v90.cpp)
# for the test programs in dsp/test
set(DMODEM_CORE_SRCS ${DMODEM_CORE_SRCS} PARENT_SCOPE)
set(DMODEM_DSP_SRCS ${DMODEM_DSP_SRCS} PARENT_SCOPE)

# the original data pumps are a 32 bit i386 object
set(DMODEM_M32 "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
	set(DMODEM_M32 -m32)
endif()
set(DMODEM_M32 ${DMODEM_M32} PARENT_SCOPE)

if(DMODEM_DSPLIBS STREQUAL "blob")
	if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i.86)$")
		message(FATAL_ERROR "dsplibs.o is i386 code: use -DDMODEM_DSPLIBS=open")
	endif()
	set(_dsp ${CMAKE_CURRENT_SOURCE_DIR}/dsplibs.o)
	set(_arch ${DMODEM_M32})
	# its code is not position independent and its object asks for an
	# executable stack it does not need
	set(_blob_link -no-pie -Wl,-z,noexecstack)
elseif(DMODEM_DSPLIBS STREQUAL "open")
	set(_dsp ${DMODEM_DSP_SRCS})
	set(_arch "")
else()
	message(FATAL_ERROR "DMODEM_DSPLIBS: blob or open")
endif()

foreach(_exe slmodemd modem_test)
	if(_exe STREQUAL "slmodemd")
		set(_main modem_main.cpp)
	else()
		set(_main modem_test.cpp)
	endif()
	add_executable(${_exe} ${_main} modem_cmdline.cpp ${DMODEM_CORE_SRCS} ${_dsp})
	target_include_directories(${_exe} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_compile_definitions(${_exe} PRIVATE CONFIG_DEBUG_MODEM)
	target_compile_options(${_exe} PRIVATE -Wall -g -O ${_arch})
	target_link_options(${_exe} PRIVATE ${_arch} ${_blob_link})
	target_link_libraries(${_exe} PRIVATE m pthread)
endforeach()
install(TARGETS slmodemd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

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
