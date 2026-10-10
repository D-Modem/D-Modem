# slmodemd's old modem core

Smart Link's slmodemd core in C (now C++ files), as it was before
`../../../core` replaced it: `modem.cpp` (states, data pumps, rings),
`modem_at.cpp` (AT commands), `modem_param.cpp` (the pumps' parameters),
the signal timers (`modem_timer.cpp`, or libdmodem's old ones,
`dmodem_timer.cpp`), the debug output, `modem_cmdline.cpp` and
`modem_half.cpp`.

Nothing ships with it. It is here for two tests:

- `half_blob`/`half_blob8` (`../CMakeLists.txt`): Smart Link's original
  binary-only DSP library (`../dsplibs.o`), the interop tests' peer, was
  written against this core and calls into it.
- `core_script_old` (`../../../core/test`): the same scripts on this core
  and on the new one must give the same transcripts.

The licence is Smart Link's (`COPYING`).
