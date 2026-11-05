# Source layout

- `core/`, `dsp/`, `link/`, `include/`: the modem itself (libdmodem): the modem core and its AT commands, the data pumps, and the data link layer.
- `frontend/`: the programs and modules built on it: `dmodemd`, the Asterisk, FreeSWITCH and FreePBX modules, `dmodem-ari` and `dmodem-capid` ([frontend/README.md](../frontend/README.md)).
- `platforms/`: what one operating system alone needs, in `haiku/`, `macos/` and `windows/`: toolchain files, build settings, MSVC's POSIX headers and the Windows driver ([platforms/README.md](../platforms/README.md)).
- `cmake/`: the build's shared modules and test scripts; `docs/`: the documentation.

Each program still builds into `build/<its name>/` (`build/dmodemd/dmodemd`, `build/asterisk/...`).
