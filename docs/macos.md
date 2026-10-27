# D-Modem on macOS

dmodemd builds for macOS 11 (Big Sur) and later, on Apple silicon and Intel, as one universal binary if asked.  It is the same modem as on Linux: SIP on libre, the open DSP (data up to V.34 and V.90/V.92 as the digital modem, class 1 fax, voice) and T.38, with a pty as its DTE.  The USB modems (`--usb`: Smart Link's SmartUSB56, the pico-serial Line and Agere's USB 2.0 soft modem) go through libusb's IOKit backend, built with dmodemd.  libdmodem (`dmodemd/dmodem.h`), `libdmodem_sip.a`, dmodem-ari and the test programs build too.

Not built for macOS:
- the ALSA line (`-a`) and `hda-modem-setup`, which are Linux's
- the 32-bit interop test peers (`dsp/test`'s `half_*`): macOS runs no 32-bit programs

The Asterisk and FreeSWITCH modules build if their headers are found, as on Linux.

**Status:** the macOS build has not run on a Mac yet.  The development environment has no Apple SDK, and the repository's CI has had no runners for a while.  The `macos` CI job (an Apple silicon runner: a universal build with warnings as errors, the offline tests and the USB modems' calls) is ready for when it has.  What could be checked on Linux was:
- the code paths macOS takes where it lacks Linux's calls: a Linux build with `-DDMODEM_PORTABLE_FALLBACKS` passes the offline tests and the USB calls
- the USB calls with the fake modems linked into dmodemd, as on macOS: `-DDMODEM_TEST_BUILTIN_FAKES=ON` on Linux, the `dmodemd_*_builtin` tests
- the test scripts with BSD's `date`, `head` and no `nproc`

## Building

Apple's command line tools (`xcode-select --install`) and CMake, with Ninja if you like (Homebrew: `brew install cmake ninja`):

    cmake -B build -G Ninja
    cmake --build build
    ctest --test-dir build -L quick

libre (SIP), Mbed TLS (libre's digests: macOS has no OpenSSL for programs to link) and libusb are fetched and built while configuring, for every architecture of the build, so nothing else is needed from Homebrew.  `-DDMODEM_USB=OFF` builds dmodemd without `--usb` (and without libusb).

For both architectures in one binary, and an older or newer oldest macOS:

    cmake -B build -G Ninja -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
          -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON

(Homebrew's GoogleTest is for one architecture: with it disabled, the tests' GoogleTest is fetched and built for both.)

`cmake --install build` puts dmodemd in `/usr/local/sbin` and the Agere modem's ROM patches in `/usr/local/share/dmodem/firmware`.

## Running

dmodemd takes the same options as on Linux ([README](../README.md)).  The differences:

- **The pty.**  The pty is one of macOS's (`/dev/ttys<n>`).  Started as root, dmodemd also makes the link `/dev/ttyDM<n>` to it, then goes on as `nobody` as on Linux; if `/dev` refuses the link, use the slave's path that dmodemd prints (`Use '...' as modem device`).
- **Real-time scheduling.**  macOS has no `SCHED_FIFO` for a process and no `mlockall()`.  Without `-n` the line's thread gets Mach's time constraint policy (10 ms periods, up to 5 ms of each), as audio threads have it; nothing needs root for it, and no memory is locked.  The SIP user agent's threads keep the normal priority.
- **The data file.**  The modem's data file is `/var/lib/dmodem/data.<name>` (`/var` is `/private/var`); make the directory first: `sudo mkdir -p /var/lib/dmodem`.
- **USB.**  libusb drives a USB modem through IOKit, with no kernel extension: none of the three modems has a macOS driver that claims it.  The SmartUSB56 streams over isochronous endpoints, the pico-serial Line and the Agere modem over bulk ones.  None has been tried on a Mac yet.

## The tests

The offline tests (`ctest -L quick`) run as on Linux.  The USB modems' calls (`dmodemd_usb`, `dmodemd_pico_line`, `dmodemd_agere`) need no LD_PRELOAD on macOS: each fake modem (`dmodemd/test/fake_*.cpp`) is linked into a dmodemd of its own (`dmodemd_fake_st7554`, `dmodemd_fake_pico_line`, `dmodemd_fake_agere`) in libusb's place, as Windows' tests have them.  The ALSA line's test is Linux's.

## What the port changed

Most of the tree is POSIX already; `dmodemd/portable.h` had macOS's ways of closing descriptors on exec, sends without SIGPIPE, the monotonic sleep and a peer's hang-up.  For macOS:

- **libusb's IOKit backend** (`cmake/libusb.cmake`): the release's own `Xcode/config.h`, linked with IOKit, CoreFoundation, Security and the Objective-C runtime, as libusb's configure does.
- **libre** links SystemConfiguration and CoreFoundation (its DNS servers); its and Mbed TLS's builds get the build's architectures (a list, passed as one argument).
- **Mbed TLS** by default (`DMODEM_CRYPTO`), and macOS 11 as the oldest (`CMAKE_OSX_DEPLOYMENT_TARGET`) unless asked otherwise.
- **Real-time scheduling** through Mach's time constraint policy (`dmodemd/main.cpp`).
- **`sprintf()`** is deprecated in Apple's SDK: `snprintf()`, bounded, everywhere.
- **The test helpers**: no `clock_nanosleep()` (a `nanosleep()` for what is left), `MSG_NOSIGNAL` or SO_NOSIGPIPE, and `test_run.sh` and `test_logs.sh` without GNU `date`'s `%N`, `head -n -<n>`, `nproc` or `/proc`.
