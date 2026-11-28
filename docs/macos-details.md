# D-Modem on macOS: technical details

This page gives the technical details for [D-Modem on macOS](macos.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

dmodemd builds for macOS 11 (Big Sur) and later, on Apple silicon and on Intel Macs. If you ask for it, the build makes one universal binary that runs on the two types.

It is the same modem as on Linux. It has these parts:
- SIP (internet telephony) through the libre library.
- The open DSP: data up to V.34, V.90/V.92 as the digital modem, class 1 fax and voice.
- T.38 fax over IP.
- A pty (pseudo-terminal) as its DTE. This is the serial device to which programs send AT commands.

The USB modems (`--usb`) also operate: the SmartUSB56 of Smart Link, the pico-serial Line and WB sticks, the USB 2.0 soft modem of Agere, the Apple USB Modem and the Conexant USB HSF modems. dmodemd connects to them through the IOKit backend of libusb, which builds together with dmodemd.

These items also build on macOS:
- libdmodem (`frontend/dmodemd/dmodem.h`).
- `libdmodem_sip.a`.
- dmodem-ari.
- The test programs.

The Asterisk and FreeSWITCH modules build if the build finds their headers, as on Linux.

These items do not build for macOS:
- The ALSA line (`-a`), `--misdn` and `hda-modem-setup`. They are only for Linux.
- The 32-bit interop test peers (the `half_*` of `dsp/test`). macOS runs no 32-bit programs.

## Status

Nobody has run the build manually on a Mac. Nobody has tried the USB modems on a Mac.

The build has run on the macOS runners of GitHub. The `macos` job of the CI (`.github/workflows/ci.yml`) runs on an Apple silicon runner, now `macos-latest`. It does these steps:
1. It makes a universal build (arm64 and x86_64) with warnings as errors.
2. It does a check with `lipo` that `dmodemd` contains the two architectures.
3. It runs the offline tests (`ctest -L quick`).
4. It runs the calls of four USB modems against the fake modems (`dmodemd_usb`, `dmodemd_pico_line`, `dmodemd_pico_wb`, `dmodemd_agere`). If a call fails, the job tries it one more time. The job does not run `dmodemd_apple` and `dmodemd_cadmus2`.
5. It keeps `dmodemd` as an artifact (`dmodemd-macos-universal`).

The commit history shows that this job ran, and that we repaired its failures. Examples are the link of libre with libresolv, the configuration of the pty through its slave, real-time scheduling for the USB tests, and the 8 KiB socket buffer of the voice test. The repository does not record the result of its latest run. Refer to the page of the job on GitHub.

Before the CI job ran, we did these checks on Linux:
- The code paths that macOS uses where it does not have the system calls of Linux. A Linux build with `-DDMODEM_PORTABLE_FALLBACKS` passes the offline tests and the USB calls.
- The USB calls with the fake modems linked into dmodemd, as on macOS. On Linux, `-DDMODEM_TEST_BUILTIN_FAKES=ON` adds the `dmodemd_*_builtin` tests.
- The test scripts with the `date` and `head` of BSD, and with no `nproc`.

## Build

You must have the command line tools of Apple (`xcode-select --install`) and CMake. Ninja is optional. With Homebrew:

    brew install cmake ninja

Then:

    cmake -B build -G Ninja
    cmake --build build
    ctest --test-dir build -L quick

The configure step downloads and builds three libraries, for each architecture of the build. Thus, no other items from Homebrew are necessary:
- libre, for SIP.
- Mbed TLS, for the digests of libre. This is the default of `DMODEM_CRYPTO`, `mbedtls`, on all systems. Also, macOS has no OpenSSL that programs can link.
- libusb, for `--usb`.

`-DDMODEM_USB=OFF` builds dmodemd without `--usb` and without libusb.

The oldest macOS for the build is 11.0 (`CMAKE_OSX_DEPLOYMENT_TARGET`), unless you set a different value. To build the two architectures into one binary:

    cmake -B build -G Ninja -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
          -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON

The GoogleTest of Homebrew is built for only one architecture. If you disable it, the build downloads GoogleTest and builds it for the two architectures.

`cmake --install build` puts dmodemd in `/usr/local/sbin`.

## Start dmodemd

dmodemd has the same options as on Linux (refer to [Using dmodemd](usage.md)). These are the differences.

**The pty.** The pty is a pty of macOS (`/dev/ttys<n>`).
- If you start dmodemd as root, it also makes the link `/dev/ttyDM<n>` to it. Then it continues as the user `nobody`, as on Linux.
- If `/dev` refuses the link, use the path that dmodemd shows at start-up (``Use `...' as modem device``).

**Real-time scheduling.** macOS has no `SCHED_FIFO` for a process and no `mlockall()`.
- Without `-n`, the thread of the line gets the time constraint policy of Mach, as audio threads do. This policy has a period of 10 ms, with up to 5 ms of computation in each period (`go_realtime()` in `frontend/dmodemd/main.cpp`).
- Root is not necessary for this, and no memory is locked.
- Threads that the program makes later, for example the thread of the SIP user agent, keep the normal priority.

**The data file.** The modem keeps its data file in `/var/lib/dmodem/data.<name>` (on macOS, `/var` is `/private/var`). Make the directory first:

    sudo mkdir -p /var/lib/dmodem

**USB.** libusb operates a USB modem through IOKit, with no kernel extension.
- The SmartUSB56, the pico-serial Line and WB and the Agere modem have no macOS driver that can claim them.
- The audio driver of macOS can possibly claim the audio interfaces of the Apple USB Modem. We did not try this.
- The SmartUSB56, the pico-serial Line and WB (the same transfers as the SmartUSB56), the Agere modem and the Apple USB Modem send their samples through isochronous endpoints. The Conexant USB HSF modems use bulk endpoints.
- Nobody has tried them on a Mac.

## The tests

The offline tests (`ctest -L quick`) run as on Linux.

On macOS, the calls of the USB modems do not use `LD_PRELOAD`:
- Each fake modem (`frontend/dmodemd/test/fake_*.cpp`) is linked into its own dmodemd in place of libusb: `dmodemd_fake_st7554`, `dmodemd_fake_pico_line`, `dmodemd_fake_pico_wb`, `dmodemd_fake_agere`, `dmodemd_fake_apple` and `dmodemd_fake_cadmus2`. The tests on Windows do the same.
- On macOS, these tests keep their Linux names: `dmodemd_usb`, `dmodemd_pico_line`, `dmodemd_pico_wb`, `dmodemd_agere`, `dmodemd_apple` and `dmodemd_cadmus2`.
- On macOS, they run dmodemd without `-n` (`frontend/dmodemd/test/usb_dte.py`). Without real-time scheduling, the CI runner started the loop of the line tens of milliseconds late, and the calls failed.
- The line programs of the test (`line_exchange`, `wb_pair`) ask for the same time constraint policy (`frontend/dmodemd/test/mac_realtime.h`).
- The `power/wakeup` check of the kernel is only for Linux, because only Linux has it.

The test of the ALSA line is only for Linux.

## Changes for macOS

Most of the tree was already POSIX. `frontend/dmodemd/portable.h` already had the macOS methods for four functions that Linux does in its own way:
- Close descriptors on exec.
- Send without a SIGPIPE.
- Sleep until a monotonic time.
- Find the hang-up of a peer without a read.

For macOS, we added these items:
- **The IOKit backend of libusb** (`cmake/libusb.cmake`): the `Xcode/config.h` of the release, linked with IOKit, CoreFoundation, Security and the Objective-C runtime, as the configure step of libusb does.
- **The system libraries of libre** (`cmake/libre.cmake`): SystemConfiguration and CoreFoundation for its DNS servers, and libresolv for `res_ninit()`. The builds of libre and Mbed TLS get the architectures of the build, as one argument.
- **Mbed TLS** as the default crypto (`DMODEM_CRYPTO`), and macOS 11 as the default oldest macOS (`CMAKE_OSX_DEPLOYMENT_TARGET`).
- **Real-time scheduling** through the time constraint policy of Mach (`frontend/dmodemd/main.cpp`).
- **The pty** (`frontend/dmodemd/pty.cpp`): a macOS pty master does not accept terminal settings before something opens its slave. Thus, dmodemd opens the slave one time while it configures the pty.
- **`snprintf()`, bounded, in all locations**, because `sprintf()` is deprecated in the SDK of Apple.
- **The test helpers**:
  - No `clock_nanosleep()`: a `nanosleep()` for the remaining time.
  - No `MSG_NOSIGNAL`: the helpers ignore SIGPIPE.
  - `cmake/test_run.sh` and `cmake/test_logs.sh` without the `%N` of GNU `date`, `head -n -<n>`, `nproc` or `/proc`.

## Mac OS X 10.4 to 10.6

The Windows app of D-Modem/gui builds for Mac OS X 10.4 (Tiger) and later, on PowerPC and on Intel. Its toolchain file is `cmake/macosx-legacy.cmake` in that repository. The library of the built-in modem (`frontend/embed`) builds with it. These items are for the versions before 10.7:
- **The digests of libre** (`cmake/libre-macosx-legacy.patch`): CommonCrypto starts in 10.5. Thus, libre uses the digests of Mbed TLS. `DMODEM_CRYPTO` must be `mbedtls`.
- **The random bytes of libre** (the same patch): `arc4random_buf()` starts in 10.7. Thus, libre uses `arc4random()`.
- **The thread names of libre** (the same patch): `pthread_setname_np()` starts in 10.6. Thus, the threads have no names before 10.6.
- **`fd_dup()`** (`frontend/dmodemd/portable.h`): `F_DUPFD_CLOEXEC` starts in 10.7. Thus, `fd_dup()` uses `F_DUPFD`, then sets `FD_CLOEXEC`.
- **`pts_name()`** (`frontend/dmodemd/pty.cpp`): `ptsname_r()` starts in 10.13.4. Before 10.13.4, `pts_name()` copies the result of `ptsname()`.
- **The PKDS value of LAPB** (`link/lapb.cpp`): the libc++ of Mac OS X 10.4 has no `std::random_device`. Thus, all macOS builds use `arc4random()`.

CMake applies the patch when `CMAKE_OSX_DEPLOYMENT_TARGET` is before 10.7. Nobody has run this build on a Mac yet.
