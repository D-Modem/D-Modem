# D-Modem on Haiku: technical details

This page gives the technical details for [D-Modem on Haiku](haiku.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

dmodemd builds for [Haiku](https://www.haiku-os.org/), the open successor to BeOS: R1/beta5 and later, on x86_64.

It is the same modem as on Linux. It has these parts:
- SIP (internet telephony) through the libre library.
- The open DSP: data up to V.34, V.90/V.92 as the digital modem, class 1 fax and voice.
- T.38 fax over IP.
- A pty (pseudo-terminal) as its DTE. This is the serial device to which programs send AT commands.

The USB modems (`--usb`) are also built in: the SmartUSB56 of Smart Link, the pico-serial Line and WB sticks, and the USB 2.0 soft modem of Agere. dmodemd connects to them through the libusb of HaikuPorts.

These items also build for Haiku:
- libdmodem (`frontend/dmodemd/dmodem.h`).
- `libdmodem_sip.a`.
- dmodem-ari.
- The test programs.

These items do not build for Haiku:
- The ALSA line (`-a`), `--misdn` and `hda-modem-setup`. They are only for Linux.
- The Asterisk and FreeSWITCH modules. Nobody has ported these PBXs to Haiku.

## Status

A Linux job does a check of the build. The `haiku` job of the CI (`.github/workflows/ci.yml`) cross-builds it with Clang, with warnings as errors. It keeps `dmodemd` as an artifact (`dmodemd-haiku-x86_64`).

Nothing has run on Haiku: no tests and no calls.

## Building on Haiku

Haiku already has GCC and its headers (`haiku_devel`). The other items come from HaikuPorts:

    pkgman install cmake ninja git pkgconfig libusb_devel
    cmake -B build -G Ninja
    cmake --build build
    ctest --test-dir build -L quick

The configure step downloads and builds libre (the SIP library) and Mbed TLS (its crypto). libre gets a small patch for Haiku (`platforms/haiku/libre-haiku.patch`). Without `libusb_devel`, dmodemd builds without `--usb`.

`cmake --install build` puts the programs in `/boot/system/non-packaged/bin`, which is on the `PATH`. `/boot/system` is part of the packages of Haiku and is read-only.

## Building on Linux

Clang and lld (version 17 or later) can build for Haiku with a sysroot that contains the packages of Haiku. `platforms/haiku/haiku_sysroot.py` makes a sysroot:
1. It downloads the packages from the repositories of Haiku and HaikuPorts, one time, into a cache (by default `haiku-packages` next to the sysroot).
2. It compares each package with the SHA-256 of its repository.
3. It extracts each package under `boot/system`, as Haiku installs it.

The script reads the package format of Haiku itself. For the zstd-compressed packages, the libzstd of the system is necessary.

    sudo apt-get install cmake ninja-build pkg-config clang lld libzstd1
    python3 platforms/haiku/haiku_sysroot.py ~/haiku-sysroot
    cmake -B build-haiku -G Ninja -DCMAKE_TOOLCHAIN_FILE=platforms/haiku/haiku.cmake \
          -DHAIKU_SYSROOT=$HOME/haiku-sysroot
    cmake --build build-haiku

The sysroot is about 500 MB. It contains:
- The base system: `haiku`, `haiku_devel`.
- The headers and libraries of GCC: `gcc`, `gcc_syslibs`, `gcc_syslibs_devel` (Clang uses the libstdc++ of GCC).
- `libusb` and `libusb_devel`.

The build makes the tests, but does not register them with ctest (`cmake/test_logs.cmake`). The reason is that Linux cannot run the programs of Haiku. To run the tests, copy the build to Haiku.

## Running

dmodemd has the same options as on Linux (refer to [Using dmodemd](usage.md)). These are the differences.

**No users.** On Haiku, all processes have uid 0, and there is no other user to run as. Thus, dmodemd does not give up the rights of root after start-up (`started_as_root()` in `frontend/dmodemd/main.cpp` is always false there). It also does not fork a keeper process for the link of the pty.

**The pty.** The pty is a pty of Haiku (`/dev/pt/<n>`, with its slave at `/dev/tt/<n>`).
- dmodemd also tries to make the link `/dev/ttyDM<n>` to it.
- If the `/dev` of Haiku refuses the link, use the path that dmodemd shows at start-up (``Use `...' as modem device``).

**Real-time scheduling.** Haiku has no `mlockall()` and no `SCHED_FIFO`.
- Without `-n`, the thread of the line gets a real-time priority (`B_REAL_TIME_DISPLAY_PRIORITY`). Such a thread runs until it blocks, as a `SCHED_FIFO` thread does.
- Threads that the program makes later, for example the thread of the SIP user agent, keep the normal priority.

**The data file.** The modem keeps its data file in `/var/lib/dmodem/data.<name>` (on Haiku, `/var` is `/boot/system/var`). Make the directory first:

    mkdir -p /var/lib/dmodem

**USB.** libusb operates a USB modem through the `usb_raw` driver of Haiku.
- The four modems send their samples through isochronous endpoints: the SmartUSB56, the pico-serial Line and WB (the same transfers as the SmartUSB56) and the Agere modem.
- Nobody has tried them on Haiku.

## What the port changed

Most of the tree was already POSIX, and it builds with no changes. For Haiku, these items changed:

- **Executables are shared objects.** The linkers of Haiku make all executables shared objects (`-shared`). Thus, the `-fPIE` of CMake for position-independent executables changes to `-fPIC` (`cmake/rules.cmake`, which the build of libre also uses).
- **The sockets come from libnetwork**, not from the C library. Thus, all programs link `-lnetwork`.
- **No `SOCK_CLOEXEC`, `MSG_CMSG_CLOEXEC` or `ptsname_r()`.** The code makes descriptors close-on-exec with `fcntl()`. It gets the name of the slave of a pty with `ptsname()`. `frontend/dmodemd/portable.h` already did the first item on the systems of Apple.
- **No `<sys/syscall.h>`.** Thus, the code closes descriptors one at a time, without `close_range()` (`frontend/dmodemd/pty.cpp`).
- **libre** (`platforms/haiku/libre-haiku.patch`):
  - The interface ioctls of Haiku are in `<sys/sockio.h>`.
  - Its `struct ifreq` has no `ifr_ifru`.
  - It has no method to list the routing table. Thus, libre uses its own stub, as on iOS.
- **The install layout**: the prefix `/boot/system/non-packaged`, `data/` in place of `share/`, and `bin/` in place of `sbin/`.
