# platforms/windows/

D-Modem on Windows 7 and later.

| Path | What it does |
|------|--------------|
| `mingw-w64.cmake` | Toolchain file for building for Windows from Linux with MinGW-w64 (Clang or GCC) |
| `clang-cl.cmake` | Toolchain file for building the MSVC way from Linux, with clang-cl and lld-link |
| `msvc/` | The POSIX headers MSVC's C library lacks (`unistd.h`, `strings.h`, `sys/time.h`, `dmodem_msvc.h`) |
| `win_imports.py` | The `win7_imports` test: no program or driver imports anything newer than Windows 7 |
| `winmodem/` | The virtual modem driver (`dmodemvm.dll`, `dmodemvm7.dll`), its installer (`dmodem-driver.exe`) and INF files |

    cmake -B build-win -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=platforms/windows/mingw-w64.cmake

The details are in [docs/windows.md](../../docs/windows.md) and
[docs/windows-details.md](../../docs/windows-details.md).
