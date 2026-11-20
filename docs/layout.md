# Source layout

- `core/`, `dsp/`, `link/`, `include/`: the modem (libdmodem). These folders contain the modem core and its AT commands, the data pumps, and the data link layer.
- `frontend/`: the programs and modules that use the modem. These are `dmodemd`, the Asterisk, FreeSWITCH and FreePBX modules, `dmodem-ari` and `dmodem-capid` ([frontend/README.md](../frontend/README.md)).
- `platforms/`: the items that are necessary for only one operating system, in `haiku/`, `macos/` and `windows/`. These are toolchain files, build settings, the POSIX headers for MSVC and the Windows driver ([platforms/README.md](../platforms/README.md)).
- `cmake/`: the shared modules and test scripts of the build.
- `docs/`: the documentation.

Each program builds into `build/<its name>/` (`build/dmodemd/dmodemd`, `build/asterisk/...`).
