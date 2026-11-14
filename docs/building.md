# Building

To build D-Modem, you must have these items:

- Linux (glibc or musl: Alpine builds and passes the same tests).
- CMake (3.16 or later).
- A C++17 compiler (Clang if it is installed, and GCC if not: refer to the text below). For the Asterisk and FreeSWITCH modules, you must also have a C compiler.
- git.

OpenSSL is not necessary. The crypto of libre (the digests for the authentication of SIP, AES for SRTP) comes from Mbed TLS. The build downloads Mbed TLS and builds it together with libre (refer to the text below).

    cmake -B build
    cmake --build build

If Clang (`clang`, `clang++`, or the newest `clang-<n>`) is installed and nothing else selected the compiler, the build uses Clang. It uses the C++ library of GCC (libstdc++) and binutils, as Clang uses them on Linux. To use GCC instead, use `CC=gcc CXX=g++`, `-DCMAKE_C_COMPILER=...` or `-DDMODEM_PREFER_CLANG=OFF`. An existing build directory keeps the compiler that you configured it with. We test the two compilers. CI builds with Clang.

With libasound2-dev (ALSA), dmodemd can also operate the codec of a softmodem (`-a`). With libusb-1.0-0-dev, dmodemd can also operate these USB modems (`--usb`, [docs/hardware.md](hardware.md)):

