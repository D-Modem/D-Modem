# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# macos.cmake: macOS's settings, before project() (the top CMakeLists.txt
# includes it on every system; it does nothing but on a Mac).
#
# The oldest macOS to run on: 11 (Big Sur, the first for Apple silicon)
# and later, unless asked otherwise; CMAKE_OSX_ARCHITECTURES="arm64;x86_64"
# builds both (docs/macos.md)
if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE AND
   NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING
		"the oldest macOS to run on")
endif()
