# platforms/windows/

D-Modem on Windows. The clang-retro build runs on Windows 98 SE and later. The MinGW-w64, MSVC and clang-cl builds run on Windows 7 and later.

| Path | What it does |
|------|--------------|
| `clang-retro.cmake` | Toolchain file to build with the toolchain of [clang-retro](https://github.com/clang-retro/dos-sdk), from Linux or Windows (the default method). `CLANG_RETRO_CONFIG` selects the oldest Windows, for example `win98` or `win7-x64` |
| `clang-retro/` | The functions that the old C library of Windows 98 does not have (`dmodem_retro.c`, `dmodem_retro.h`) |
| `mingw-w64.cmake` | Toolchain file to build for Windows from Linux with MinGW-w64 (Clang or GCC) |
| `clang-cl.cmake` | Toolchain file to build in the MSVC method from Linux, with clang-cl and lld-link |
| `msvc/` | The POSIX headers that the C library of MSVC does not have (`unistd.h`, `strings.h`, `sys/time.h`, `dmodem_msvc.h`) |
| `win_imports.py` | The `win7_imports` test: it makes sure that no program or driver imports an item that is newer than Windows 7 |
| `winmodem/` | The virtual modem driver (`dmodemvm.dll`, `dmodemvm7.dll`), its installer (`dmodem-driver.exe`) and the INF files |

    cmake -B build-win -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=platforms/windows/mingw-w64.cmake

The details are in [docs/windows.md](../../docs/windows.md) and
[docs/windows-details.md](../../docs/windows-details.md).
