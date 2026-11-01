# D-Modem on Haiku

D-Modem's daemon, `dmodemd`, builds for [Haiku](https://www.haiku-os.org/),
the open-source successor to BeOS (R1/beta5 and later, 64-bit). It is the
same modem as on Linux: it makes calls over SIP (internet telephony),
handles data and fax, and gives you a serial device to talk to it with AT
commands. The USB modems D-Modem supports work too.

For the full technical description, see
[the technical details](haiku-details.md).

> **Status:** the Haiku build is checked automatically from Linux, but it
> has not been run on Haiku itself yet, so no calls or tests have run
> there.

## What is missing on Haiku

- The ALSA line (`-a`) for modem chips inside a PC: that is Linux only.
- The Asterisk and FreeSWITCH modules, because neither phone system runs
  on Haiku.

## Building on Haiku

Install the tools and libraries from HaikuPorts, then build:

    pkgman install cmake ninja git pkgconfig openssl3_devel libusb_devel
    cmake -B build -G Ninja
    cmake --build build
    ctest --test-dir build -L quick

`cmake --install build` puts the programs in
`/boot/system/non-packaged/bin`, which is already on your `PATH`.

## Building on Linux for Haiku

You can also build Haiku programs on a Linux machine. A script downloads
Haiku's own packages (about 500 MB) to build against:

    sudo apt-get install cmake ninja-build pkg-config clang lld libzstd1
    python3 platforms/haiku/haiku_sysroot.py ~/haiku-sysroot
    cmake -B build-haiku -G Ninja -DCMAKE_TOOLCHAIN_FILE=platforms/haiku/haiku.cmake \
          -DHAIKU_SYSROOT=$HOME/haiku-sysroot
    cmake --build build-haiku

Copy the result to a Haiku machine to run it.

## Running

`dmodemd` takes the same options as on Linux (see the main
[README](../README.md)). A few things differ:

- **No separate users.** Everything on Haiku runs with full rights, so
  `dmodemd` does not switch to a less privileged user.
- **The serial device.** `dmodemd` tries to create `/dev/ttyDM0`. If Haiku
  refuses, use the device path it prints at start-up.
- **The data folder.** Create it once before the first run:
  `mkdir -p /var/lib/dmodem`.
- **USB modems** have not been tried on Haiku yet.
