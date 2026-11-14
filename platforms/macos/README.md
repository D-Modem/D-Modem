# platforms/macos/

D-Modem on macOS 11 and later, on Apple silicon and Intel.

| File | What it does |
|------|--------------|
| `macos.cmake` | The build settings for macOS. CMake reads this file before `project()`. It sets the oldest macOS on which the programs run (11.0). |

The remaining parts of the macOS port are in the shared code, behind `__APPLE__`:

- The real-time scheduling of Mach and the pty, in `frontend/dmodemd/`.
- The libusb and Mbed TLS that the build downloads, in `cmake/`.

The details are in [docs/macos.md](../../docs/macos.md) and
[docs/macos-details.md](../../docs/macos-details.md).