- A Smart Link SmartUSB56.
- A [pico-serial Line](https://github.com/D-Modem/hardware) stick.
- A pico-serial WB stick (a bare pair, the wideband mode).
- An Agere USB 2.0 soft modem.

This procedure builds these items:

- `build/dmodemd/dmodemd`, `build/dmodemd/libdmodem.a` and `build/dmodemd/libdmodem_sip.a`.
- If the headers of Asterisk are installed: `build/asterisk/codec_clearmode.so` (the CLEARMODE of RFC 4040, for ISDN data calls). With `-DDMODEM_APP_DMODEM=ON`, the build also makes `app_dmodem.so`, for your own Asterisk only ([LICENSING.md](../LICENSING.md)).
- If FreeSWITCH is installed (its `freeswitch.pc`): `build/freeswitch/mod_dmodem.so`.

`cmake --install build` installs these items.

The `Dockerfile` makes the same build on Alpine Linux (musl). It makes a small image with dmodemd, dmodem-ari and libdmodem. Use `docker build -t dmodem .`, then `docker run --rm -it --network host -e SIP_LOGIN=user:password@domain dmodem`. With `--build-arg TESTS=ON`, the build also builds the tests and runs the offline tests.

The SIP of dmodemd is [libre](https://github.com/baresip/re) (the re of baresip, BSD-3-Clause). libre is not part of this repository. CMake uses an installed libre (`pkg-config libre`, 4.12 or later). If no libre is installed, CMake clones the pinned release (v4.12.0) one time and builds it in the build directory as a static library. These options change this behavior:

- `-DDMODEM_LIBRE=system` makes an installed libre necessary.
- `-DDMODEM_LIBRE=fetch` always builds the pinned libre.
- `-DDMODEM_LIBRE_TAG=...` selects a different release.

The SDP, RTP (G.711) and UDPTL are the code of dmodemd. That libre uses [Mbed TLS](https://github.com/Mbed-TLS/mbedtls). The build downloads the pinned release of Mbed TLS (v3.6.7) and builds it in the same method, and links it statically. libre uses some digests and AES from Mbed TLS, but not TLS. Thus, only those parts go into dmodemd. On Mbed TLS, libre has no AES of its own. The patch of D-Modem, `cmake/libre-mbedtls-aes.patch`, adds it for SRTP. `-DDMODEM_CRYPTO=openssl` builds libre on the OpenSSL of the system (libssl-dev) instead.

`libdmodem_sip.a` is the modem on a SIP line as a library (`frontend/dmodemd/sip_modem.h`). It has these parts:

- The modem core with its SIP user agent.
- Its DTE, which can be any descriptor (a pty, or one end of a socket pair).

The library runs in a thread of the program, and a different thread stops it. dmodemd is a pty in front of the library.

The library cross compiles. With a CMake toolchain file, the build downloads libre and Mbed TLS and builds them with the same toolchain. An example is the toolchain file of the Android NDK: `-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26`. On a system without `/etc/resolv.conf` (Android), give the DNS servers in `sip::Config`. The tree also builds as a subdirectory of a different CMake project.

## Windows

dmodemd also builds for Windows (7 and later) as one static `dmodemd.exe`. You can build it in these methods:

- With MSVC on Windows (`cmake -G Ninja` in a Developer Command Prompt).
- From Linux with MinGW-w64 (`-DCMAKE_TOOLCHAIN_FILE=platforms/windows/mingw-w64.cmake`).
- From Linux with clang-cl and the libraries of MSVC (`platforms/windows/clang-cl.cmake`).

The DTE of `dmodemd.exe` can be one of these items:

- A COM port (`--com COM5`, one end of a [com0com](https://sourceforge.net/projects/com0com/) pair).
- A named pipe (`--pipe`, `--pipe-connect`). This is the COM port of a virtual machine, so that a guest can dial through it.
- The virtual modem driver of D-Modem (`--driver auto`). This is a UMDF driver that Windows shows as a modem, for dial-up networking and fax programs through Unimodem. A UMDF 1 build is available for Windows 7 and 8. We did not try it on Windows itself yet.
- A TCP connection (`--tcp`, by default 127.0.0.1:2323).

The USB modems (`--usb`: the SmartUSB56, the pico-serial Line and the Agere modem, on WinUSB) also operate on Windows. The PBX modules and the ALSA line are for Linux only. Refer to [docs/windows.md](windows.md).

## macOS

dmodemd also builds for macOS 11 and later, on Apple silicon and Intel. For one universal binary, use `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`. You must have only the command line tools of Apple and CMake. The build downloads libre, Mbed TLS and libusb and builds them. The result is the Linux modem with its pty and the USB modems (`--usb`, through the IOKit backend of libusb). The build does not include the ALSA line. We did not build it on a Mac yet. The CI job is ready for the time when the CI of the repository has runners again. Refer to [docs/macos.md](macos.md).

## Haiku

dmodemd also builds for [Haiku](https://www.haiku-os.org/) (x86_64), the open successor of BeOS. You can build it in these methods:

- On Haiku itself: use `pkgman install cmake ninja git pkgconfig libusb_devel`, then `cmake -B build`.
- From Linux with Clang and a sysroot of the packages of Haiku: use `python3 platforms/haiku/haiku_sysroot.py ~/haiku-sysroot`, then `-DCMAKE_TOOLCHAIN_FILE=platforms/haiku/haiku.cmake -DHAIKU_SYSROOT=$HOME/haiku-sysroot`.

The result is the Linux modem with its pty and the USB modems (`--usb`, through the libusb of HaikuPorts). The build does not include the ALSA line and the PBX modules. We check the cross build with warnings as errors, but we did not run it on Haiku itself yet. Refer to [docs/haiku.md](haiku.md).

Tests: `ctest --test-dir build -L quick` runs the fast tests (some seconds). These are GoogleTest programs. They use the libgtest-dev of the system, or a copy that CMake downloads when you configure. `-L spandsp` (with libspandsp-dev) and `-L smartlink` take more time. The interop tests run the modem against other soft modems ([docs/interop.md](interop.md)). `-L smartlink` runs the modem against the original binary-only i386 DSP library of Smart Link (`dsp/test/smartlink/dsplibs.o`). We keep this library only as a test peer. These tests must have a 32-bit build environment (`-m32` with Clang or GCC, and 32-bit libraries: on Debian-based systems, install `g++-multilib`). Without this environment, the tests do not run. Refer to [docs/testing.md](testing.md).
