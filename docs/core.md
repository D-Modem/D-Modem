# The modem core

`core/` is the part of the modem between the DTE and the data pumps (C++,
`namespace dcore`, `dcore.h`):

- **The AT command interpreter** (`at.cpp`): `A B D E H I L M N O P Q S T V X
  Y Z`, `&A &C &D &E &F &H &K &P &Q &R &S &V`, `\A \N \V`, `%B %C %E`, `*B`,
  `#CID`, `+MS` (by number or V.250's name) `+GCI +GCAP +GMI +GMM +GMR
  +FCLASS +FAA +VCID`, V.92 modem-on-hold (`+PMH +PMHT +PMHR`), class 1 fax
  (`+FTS +FRS +FTM +FRM +FTH +FRH +FMI +FMM +FMR`) and voice (`+V...` in
  `+FCLASS=8`). It takes as well, with no effect, the commands other
  modems' setup strings send (V.250's, Rockwell's and Conexant's,
  USRobotics'): the basic ones of `taken_cmds` and the extended parameters
  of `kept_params`, which keep their values for `?`. `;` may separate
  commands. Command lines are edited (backspace), echoed, repeated with
  `A/`. [at_commands.pdf](at_commands.pdf) is their reference.
- **The S-registers**, the **result codes** (verbose or numeric, quiet, the
  connect messages of `&A`, `X` and `\V`; with `V0` a CONNECT's number
  for its rate, Smart Link's), and the **country parameters**
  (`homolog_data.cpp`, `+GCI`).
- **The call's statistics** (`ATI11`, `call_stats()`): kept from one
  call's start to the next's (`CallStats`): its modulation and rates at
  CONNECT and at its end, the line's power each way while it was up
  (measured on the samples in `process()`), why it ended (`Cause`: the
  first that says so, from the DTE's hangup, the link's release or
  error, the pump's failure before or after CONNECT, or `line_dropped()`,
  dmodemd's when the SIP call is over), with what the pumps fill in
  (`dp_stats_`, `MDMPRM_STATS`: retrains, renegotiations, SNR, round trip
  delay, echoes) and the link's counts. Each call's end logs a line of
  them.
- **The call's state machine** (`modem.cpp`): idle, connecting, the data
  link coming up, online, online command mode (after `+++`), disconnecting;
  dialing and answering, rings (a SIP call's, or ring signals on a line
  with their cadence) and auto answer, caller ID, the fax and voice modes.
  Adaptive answer (`+FAA=1`): the data answer runs, in data mode or in
  fax mode, and the call is found out to be a fax call (the caller's
  CNG, `dsp.h`'s `CngDetect`, or a burst of it cut short as our answer
  tone starts; the V.8 CM of a fax call, which the V.8 pump tells with
  `DPSTAT_FAX`; T.38 from the far end; a caller silent through the
  whole automode chain, which its last stage, Bell 103's, tells with
  `DPSTAT_FAX` after 3 s) or a data call (its pump's CONNECT): FAX
  and class 1, the fax engine's CED cut short by the answer tone sent
  already (`FAX_answer_tone()`), or DATA and its CONNECT.
  `Options::fax_detect` has the program take a fax caller itself
  (libdmodem's fax receiver): `fax_caller()`, the modem staying in data
  mode.
  Voice mode: the voice engine reports calling tones (`<DLE>c` for CNG,
  `<DLE>e` for V.25's 1300 Hz, `VOICE_tones()`'s `cng` and `ct`), and
  `+FCLASS=0` or `1` off hook keeps the call (`voice_hold()`,
  `voice_held_`: the line runs on, silent) for `ATA` or `ATD` on it
  (`start_call(true)`); with `+FAA=1` a calling tone does that itself
  in the voice command state (`voice_calling_tones()`).
  A dial string's `;` holds the call in command state once it is dialed
  (call progress's `DPSTAT_DIALED`): `ATD` dials more on it (call progress
  reads `dialing_`, the dial string and what is added, while SIP and `ATDL`
  have the number as `ATD` first gave it) or starts the handshake, `ATA`
  answers it (V.250's call-back).
- **The timers**: the escape guard time and the ring cadence are deadlines
  on the program's clock (`poll()`, `next_timer_ms()`); the rest counts
  samples. No signals, no global timer list.

A program that runs a modem (dmodemd, libdmodem, a test) is its `Host`: it
starts and stops the line's audio, gets the characters for the DTE, and
tells the time. It hands the modem the DTE's characters (`dte_input()`), the
line's samples while the modem is started (`process()`), and calls
`poll()`.

The data pumps (`../dsp`) see the core through their C interface
(`interface.cpp`: `modem_dp.h`, `modem_param.h`, `dsplibs.h`): a
`struct modem` that leads back to the `Modem`. The data link layer
(`../link`: V.42 detection, LAPM, MNP 2 to 4, V.44, V.42bis, MNP 5,
async) runs between the DTE's characters and the pumps' bits. `log.cpp`
has the debug output (stderr or a hook, and the binary `dmodem.log`).

The behaviour started as that of Smart Link's slmodemd core (`modem.c`,
`modem_at.c`, `modem_param.c`): the same commands, responses, states and
timing. dmodemd, libdmodem (`dmodem.h`), modem_test and the interop
tests' modems run on it, Smart Link's original DSP library as well
(`half_smartlink`, `docs/interop.md`). The old core is no longer in the
tree: `core_equiv` compares this one with the transcripts recorded from
it. None of slmodemd's code is left: `at.cpp`, `modem.cpp`,
`interface.cpp` and the country data were written anew in 2026 from
specifications of what they do, and give the same transcripts. This
core has gone beyond the old one where it fell short:

- the version banner is the program's to print (`Modem::product`,
  `version`; dmodemd and modem_test print it at the start);
- `AT&A1` in fax mode prints `Modulation: NONE` (the old core dereferenced
  a null data pump there);
- `AT*B` (the blacklisted numbers: none are kept) and `AT\A<0-3>` (the
  MNP block size) answer OK, `AT+M<x>` other than `+MS` ERROR,
  each for itself: the old core answered with the result of the command
  before them on the line (ERROR at its start);
- what the DTE sends after a fax or voice command's data (they stop at
  its `<DLE><ETX>`) is its next commands, run as the command ends; the
  old core ran them only when more characters came;
- a call that ends online drops the data still waiting to go out; the old
  core kept it, and its AT interpreter later took it for commands (a line
  of the data that began with AT ran);
- `AT&V` lists the active settings (the old core: ERROR), `AT\V1` puts the
  protocol after CONNECT's rate, and with `ATV0` a data call's CONNECT is
  its rate's number (the old core: 1 for every one);
- `ATI11` gives the last call's statistics (the old core: ERROR), and
  `&A`'s `Modulation:` is the one the call came up with (`V91` between two
  D-Modems; the old core named the pump AT+MS chose, `V92`);
- the dial string's `;` (back to command state), `W` (wait for a dial
  tone) and `@` (wait for quiet after ringing) do what V.250 says (the
  old core skipped `;` and `@`, and paused 2 s for `W`), and `S=<n>` (a
  stored number: there are none) is ERROR.
- `+FAA=1` answers adaptively: FAX or DATA, then the call goes on as a
  fax or a data call (the old core took only `+FAA=0`).

## Tests

    ctest --test-dir build -R Core

- `core_equiv` (`CoreEquiv`): `core_script` runs a script (`test/scripts/`) with two
  modems back to back, and its transcript (what each modem tells its DTE,
  with the time to 20 ms) must be the script's `<name>.expect`, the old
  core's, recorded from it when it left the tree (the two cores gave the
  same transcripts then): calls at V.32bis, V.34/V.90 (`&A7`), V.22bis and V.21 with
  data both ways, the escape and `ATO`, hangups from either side and while
  connecting, rings and auto answer, no answer, class 1 fax (V.21 frames,
  a TCF at 9600), the voice commands. Where this core differs on purpose
  (the list above) the `.expect` is this core's transcript instead: `at`
  (all the AT commands), `setup` (other modems' setup strings), `fax_dte`
  (frames and commands around `<DLE><ETX>`), `data_drop` (a call that
  ends with data waiting), `call_v22` (numeric CONNECTs), `dial` (the dial
  string's `;`, `W` and `@`, the call-back, numeric `&A` lines and `\V1`;
  `tone A 350+440`, or with on and off ms, puts a dial tone or ringing on
  A's line), `call_v34` (`&A`'s modulation, V.91), `stats` (`ATI11`
  before, during and after calls at V.34 with echoes, V.32bis with MNP
  and without error correction, and one with no answer; `echo A -12 0
  -30 40` sends A's signal back to it, a block and 0 and 40 ms later,
  at -12 and -30 dB), `adaptive` (`+FAA=1`: fax and data callers
  into data and fax mode, FAX or DATA, `+FCLASS` after each call),
  `voice` (`+VLS=1`, whose OK the voice engine
  gives, and a tone after it).
  `CORE_EXPECT_WRITE=1 core_equiv` writes the `.expect` files anew, to be
  read in `git diff`; an empty one starts a new script off. In a script `\\` is a backslash and `\xNN` any
  other character (`\b` is not a backspace). `FaxDteUntilsCome`: every
  `until` of `fax_dte` comes.
- `core_test` (`Core`, `CoreCall`): this core by itself: its `Host` interface, a line that does
  not start or go off hook, the escape timer, the options, and calls at
  V.32bis, V.34 and V.22 with data both ways, byte for byte.
