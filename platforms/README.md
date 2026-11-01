# platforms/

The support for one operating system alone: its toolchain files, its
build settings, its drivers and its headers.  The modem itself (`core/`,
`dsp/`, `link/`) and the programs on it (`frontend/`) are the same on
every system and stay out of here.

| Directory | What is in it |
|-----------|---------------|
| [haiku/](haiku/README.md) | The cross build from Linux (toolchain file, sysroot maker) and libre's patch for Haiku |
| [macos/](macos/README.md) | macOS's build settings (the oldest release to run on) |
| [windows/](windows/README.md) | The cross builds from Linux (MinGW-w64, clang-cl), MSVC's POSIX headers, the Windows 7 import check and the virtual modem driver |

Linux needs nothing of its own here: it is the system the tree was
written on.  A few parts of `frontend/dmodemd/` are still for one system
only (the ALSA, HD-audio and mISDN lines on Linux, `main_windows.cpp` and
`windows_dte.cpp` on Windows); they are built from there, inside dmodemd.

The documentation for each system is in `docs/`:
[haiku.md](../docs/haiku.md), [macos.md](../docs/macos.md),
[windows.md](../docs/windows.md).
