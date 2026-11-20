# platforms/

This folder has the support for only one operating system: its toolchain files, its
build settings, its drivers and its headers. The modem itself (`core/`,
`dsp/`, `link/`) and the programs that use it (`frontend/`) are the same on
all systems. They are not in this folder.

| Directory | What is in it |
|-----------|---------------|
| [haiku/](haiku/README.md) | The cross build from Linux (toolchain file, sysroot maker) and the patch of libre for Haiku |
| [macos/](macos/README.md) | The build settings for macOS (the oldest release on which the programs run) |
| [windows/](windows/README.md) | The cross builds from Linux (MinGW-w64, clang-cl), the POSIX headers for MSVC, the Windows 7 import check and the virtual modem driver |

Linux has no files of its own in this folder, because we wrote the tree on Linux.
Some parts of `frontend/dmodemd/` are for only one system:

- The ALSA, HD-audio and mISDN lines, on Linux.
- `main_windows.cpp` and `windows_dte.cpp`, on Windows.

These parts build from `frontend/dmodemd/`, inside dmodemd.

The documentation for each system is in `docs/`:
[haiku.md](../docs/haiku.md), [macos.md](../docs/macos.md),
[windows.md](../docs/windows.md).
