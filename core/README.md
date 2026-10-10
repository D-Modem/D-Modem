# The modem core

`core/` is the part of the modem between the DTE and the data pumps (C++,
`namespace dcore`, `dcore.h`):

- **The AT command interpreter** (`at.cpp`): `A B D E H I L M N O P Q S T V X
  Y Z`, `&A &C &D &E &F &H &K &P &R &S`, `\A \N`, `%C %E`, `*B`, `#CID`,
  `+MS +GCI +GMI +GMM +GMR +FCLASS +IFC`, class 1 fax (`+FTS +FRS +FTM +FRM
  +FTH +FRH`) and voice (`+V...` in `+FCLASS=8`). Command lines are edited
  (backspace), echoed, repeated with `A/`.
- **The S-registers**, the **result codes** (verbose or numeric, quiet, the
  connect messages of `&A` and `X`), and the **country parameters**
  (`homolog_data.cpp`, `+GCI`).
- **The call's state machine** (`modem.cpp`): idle, connecting, the data
  link coming up, online, online command mode (after `+++`), disconnecting;
  dialing and answering, rings (a SIP call's, or ring signals on a line
  with their cadence) and auto answer, caller ID, the fax and voice modes.
- **The timers**: the escape guard time and the ring cadence are deadlines
  on the program's clock (`poll()`, `next_timer_ms()`); the rest counts
  samples. No signals, no global timer list.

A program that runs a modem (slmodemd, libdmodem, a test) is its `Host`: it
starts and stops the line's audio, gets the characters for the DTE, and
tells the time. It hands the modem the DTE's characters (`dte_input()`), the
line's samples while the modem is started (`process()`), and calls
`poll()`.

The data pumps (`../dsp`) see the core through their C interface
(`interface.cpp`: `modem_dp.h`, `modem_param.h`, `dsplibs.h`): a
`struct modem` that leads back to the `Modem`. The data link layer
(`../link`: V.42 detection, LAPM, V.42bis, async) runs between the DTE's
characters and the pumps' bits. `log.cpp` has the debug output (stderr or
a hook, and the binary `slmodem.log`).

The behaviour is that of Smart Link's slmodemd core (`modem.c`,
`modem_at.c`, `modem_param.c`): the same commands, responses, states and
timing. slmodemd, libdmodem (`dmodem.h`) and modem_test run on it; the old
core is left only for the tests that need it (the original DSP library
calls into it). Differences:

- the version banner is the program's to print (`Modem::product`,
  `version`; slmodemd and modem_test print it at the start);
- `AT&A1` in fax mode prints `Modulation: NONE` (the old core dereferenced
  a null data pump there).

## Tests

    ctest --test-dir build -R core_

- `core_equiv_*`: `core_script` runs a script (`test/scripts/`) with two
  modems back to back, once on this core and once on the old one, and the
  transcripts (what each modem tells its DTE, with the time to 20 ms) must
  be the same: the AT commands, calls at V.32bis, V.34/V.90 (`&A7`), V.22bis
  and V.21 with data both ways, the escape and `ATO`, hangups from either
  side and while connecting, rings and auto answer, no answer, class 1 fax
  (V.21 frames, a TCF at 9600), the voice commands.
- `core_test`: this core by itself: its `Host` interface, a line that does
  not start or go off hook, the escape timer, the options, and calls at
  V.32bis, V.34 and V.22 with data both ways, byte for byte.
