# slmodemd's old modem core

Smart Link's slmodemd core in C (now C++ files), as it was before
`../../../core` replaced it: `modem.cpp` (states, data pumps, rings),
`modem_at.cpp` (AT commands), `modem_param.cpp` (the pumps' parameters),
the timers (libdmodem's old ones, `dmodem_timer.cpp`: their time set and
the ones due run by the program; slmodemd's on SIGALRM and the wall
clock, `modem_timer.cpp`, are no longer built), the debug output,
`modem_cmdline.cpp` and `modem_half.cpp`.

`modem_half.cpp` runs the timers between half frames of the line, on
its samples with `PIPE_SYNC` (the wall clock's otherwise), in real time
on hook and with `MODEM_SPEED` as `../modem_half.cpp` does, so that the
interop tests' calls with the original library run unpaced too.

Nothing ships with it. It is here for two tests:

- `half_smartlink`/`half_smartlink8` (`../CMakeLists.txt`): Smart Link's
  original binary-only DSP library (`../smartlink/dsplibs.o`), one of the
  interop tests' peers (`docs/interop.md`), was written against this core
  and calls into it.
- `core_script_old` (`../../../core/test`): the same scripts on this core
  and on the new one must give the same transcripts.

The licence is Smart Link's (`COPYING`).
