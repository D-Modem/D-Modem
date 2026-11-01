# D-Modem on Haiku: technical details

This is the technical companion to [D-Modem on Haiku](haiku.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

dmodemd builds for [Haiku](https://www.haiku-os.org/), the open successor to BeOS: R1/beta5 and later, on x86_64.

It is the same modem as on Linux:
- SIP (internet telephony) through the libre library;
- the open DSP: data up to V.34, V.90/V.92 as the digital modem, class 1 fax and voice;
- T.38 fax over IP;
- a pty (pseudo-terminal) as its DTE, the serial device that programs talk AT commands to.

The USB modems (`--usb`) are built in too: Smart Link's SmartUSB56, the pico-serial Line and WB sticks, and Agere's USB 2.0 soft modem. dmodemd reaches them through HaikuPorts' libusb.

These also build for Haiku:
- libdmodem (`frontend/dmodemd/dmodem.h`);
- `libdmodem_sip.a`;
- dmodem-ari;
- the test programs.

These are not built for Haiku:
- The ALSA line (`-a`), `--misdn` and `hda-modem-setup`: they are Linux's own.
- The Asterisk and FreeSWITCH modules: neither PBX has been ported to Haiku.

## Status

The build is checked from Linux: CI's `haiku` job (`.github/workflows/ci.yml`) cross-builds it with Clang, with warnings as errors, and keeps `dmodemd` as an artifact (`dmodemd-haiku-x86_64`).

Nothing has run on Haiku itself yet: neither the tests nor a call.

## Building on Haiku

Haiku already has GCC and its headers (`haiku_devel`). The rest comes from HaikuPorts:

    pkgman install cmake ninja git pkgconfig openssl3_devel libusb_devel
    cmake -B build -G Ninja
    cmake --build build
    ctest --test-dir build -L quick

libre (the SIP library) is fetched and built while configuring, with a small patch for Haiku (`platforms/haiku/libre-haiku.patch`). Without `libusb_devel`, dmodemd builds without `--usb`.

`cmake --install build` puts the programs in `/boot/system/non-packaged/bin`, which is on the `PATH`. `/boot/system` itself belongs to Haiku's packages and is read-only.

## Building on Linux

Clang and lld (version 17 or later) can build for Haiku against a sysroot made of Haiku's own packages. `platforms/haiku/haiku_sysroot.py` makes one:
1. It downloads the packages from Haiku's and HaikuPorts' repositories, once, into a cache (by default `haiku-packages` beside the sysroot).
2. It checks each package against its repository's SHA-256.
3. It extracts each one under `boot/system`, as Haiku installs it.

The script reads Haiku's package format itself. Its zstd-compressed packages need the system's libzstd.

    sudo apt-get install cmake ninja-build pkg-config clang lld libzstd1
    python3 platforms/haiku/haiku_sysroot.py ~/haiku-sysroot
    cmake -B build-haiku -G Ninja -DCMAKE_TOOLCHAIN_FILE=platforms/haiku/haiku.cmake \
          -DHAIKU_SYSROOT=$HOME/haiku-sysroot
    cmake --build build-haiku

The sysroot is about 500 MB. It holds:
- the base system: `haiku`, `haiku_devel`;
- GCC's headers and libraries: `gcc`, `gcc_syslibs`, `gcc_syslibs_devel` (Clang uses GCC's libstdc++);
- `libusb` and `libusb_devel`;
- `openssl3` and `openssl3_devel`.

The tests are built but not registered with ctest, because nothing on Linux can run Haiku's programs (`cmake/test_logs.cmake`). Copy the build to Haiku to run them.

## Running

dmodemd takes the same options as on Linux (see the [README](../README.md)). The differences are these.

**No users.** Every process on Haiku has uid 0, and there is no other user to run as. So dmodemd does not give up root's rights after start-up (`started_as_root()` in `frontend/dmodemd/main.cpp` is always false there), and it forks no keeper process for the pty's link.

**The pty.** The pty is one of Haiku's (`/dev/pt/<n>`, with its slave at `/dev/tt/<n>`).
- dmodemd also tries to make the link `/dev/ttyDM<n>` to it.
- If Haiku's `/dev` refuses the link, use the path that dmodemd prints at start-up (``Use `...' as modem device``).

**Real-time scheduling.** Haiku has no `mlockall()` and no `SCHED_FIFO`.
- Without `-n`, the line's thread gets a real-time priority (`B_REAL_TIME_DISPLAY_PRIORITY`). Such a thread runs until it blocks, as a `SCHED_FIFO` one does.
- Threads made later, such as the SIP user agent's, keep the normal priority.

**The data file.** The modem keeps its data file in `/var/lib/dmodem/data.<name>` (on Haiku `/var` is `/boot/system/var`). Make the directory first:

    mkdir -p /var/lib/dmodem

**USB.** libusb drives a USB modem through Haiku's `usb_raw` driver.
- All four modems stream their samples over isochronous endpoints: the SmartUSB56, the pico-serial Line and WB (the same transfers as the SmartUSB56's) and the Agere modem.
- None has been tried on Haiku yet.

## What the port changed

Most of the tree was POSIX already and builds unchanged. For Haiku:

- **Executables are shared objects.** Haiku's linkers make every executable a shared object (`-shared`). So CMake's `-fPIE` for position-independent executables becomes `-fPIC` (`cmake/rules.cmake`, which libre's build uses too).
- **The sockets are libnetwork's**, not the C library's, so every program links `-lnetwork`.
- **No `SOCK_CLOEXEC`, `MSG_CMSG_CLOEXEC` or `ptsname_r()`.** Descriptors are made close-on-exec with `fcntl()`, and a pty's slave is named with `ptsname()`. `frontend/dmodemd/portable.h` already did the first on Apple's systems.
- **No `<sys/syscall.h>`**, so descriptors are closed one by one, without `close_range()` (`frontend/dmodemd/pty.cpp`).
- **libre** (`platforms/haiku/libre-haiku.patch`):
  - Haiku's interface ioctls are in `<sys/sockio.h>`;
  - its `struct ifreq` has no `ifr_ifru`;
  - it has no way to list the routing table, so libre's own stub is used, as on iOS.
- **The install layout**: the prefix `/boot/system/non-packaged`, `data/` in place of `share/`, and `bin/` in place of `sbin/`.
