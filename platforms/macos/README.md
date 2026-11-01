# platforms/macos/

D-Modem on macOS 11 and later, on Apple silicon and Intel.

| File | What it does |
|------|--------------|
| `macos.cmake` | macOS's build settings, read before `project()`: the oldest macOS to run on (11.0) |

The rest of the macOS port is in the shared code, behind `__APPLE__`:
Mach's real-time scheduling and the pty in `frontend/dmodemd/`, and the
fetched libusb and Mbed TLS in `cmake/`.  The details are in
[docs/macos.md](../../docs/macos.md) and
[docs/macos-details.md](../../docs/macos-details.md).
