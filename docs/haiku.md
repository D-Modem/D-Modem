# D-Modem on Haiku

dmodemd builds for [Haiku](https://www.haiku-os.org/) (R1/beta5 and later, x86_64), BeOS's open successor.  It is the same modem as on Linux: SIP on libre, the open DSP (data up to V.34 and V.90/V.92 as the digital modem, class 1 fax, voice) and T.38, with a pty as its DTE.  The USB modems (`--usb`: Smart Link's SmartUSB56, the pico-serial Line and Agere's USB 2.0 soft modem) go through HaikuPorts' libusb.  libdmodem (`dmodemd/dmodem.h`), `libdmodem_sip.a`, dmodem-ari and the test programs build too.

Not built for Haiku:
- the ALSA line (`-a`) and `hda-modem-setup`, which are Linux's
- the Asterisk and FreeSWITCH modules: neither PBX is ported to Haiku

The build is checked from Linux (a cross build, warnings as errors).  It has not yet run on Haiku itself, so neither the tests nor a call have run there.

## Building on Haiku

Haiku has GCC and its headers (`haiku_devel`) already.  The rest comes from HaikuPorts:

    pkgman install cmake ninja git pkgconfig openssl3_devel libusb_devel
    cmake -B build -G Ninja
    cmake --build build
    ctest --test-dir build -L quick

libre (the SIP library) is fetched and built while configuring, with a small patch for Haiku (`cmake/libre-haiku.patch`).  Without `libusb_devel` dmodemd builds without `--usb`.

`cmake --install build` puts the programs in `/boot/system/non-packaged/bin` (on the `PATH`).  `/boot/system` itself is Haiku's packages', read only.

## Building on Linux

Clang and lld (17 or later) build for Haiku with a sysroot of Haiku's own packages.  `cmake/haiku_sysroot.py` makes one: it downloads the packages from Haiku's and HaikuPorts' repositories (once, into a cache beside it), checks each against its repository's SHA-256 and extracts it.  Haiku's package format is read there; its zstd-compressed packages need the system's libzstd.

    sudo apt-get install cmake ninja-build pkg-config clang lld libzstd1
    python3 cmake/haiku_sysroot.py ~/haiku-sysroot
    cmake -B build-haiku -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/haiku.cmake \
          -DHAIKU_SYSROOT=$HOME/haiku-sysroot
    cmake --build build-haiku

The sysroot is about 500 MB: the base system (`haiku`, `haiku_devel`), GCC's headers and libraries (`gcc`, `gcc_syslibs`, `gcc_syslibs_devel`: Clang uses its libstdc++), `libusb`, `libusb_devel`, `openssl3` and `openssl3_devel`.  The tests are built but not registered with ctest: nothing here runs Haiku's programs.  Copy the build to Haiku to run them.

## Running

dmodemd takes the same options as on Linux ([README](../README.md)).  The differences:

- **No users.**  Every process on Haiku has uid 0 and there is no other user to run as, so dmodemd does not give up root's rights after start-up and forks no keeper process for the pty's link.
- **The pty.**  The pty is one of Haiku's (`/dev/pt/<n>`, its slave `/dev/tt/<n>`).  dmodemd also tries the link `/dev/ttyDM<n>` to it; if Haiku's `/dev` refuses the link, use the slave's path that dmodemd prints (`Use '...' as modem device`).
- **Real-time scheduling.**  Haiku has no `mlockall()` or `SCHED_FIFO`.  Without `-n` the line's thread gets a real-time priority (`B_REAL_TIME_DISPLAY_PRIORITY`), which runs until it blocks, as `SCHED_FIFO` does.  The SIP user agent's threads keep the normal priority.
- **The data file.**  The modem's data file is `/var/lib/dmodem/data.<name>` (`/var` is `/boot/system/var`); make the directory first: `mkdir -p /var/lib/dmodem`.
- **USB.**  libusb drives a USB modem through Haiku's `usb_raw` driver.  The SmartUSB56 streams over isochronous endpoints, the pico-serial Line and the Agere modem over bulk ones.  None has been tried on Haiku yet.

## What the port changed

Most of the tree is POSIX already and builds unchanged.  For Haiku:

- **Executables are shared objects.**  Haiku's linkers make every executable a shared object (`-shared`), so CMake's `-fPIE` for position independent executables becomes `-fPIC` (`cmake/rules.cmake`, also for libre's build).
- **The sockets are libnetwork's**, not the C library's: every program links `-lnetwork`.
- **No `SOCK_CLOEXEC`, `MSG_CMSG_CLOEXEC` or `ptsname_r()`.**  Descriptors are made close-on-exec with `fcntl()`, and a pty's slave is named with `ptsname()`.  `dmodemd/portable.h` already did the first on Apple's systems.
- **No `<sys/syscall.h>`**: descriptors are closed one by one, without `close_range()`.
- **libre** (`cmake/libre-haiku.patch`): Haiku's interface ioctls are in `<sys/sockio.h>`, its `struct ifreq` has no `ifr_ifru`, and it has no routing table listing (libre's own stub, as on iOS).
- **Install layout**: `/boot/system/non-packaged`, `data/` for `share/`, `bin/` for `sbin/`.
