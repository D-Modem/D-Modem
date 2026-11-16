# D-Modem on Haiku

The daemon of D-Modem, `dmodemd`, builds for [Haiku](https://www.haiku-os.org/).
Haiku is the open-source successor to BeOS (R1/beta5 and later, 64-bit).
It is the same modem as on Linux. It makes calls through SIP (internet
telephony) and it sends data and fax. It gives you a serial device to
send AT commands to it. The USB modems that D-Modem supports also operate.

For the full technical description, refer to
[the technical details](haiku-details.md).

> **Status:** A Linux job does an automatic check of the Haiku build. But
> nobody has run it on Haiku. Thus, no calls or tests have run on Haiku.

## What is missing on Haiku

- The ALSA line (`-a`) for modem chips in a PC. This is only for Linux.
- The Asterisk and FreeSWITCH modules. These telephone systems do not
  run on Haiku.

## Building on Haiku

1. Install the tools and libraries from HaikuPorts.
2. Build:

        pkgman install cmake ninja git pkgconfig libusb_devel
        cmake -B build -G Ninja
        cmake --build build
        ctest --test-dir build -L quick

`cmake --install build` puts the programs in
`/boot/system/non-packaged/bin`. This folder is already on your `PATH`.

## Building on Linux for Haiku

You can also build Haiku programs on a Linux machine. A script downloads
the packages of Haiku (about 500 MB) for the build:

    sudo apt-get install cmake ninja-build pkg-config clang lld libzstd1
    python3 platforms/haiku/haiku_sysroot.py ~/haiku-sysroot
    cmake -B build-haiku -G Ninja -DCMAKE_TOOLCHAIN_FILE=platforms/haiku/haiku.cmake \
          -DHAIKU_SYSROOT=$HOME/haiku-sysroot
    cmake --build build-haiku

To run the result, copy it to a Haiku machine.

## Running

`dmodemd` has the same options as on Linux (refer to
[Using dmodemd](usage.md)). Some items are different:

- **No separate users.** On Haiku, all programs run with full rights.
  Thus, `dmodemd` does not change to a user with fewer rights.
- **The serial device.** `dmodemd` tries to make `/dev/ttyDM0`. If Haiku
  does not let it, use the device path that it shows at start-up.
- **The data folder.** Make it one time before the first run:
  `mkdir -p /var/lib/dmodem`.
- **USB modems.** Nobody has tried them on Haiku.
