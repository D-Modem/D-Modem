# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# winmodem/: D-Modem's virtual modem driver for Windows, a UMDF 2 driver
# whose COM port Windows' modem stack (Unimodem, modem.sys) and apps use
# as a modem, dmodemd (--driver) on its other side through a named pipe.
#
#   dmodem_winmodem   the port (port.cpp) and the frames (wire.cpp):
#                     portable, unit-tested everywhere (port_test)
#   dmodem_winlink    (Windows) the driver's end of the pipe (link.cpp),
#                     also run by vmodem_test against dmodemd.exe
#   dmodemvm.dll      (Windows, with the WDK's UMDF 2 stub library: on
#                     MSVC by DMODEM_WDK) the driver, with dmodem.inf
#   dmodemvm_check    (DMODEM_WDF_INCLUDE: the UMDF headers, e.g. from
#                     github.com/microsoft/Windows-Driver-Frameworks) the
#                     driver compiled only, where no WDK is (clang-cl)
#   dmodemvm7.dll     (Windows, with UMDF 1.9's headers:
#                     DMODEM_UMDF1_INCLUDE) the driver for Windows 7 and 8
#                     (driver7.cpp), with dmodem7.inf; umdf1_test runs it
#   dmodem-driver.exe (Windows) installs and removes the driver's device,
#                     and WinUSB for the USB modems (dmodem-usb.inf)

