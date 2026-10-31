# D-Modem on macOS

D-Modem's daemon, `dmodemd`, builds for macOS 11 (Big Sur) and later, on
both Apple silicon and Intel Macs. It is the same modem as on Linux: it
makes calls over SIP (internet telephony), handles data and fax, and
gives you a serial device to talk to it with AT commands. The USB modems
D-Modem supports (Smart Link's SmartUSB56, the pico-serial sticks, and
Agere's USB modem) work too.

For the full technical description, see
[the technical details](macos-details.md).

> **Status:** the macOS version has not been run on a real Mac yet.
> It has been checked as far as possible from Linux, and an automated
> test on a Mac is ready to run once the project's build servers are
> available again.

## What is missing on macOS

- The ALSA line (`-a`) for modem chips inside a PC: that is Linux only.
- The 32-bit test peers, because macOS no longer runs 32-bit programs.

## Building

You need Apple's command line tools (`xcode-select --install`) and
CMake. Ninja is optional (`brew install cmake ninja`).

    cmake -B build -G Ninja
    cmake --build build
    ctest --test-dir build -L quick

Everything else D-Modem needs is downloaded and built for you. To build
one program that runs on both kinds of Mac:

    cmake -B build -G Ninja -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
          -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON

`cmake --install build` installs `dmodemd` in `/usr/local/sbin`.

## Running

`dmodemd` takes the same options as on Linux (see the main
[README](../README.md)). A few things differ:

- **The serial device.** Run as root, `dmodemd` creates `/dev/ttyDM0`
  for you. If macOS refuses, use the device path it prints at start-up.
- **The data folder.** Create it once before the first run:
  `sudo mkdir -p /var/lib/dmodem`.
- **Timing.** The modem asks macOS for audio-style real-time scheduling,
  which needs no special rights.
- **USB modems** need no driver; none has been tried on a Mac yet.
