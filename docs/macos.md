# D-Modem on macOS

The daemon of D-Modem, `dmodemd`, builds for macOS 11 (Big Sur) and later, on
Apple silicon Macs and on Intel Macs. It is the same modem as on Linux. It
makes calls over SIP (internet telephony) and it sends and receives data and fax.
It gives you a serial device, so that you can control it with AT commands. The USB modems
that D-Modem supports (the SmartUSB56 of Smart Link, the pico-serial sticks, and
the USB modem of Agere) also operate on macOS.

For the full technical description, refer to
[the technical details](macos-details.md).

> **Status:** the automated build servers of the project build and test the
> macOS version on a Mac (the Apple silicon machines of GitHub). The tests include
> calls through simulated USB modems. Nobody tried it manually
> on a Mac yet, or with a real USB modem.

## What is missing on macOS

- The ALSA line (`-a`) for modem chips inside a PC. This line is for Linux only.
- The 32-bit test peers, because macOS does not run 32-bit programs now.

## Building

You must have the command line tools of Apple (`xcode-select --install`) and
CMake. Ninja is optional (`brew install cmake ninja`).

    cmake -B build -G Ninja
    cmake --build build
    ctest --test-dir build -L quick

The build downloads and builds all the other items that D-Modem must have. To build
one program that runs on the two types of Mac:

    cmake -B build -G Ninja -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
          -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON

`cmake --install build` installs `dmodemd` in `/usr/local/sbin`.

## Running

`dmodemd` accepts the same options as on Linux (refer to
[Using dmodemd](usage.md)). Some items are different:

- **The serial device.** If `dmodemd` runs as root, it makes `/dev/ttyDM0`.
  If macOS refuses, use the device path that `dmodemd` shows at start-up.
- **The data folder.** Make it one time before the first run:
  `sudo mkdir -p /var/lib/dmodem`.
- **Timing.** The modem asks macOS for real-time scheduling of the audio type.
  This scheduling does not need special rights.
- **USB modems** do not need a driver. Nobody tried a USB modem on a Mac yet.