add_library(dmodem_winmodem STATIC port.cpp wire.cpp)
target_include_directories(dmodem_winmodem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
dmodem_flags(dmodem_winmodem EXTRA)
set_target_properties(dmodem_winmodem PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(DMODEM_TESTS)
	add_executable(port_test test/port_test.cpp)
	dmodem_flags(port_test EXTRA NOOPT)
	target_link_libraries(port_test PRIVATE dmodem_winmodem)
	dmodem_gtest(port_test LABELS quick TIMEOUT 60)
	# dmodem.inf as make_inf.py makes it from the core's result codes
	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(Python3_Interpreter_FOUND)
		dmodem_add_test(winmodem_inf ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/make_inf.py
			${PROJECT_SOURCE_DIR} --check)
		set_tests_properties(winmodem_inf PROPERTIES LABELS quick
			TIMEOUT 30)
	endif()
endif()

if(NOT WIN32)
	return()
endif()
add_library(dmodem_winlink STATIC link.cpp)
target_link_libraries(dmodem_winlink PUBLIC dmodem_winmodem advapi32)
dmodem_flags(dmodem_winlink EXTRA)

add_executable(dmodem-driver install.cpp)
target_link_libraries(dmodem-driver PRIVATE setupapi newdev)
dmodem_flags(dmodem-driver EXTRA)
install(TARGETS dmodem-driver RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# (WinUSB for the USB modems, dmodemd --usb)
install(FILES dmodem-usb.inf DESTINATION driver)
if(DMODEM_TESTS)
	# (no device installed: listed as none)
	dmodem_add_test(dmodem_driver_list $<TARGET_FILE:dmodem-driver> list)
	set_tests_properties(dmodem_driver_list PROPERTIES LABELS quick
		TIMEOUT 60 PASS_REGULAR_EXPRESSION "no virtual modem installed")
endif()

# the driver: Microsoft's compiler's ABI (MSVC, or clang-cl), the UMDF
# 2.15 headers (Windows 10 1507 and later), and to link it the WDK's
# stub library.  DMODEM_WDF_INCLUDE and DMODEM_WDF_STUB are found in an
# installed WDK; DMODEM_WDF_INCLUDE alone (the headers of
# github.com/microsoft/Windows-Driver-Frameworks, src/publicinc/wdf/
# umdf/2.15) compiles the driver without linking it (dmodemvm_check)
if(NOT MSVC)
	return()
endif()

# the driver for Windows 7 and 8 (driver7.cpp): UMDF 1.9, Windows 7's own,
# a COM DLL that needs only UMDF 1.9's headers (the WDK's
# Include/wdf/umdf/1.9: DMODEM_UMDF1_INCLUDE).  umdf1_test runs it in a
# host of its own (test/umdf1_host.cpp) against dmodemd.exe
set(DMODEM_UMDF1_INCLUDE "${DMODEM_UMDF1_INCLUDE}" CACHE PATH
	"UMDF 1.9's headers (wudfddi.h's directory)")
if(NOT DMODEM_UMDF1_INCLUDE AND
   EXISTS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Include/wdf/umdf/1.9/wudfddi.h")
	set(DMODEM_UMDF1_INCLUDE
		"$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Include/wdf/umdf/1.9")
endif()
# (found here: for ../dmodemd's umdf1_test too)
set(DMODEM_UMDF1_INCLUDE "${DMODEM_UMDF1_INCLUDE}" PARENT_SCOPE)
if(DMODEM_UMDF1_INCLUDE)
	add_library(dmodemvm7 SHARED driver7.cpp dmodemvm7.def dmodemvm7.rc)
	target_include_directories(dmodemvm7 SYSTEM PRIVATE
		${DMODEM_UMDF1_INCLUDE})
	target_link_libraries(dmodemvm7 PRIVATE dmodem_winlink ole32 uuid)
	dmodem_flags(dmodemvm7)
	install(TARGETS dmodemvm7 RUNTIME DESTINATION driver7)
	install(FILES dmodem7.inf DESTINATION driver7)
endif()

set(_kits "$ENV{ProgramFiles\(x86\)}/Windows Kits/10")
if(NOT DMODEM_WDF_INCLUDE AND EXISTS "${_kits}/Include/wdf/umdf/2.15/wdf.h")
	set(DMODEM_WDF_INCLUDE "${_kits}/Include/wdf/umdf/2.15")
endif()
if(NOT DMODEM_WDF_STUB AND
   EXISTS "${_kits}/Lib/wdf/umdf/x64/2.15/WdfDriverStubUm.lib")
	set(DMODEM_WDF_STUB "${_kits}/Lib/wdf/umdf/x64/2.15/WdfDriverStubUm.lib")
endif()
set(DMODEM_WDF_INCLUDE "${DMODEM_WDF_INCLUDE}" CACHE PATH
	"UMDF 2.15's headers (wdf.h's directory)")
set(DMODEM_WDF_STUB "${DMODEM_WDF_STUB}" CACHE FILEPATH
	"the WDK's WdfDriverStubUm.lib (UMDF 2.15, x64)")
if(NOT DMODEM_WDF_INCLUDE)
	return()
endif()

set(_wdf_inc ${DMODEM_WDF_INCLUDE})
if(NOT CMAKE_HOST_WIN32)
	# (a case-sensitive file system: the headers' own names for each
	# other, WudfWdm.h for wudfwdm.h, as links in a copy)
	set(_wdf_inc ${CMAKE_CURRENT_BINARY_DIR}/wdf-include)
	file(GLOB _hs ${DMODEM_WDF_INCLUDE}/*.h)
	file(COPY ${_hs} DESTINATION ${_wdf_inc})
	foreach(_h ${_hs})
		file(STRINGS ${_h} _incs REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
		foreach(_i ${_incs})
			string(REGEX REPLACE ".*[<\"]([^>\"]+)[>\"].*" "\\1" _n "${_i}")
			string(TOLOWER "${_n}" _l)
			if(NOT EXISTS ${_wdf_inc}/${_n} AND EXISTS ${_wdf_inc}/${_l})
				file(CREATE_LINK ${_l} ${_wdf_inc}/${_n} SYMBOLIC)
			endif()
		endforeach()
	endforeach()
endif()

# what the WDK's UMDF projects define
set(_wdf_defs UMDF_VERSION_MAJOR=2 UMDF_VERSION_MINOR=15 UMDF_USING_NTSTATUS
	_UNICODE UNICODE)
if(DMODEM_WDF_STUB)
	add_library(dmodemvm SHARED driver.cpp dmodemvm.rc)
	target_link_libraries(dmodemvm PRIVATE dmodem_winlink
		${DMODEM_WDF_STUB} mincore)
	# (the stub's FxDriverEntryUm is the DLL's entry for the framework)
	target_link_options(dmodemvm PRIVATE /INCLUDE:FxDriverEntryUm)
	install(TARGETS dmodemvm RUNTIME DESTINATION driver)
	install(FILES dmodem.inf DESTINATION driver)
else()
	add_library(dmodemvm OBJECT driver.cpp dmodemvm.rc)
	add_custom_target(dmodemvm_check ALL DEPENDS dmodemvm)
endif()
target_include_directories(dmodemvm SYSTEM PRIVATE ${_wdf_inc})
target_compile_definitions(dmodemvm PRIVATE ${_wdf_defs})
dmodem_flags(dmodemvm)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	# (WDF_DECLARE_CONTEXT_TYPE's own: extern twice, a string literal
	# as PCHAR)
	target_compile_options(dmodemvm PRIVATE -Wno-duplicate-decl-specifier
		-Wno-writable-strings)
endif()
