# D-Modem on macOS: technical details

This is the technical companion to [D-Modem on macOS](macos.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

dmodemd builds for macOS 11 (Big Sur) and later, on Apple silicon and on Intel Macs. If you ask for it, the build makes one universal binary that runs on both.

It is the same modem as on Linux:
- SIP (internet telephony) through the libre library;
- the open DSP: data up to V.34, V.90/V.92 as the digital modem, class 1 fax and voice;
- T.38 fax over IP;
- a pty (pseudo-terminal) as its DTE, the serial device that programs talk AT commands to.

The USB modems (`--usb`) work too: Smart Link's SmartUSB56, the pico-serial Line and WB sticks, and Agere's USB 2.0 soft modem. dmodemd reaches them through libusb's IOKit backend, which is built along with dmodemd.

These also build on macOS:
- libdmodem (`dmodemd/dmodem.h`);
- `libdmodem_sip.a`;
- dmodem-ari;
- the test programs.

The Asterisk and FreeSWITCH modules build if their headers are found, as on Linux.

These are not built for macOS:
- The ALSA line (`-a`), `--misdn` and `hda-modem-setup`: they are Linux's own.
- The 32-bit interop test peers (`dsp/test`'s `half_*`): macOS runs no 32-bit programs.

## Status

The build has not yet been run on a Mac by hand, and none of the USB modems has been tried on one.

It has run on GitHub's macOS runners. CI's `macos` job (`.github/workflows/ci.yml`) runs on an Apple silicon runner, now `macos-latest`. It:
1. makes a universal build (arm64 and x86_64) with warnings as errors;
2. checks with `lipo` that `dmodemd` holds both architectures;
3. runs the offline tests (`ctest -L quick`);
4. runs the USB modems' calls against the fake modems (`dmodemd_usb`, `dmodemd_pico_line`, `dmodemd_pico_wb`, `dmodemd_agere`), each retried once if it fails;
5. keeps `dmodemd` as an artifact (`dmodemd-macos-universal`).

The commit history shows this job running and its failures being fixed. Examples: libre's link against libresolv, the pty set up through its slave, real-time scheduling for the USB tests, and the voice test's 8 KiB socket buffer. Whether its latest run passed is not recorded in the repository: see the job's page on GitHub.

Before the CI job ran, these checks were made on Linux:
- The code paths macOS takes where it lacks Linux's system calls: a Linux build with `-DDMODEM_PORTABLE_FALLBACKS` passes the offline tests and the USB calls.
- The USB calls with the fake modems linked into dmodemd, as macOS has them: `-DDMODEM_TEST_BUILTIN_FAKES=ON` on Linux, which adds the `dmodemd_*_builtin` tests.
- The test scripts with BSD's `date` and `head`, and with no `nproc`.

## Building

You need Apple's command line tools (`xcode-select --install`) and CMake. Ninja is optional. With Homebrew:

    brew install cmake ninja

Then:

    cmake -B build -G Ninja
    cmake --build build
    ctest --test-dir build -L quick

Three libraries are fetched and built while configuring, for every architecture of the build, so nothing else is needed from Homebrew:
- libre, for SIP;
- Mbed TLS, for libre's digests (macOS has no OpenSSL that programs may link; `DMODEM_CRYPTO` defaults to `mbedtls` on Apple's systems);
- libusb, for `--usb`.

`-DDMODEM_USB=OFF` builds dmodemd without `--usb` and without libusb.

The oldest macOS the build targets is 11.0 (`CMAKE_OSX_DEPLOYMENT_TARGET`), unless you set another. To build both architectures into one binary:

    cmake -B build -G Ninja -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
          -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON

Homebrew's GoogleTest is built for one architecture only. With it disabled, the build fetches GoogleTest and builds it for both.

`cmake --install build` puts dmodemd in `/usr/local/sbin`.

## Running

dmodemd takes the same options as on Linux (see the [README](../README.md)). The differences are these.

**The pty.** The pty is one of macOS's (`/dev/ttys<n>`).
- Started as root, dmodemd also makes the link `/dev/ttyDM<n>` to it, then goes on as the user `nobody`, as on Linux.
- If `/dev` refuses the link, use the path that dmodemd prints at start-up (``Use `...' as modem device``).

**Real-time scheduling.** macOS has no `SCHED_FIFO` for a process and no `mlockall()`.
- Without `-n`, the line's thread gets Mach's time constraint policy, as audio threads do: a period of 10 ms, with up to 5 ms of computation in each (`go_realtime()` in `dmodemd/main.cpp`).
- This needs no root, and no memory is locked.
- Threads made later, such as the SIP user agent's, keep the normal priority.

**The data file.** The modem keeps its data file in `/var/lib/dmodem/data.<name>` (on macOS `/var` is `/private/var`). Make the directory first:

    sudo mkdir -p /var/lib/dmodem

**USB.** libusb drives a USB modem through IOKit, with no kernel extension: none of the four modems has a macOS driver that would claim it.
- All four stream their samples over isochronous endpoints: the SmartUSB56, the pico-serial Line and WB (the same transfers as the SmartUSB56's) and the Agere modem.
- None has been tried on a Mac yet.

## The tests

The offline tests (`ctest -L quick`) run as on Linux.

The USB modems' calls need no `LD_PRELOAD` on macOS:
- Each fake modem (`dmodemd/test/fake_*.cpp`) is linked into a dmodemd of its own in libusb's place: `dmodemd_fake_st7554`, `dmodemd_fake_pico_line`, `dmodemd_fake_pico_wb` and `dmodemd_fake_agere`. Windows' tests do the same.
- On macOS these tests keep their Linux names: `dmodemd_usb`, `dmodemd_pico_line`, `dmodemd_pico_wb` and `dmodemd_agere`.
- They run dmodemd without `-n` on macOS (`dmodemd/test/usb_dte.py`). Without real-time scheduling the CI runner woke the line's loop tens of milliseconds late, and the calls failed.
- The test's line programs (`line_exchange`, `wb_pair`) ask for the same time constraint policy (`dmodemd/test/mac_realtime.h`).
- The kernel's `power/wakeup` check is Linux's alone, since only Linux has it.

The ALSA line's test is Linux's.

## What the port changed

Most of the tree was POSIX already. `dmodemd/portable.h` already had macOS's ways of doing four things that Linux does its own way:
- closing descriptors on exec;
- sending without raising SIGPIPE;
- sleeping until a monotonic time;
- seeing a peer's hang-up without reading.

For macOS the port added:
- **libusb's IOKit backend** (`cmake/libusb.cmake`): the release's own `Xcode/config.h`, linked with IOKit, CoreFoundation, Security and the Objective-C runtime, as libusb's configure does.
- **libre's system libraries** (`cmake/libre.cmake`): SystemConfiguration and CoreFoundation for its DNS servers, and libresolv for `res_ninit()`. libre's and Mbed TLS's builds get the build's architectures, passed as one argument.
- **Mbed TLS** as the default crypto (`DMODEM_CRYPTO`), and macOS 11 as the default oldest macOS (`CMAKE_OSX_DEPLOYMENT_TARGET`).
- **Real-time scheduling** through Mach's time constraint policy (`dmodemd/main.cpp`).
- **The pty** (`dmodemd/pty.cpp`): a macOS pty master takes no terminal settings before its slave has been opened, so dmodemd opens the slave once while it sets the pty up.
- **`snprintf()`, bounded, everywhere**, since `sprintf()` is deprecated in Apple's SDK.
- **The test helpers**:
  - no `clock_nanosleep()`: a `nanosleep()` for the time that is left;
  - no `MSG_NOSIGNAL`: SIGPIPE is ignored instead;
  - `cmake/test_run.sh` and `cmake/test_logs.sh` without GNU `date`'s `%N`, `head -n -<n>`, `nproc` or `/proc`.
