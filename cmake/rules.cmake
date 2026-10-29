# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# rules.cmake: CMAKE_USER_MAKE_RULES_OVERRIDE, read after CMake's own
# settings for each compiler (its own and the fetched libraries' builds).
#
# Haiku: an executable is a shared object there (Clang and GCC link it
# with -shared), so position independent executables are compiled as
# shared libraries' code: -fPIC, not -fPIE (whose direct references to
# its own globals a shared object can't have).
if(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
	foreach(_lang C CXX)
		set(CMAKE_${_lang}_COMPILE_OPTIONS_PIE "-fPIC")
	endforeach()
endif()
