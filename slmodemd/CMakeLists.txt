# slmodemd (the modem daemon behind a pty), modem_test, and libdmodem (the
# modem core and the open data pumps at 8000 Hz as a library, dmodem.h)

set(DMODEM_CORE_SRCS
	modem.c modem_datafile.c modem_at.c modem_timer.c modem_pack.c
	modem_ec.c modem_comp.c modem_param.c modem_debug.c homolog_data.c
	dp_sinus.c dp_dummy.c sysdep_common.c)
set(DMODEM_DSP_SRCS
	dsp/dsp_glue.c dsp/dsp_util.c dsp/dp_callprog.c dsp/dp_fsk.c
	dsp/dp_v22.c dsp/dp_v32.c dsp/qam.c dsp/qam_tables.c dsp/fax_modems.c
	dsp/fax_class1.c dsp/dp_v34.c dsp/v34_tables.c dsp/v34_modem.c
	dsp/v34_data.c dsp/v90.c)
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
		set(_main modem_main.c)
	else()
		set(_main modem_test.c)
	endif()
	add_executable(${_exe} ${_main} modem_cmdline.c ${DMODEM_CORE_SRCS} ${_dsp})
	target_include_directories(${_exe} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_compile_definitions(${_exe} PRIVATE CONFIG_DEBUG_MODEM)
	target_compile_options(${_exe} PRIVATE -Wall -g -O ${_arch})
	target_link_options(${_exe} PRIVATE ${_arch} ${_blob_link})
	target_link_libraries(${_exe} PRIVATE m pthread)
endforeach()
install(TARGETS slmodemd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

# the library brings its own timers (dmodem_timer.c, no signals)
set(_lib_core ${DMODEM_CORE_SRCS})
list(REMOVE_ITEM _lib_core modem_timer.c)
add_library(dmodem STATIC
	${_lib_core} dmodem.c dmodem_timer.c dmodem_fax.c
	dsp/t30.c dsp/t4.c dsp/tiff.c ${DMODEM_DSP_SRCS})
target_include_directories(dmodem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(dmodem PRIVATE CONFIG_DEBUG_MODEM MODEM_RATE=8000)
target_compile_options(dmodem PRIVATE -Wall -g -O2)
set_target_properties(dmodem PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dmodem PUBLIC m pthread)
