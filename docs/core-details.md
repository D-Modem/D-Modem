# The modem core: technical details

This is the technical companion to [The modem core](core.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

`core/` is the part of the modem between the DTE and the data pumps.
(The DTE, data terminal equipment, is the program on the other side of
the serial port or pty.) It is C++, in `namespace dcore`, declared in
`dcore.h`.

## What the core holds

### The AT command interpreter

The interpreter is in `at.cpp`. [at_commands.pdf](at_commands.pdf) is
the reference for every command.

| Group | Commands |
|---|---|
| Basic | `A B D E H I L M N O P Q S T V X Y Z` |
| `&` | `&A &C &D &E &F &H &K &L &P &Q &R &S &T &V` |
| `\` and `%` | `\A \N \V`, `%B %C %E` |
| Others | `*B`, `#CID #UD` |
| Modulation | `+MS` (by number or by V.250's name); V.18 text telephones (`+MV18S +MV18R +MV18AM +MV18P`, `+MBAUDOT`) |
| Identity and country | `+GCI +GCAP +GMI +GMM +GMR` |
| Fax and voice | `+FCLASS`; class 1 fax (`+FTS +FRS +FTM +FRM +FTH +FRH +FMI +FMM +FMR`); class 2 and 2.0 fax (`+FCLASS=2` and `2.0`: `+FLI +FPI +FCC +FIS +FCS +FBO +FCR +FPS +FLP +FSP +FHS +FDT +FDR +FET +FKS`, and class 2's names, in `at_fax2()`); voice (`+V...` in `+FCLASS=8`, `+VCID`) |
| V.92 | modem-on-hold (`+PMH +PMHT +PMHR`) |

**Other modems' setup strings.** The interpreter also accepts the
commands that other modems' setup strings send (V.250's, Rockwell's and
Conexant's, USRobotics'):

- the basic commands of `taken_cmds`, which have no effect;
- the extended parameters of `kept_params`, which keep their values for
  `?` and `=?`. Most have no effect either. Some are D-Modem settings
  that `modem.cpp` reads: `+ES`, `+ESA`, `+ITF` and `+IFC` (V.80),
  `+DS`, `+DS44` and `+DSZ` (compression), `+B2`, `+CBST` and `+CRC`
  (ISDN clear channels), `+FAA` (adaptive answer) and `+CVM` (V.cvm).

**Command lines.** `;` may separate commands. Command lines are edited
(backspace), echoed, and repeated with `A/`.

### Settings and results

- **The S-registers.**
- **The result codes**: verbose or numeric, quiet, and the connect
  messages of `&A`, `X` and `\V`. With `V0` a CONNECT's number gives its
  rate, as Smart Link's did.
- **The country parameters** (`homolog_data.cpp`, `+GCI`).

### The call's statistics

`ATI11`, `ATI6` and `AT&V1` all print the same statistics
(`call_stats()`). `AT#UD` prints them as Unimodem diagnostics
(`unimodem_diag()`).

**What is kept.** The statistics (`CallStats`) are kept from one call's
start to the next call's start:

- the modulation and the rates at CONNECT and at the call's end;
- the line's power each way while the call was up (measured on the
  samples in `process()`);
- why the call ended (`Cause`). The first event that says so wins: the
  DTE's hangup, the link's release or error, the pump's failure before
  or after CONNECT, or `line_dropped()` (dmodemd's call when the SIP
  call is over);
- what the pumps fill in (`dp_stats_`, `MDMPRM_STATS`): retrains,
  renegotiations, SNR, round trip delay, echoes; each direction's symbol
  rate and carrier; V.34's coding each way (trellis code, non-linear
  encoder, precoding, shaping); the receiver's carrier and clock
  offsets; and on a PCM call the network's law and, towards the analogue
  modem, its digital loss and robbed bits;
- the link's counts.

Each call's end logs a line of them.

**`AT#UD`** follows Microsoft's Unimodem Diagnostics Command Reference
Specification 1.0 (`docs/specs/umud10.rtf`):

- each key is a line `DIAG <2A4D3263 key=value>`, the key in
  hexadecimal;
- the modulation, error control, compression and the reason the call
  ended use V.58's codes;
- `ATZ` clears the log. After `ATZ` has ended a call, only the keys 0, 1
  and `60=52` remain.

### The call's state machine

The state machine is in `modem.cpp`. A call goes through: idle,
connecting, the data link coming up, online, online command mode (after
`+++`), and disconnecting. The state machine also handles dialling and
answering, rings (a SIP call's, or ring signals on a line, with their
cadence) and auto answer, caller ID, and the fax and voice modes.

**Adaptive answer** (`+FAA=1`). The data answer runs, in data mode or in
fax mode, until the call turns out to be one of these:

- **A fax call**, told by any of:
  - the caller's CNG (`dsp.h`'s `CngDetect`), or a burst of it cut short
    as our answer tone starts;
  - the V.8 CM of a fax call, which the V.8 pump reports with
    `DPSTAT_FAX`;
  - T.38 from the far end;
  - a caller that stays silent through the whole automode chain. The
    chain's last stage, Bell 103, reports `DPSTAT_FAX` after 3 s.

  The modem then reports FAX and goes on in class 1. The fax engine's
  CED is cut short because the answer tone has already been sent
  (`FAX_answer_tone()`).
- **A data call**, told by its pump's CONNECT: DATA, then its CONNECT.

`Options::fax_detect` has the program take a fax caller itself
(libdmodem's fax receiver) through `fax_caller()`, while the modem
stays in data mode.

**Voice mode.**

- The voice engine reports calling tones: `<DLE>c` for CNG and `<DLE>e`
  for V.25's 1300 Hz tone (`VOICE_tones()`'s `cng` and `ct`).
- `+FCLASS=0` or `1` while off hook keeps the call (`voice_hold()`,
  `voice_held_`: the line runs on, silent) for an `ATA` or `ATD` on it
  (`start_call(true)`).
- With `+FAA=1`, a calling tone does that by itself in the voice command
  state (`voice_calling_tones()`).

**A dial string's `;`** holds the call in command state once it is
dialled (call progress's `DPSTAT_DIALED`).

- `ATD` then dials more on it, or starts the handshake. Call progress
  reads `dialing_` (the dial string plus what was added), while SIP and
  `ATDL` use the number as the first `ATD` gave it.
- `ATA` answers it (V.250's call-back).

### The timers

The escape guard time and the ring cadence are deadlines on the
program's clock (`poll()`, `next_timer_ms()`). All other timers count
samples. There are no signals and no global timer list.

## How it connects to the rest

**The host.** A program that runs a modem (dmodemd, libdmodem, a test)
is its `Host`. The host starts and stops the line's audio, gets the
characters for the DTE, and tells the time. It hands the modem the
DTE's characters (`dte_input()`) and the line's samples while the modem
is started (`process()`), and it calls `poll()`.

**The data pumps** (`../dsp`) see the core through their C interface
(`interface.cpp`: `modem_dp.h`, `modem_param.h`, `dsplibs.h`): a
`struct modem` that leads back to the `Modem`.

**The data link layer** (`../link`: V.42 detection, LAPM, MNP 2 to 4,
V.44, V.42bis, MNP 5, async) runs between the DTE's characters and the
pumps' bits. On calls with no error correction, two more layers sit
between the link and the pumps:

- V.13's simulated carrier (`v13.cpp`, S48);
- V.54's loop tests with V.52's self-test (`v54.cpp`, `AT&T`).

Both are described in [leased.md](leased-details.md).

**Logging.** `log.cpp` writes the debug output (to stderr or a hook) and
the binary `dmodem.log`.

## Where it came from

The behaviour started as that of Smart Link's slmodemd core (`modem.c`,
`modem_at.c`, `modem_param.c`): the same commands, responses, states and
timing. dmodemd, libdmodem (`dmodem.h`), modem_test and the interop
tests' modems run on it, and so does Smart Link's original DSP library
(`half_smartlink`, [interop.md](interop.md)).

The old core is no longer in the tree. `core_equiv` compares this one
with the transcripts recorded from it. None of slmodemd's code is left:
`at.cpp`, `modem.cpp`, `interface.cpp` and the country data were written
anew in 2026 from specifications of what they do, and give the same
transcripts.

### Where this core differs from the old one

This core goes beyond the old one where the old one fell short:

- **Version banner.** The banner is the program's to print
  (`Modem::product`, `version`); dmodemd and modem_test print it at the
  start.
- **`AT&A1` in fax mode** prints `Modulation: NONE`. The old core
  dereferenced a null data pump there.
- **Each command answers for itself.** `AT*B` (blacklisted numbers; none
  are kept) and `AT\A<0-3>` (the MNP block size) answer OK, and an
  `AT+M<x>` the core does not know is ERROR. The old core answered each
  of these with the result of the command before it on the line (ERROR
  at the line's start). The `+M` commands the core knows are `+MS`,
  `+MR` (kept), and V.18's `+MV18...` and `+MBAUDOT`.
- **After a fax or voice command's data.** Fax and voice data stops at
  the DTE's `<DLE><ETX>`; what the DTE sends after it is its next
  commands, and they run as the command ends. The old core ran them only
  when more characters came.
- **A call that ends online** drops the data still waiting to go out.
  The old core kept it, and its AT interpreter later took it for
  commands: a line of the data that began with AT ran.
- **The escape keeps the data** still waiting to go out, with the `+++`
  (`esc_held_`), for the line once `ATO` is back online; the AT
  interpreter starts with none of it. The old core dropped it, so
  whether the `+++` reached the remote depended on whether the line had
  taken it within the guard time (`EscapeKeepsData`).
- **`AT&V`** lists the active settings (the old core: ERROR). `AT\V1`
  puts the protocol after CONNECT's rate. With `ATV0`, a data call's
  CONNECT is its rate's number (the old core gave 1 for every one).
- **S-register numbers.** `ATS<n>?` and `ATS<n>=<v>` read `n` and `v` in
  decimal, a leading 0 too (`ATS09?` is S9), as V.250 has them. The old
  core read them as C's `strtoul()` does with base 0, octal after a
  leading 0, so `ATS08=1` and `ATS09?` were ERROR and `ATS0=010` set 8.
- **Statistics.** `ATI11` gives the last call's statistics (the old
  core: ERROR). So do `ATI6` (USRobotics'; in the old core, its second
  stored profile, empty) and `AT&V1` (Rockwell's; the old core: ERROR).
  `AT#UD` gives them as Unimodem's diagnostics. `&A`'s `Modulation:` is
  the one the call came up with (`V91` between two D-Modems); the old
  core named the pump that `AT+MS` chose (`V92`).
- **Dial string modifiers.** `;` (back to command state), `W` (wait for
  a dial tone) and `@` (wait for quiet after ringing) do what V.250 says.
  The old core skipped `;` and `@`, and paused 2 s for `W`. `S=<n>` (a
  stored number; there are none) is ERROR.
- **`+FAA=1`** answers adaptively: FAX or DATA, then the call goes on as
  a fax or a data call. The old core took only `+FAA=0`.

## Tests

    ctest --test-dir build -R Core

### core_equiv

`core_equiv` (`CoreEquiv`) runs each script in `test/scripts/` through
`core_script`: two modems back to back, driven as a DTE would drive
them. The transcript (what each modem tells its DTE, with the time in
20 ms steps) must equal the script's `<name>.expect`.

Most `.expect` files are the old core's transcripts, recorded from it
when it left the tree (the two cores gave the same transcripts then).
Where this core differs on purpose (the list above), the `.expect` is
this core's transcript instead.

| Script | What it covers |
|---|---|
| `call_v32bis`, `call_v22`, `call_v34` | calls with data both ways, the escape and `ATO`. `call_v22` also has V.21 and numeric CONNECTs; `call_v34` has V.34/V.90 (`&A7`) and `&A`'s modulation (V.91) |
| `misc` | a call nobody answers, a key that aborts dialling, `ATDL`, S7 running out, hangups from either side and while connecting |
| `rings` | a SIP call that rings and goes away, auto answer at the second ring, ring signals with their cadence |
| `fax` | class 1 fax: V.21 frames, a TCF at 9600 |
| `fax_dte` | frames and commands around `<DLE><ETX>` |
| `voice`, `voice_dial` | the voice commands; `+VLS=1`, whose OK the voice engine gives, and a tone after it; `ATD` in voice mode (`+VRA`, `+VRN`) |
| `at` | all the AT commands |
| `setup` | other modems' setup strings |
| `data_drop` | a call that ends with data waiting |
| `dial` | the dial string's `;`, `W` and `@`, the call-back, numeric `&A` lines and `\V1` |
| `stats` | `ATI11`, `AT&V1`, `ATI6` and `AT#UD` before, during and after calls: V.34 with echoes, V.32bis with MNP and without error correction, and one with no answer |
| `adaptive` | `+FAA=1`: fax and data callers into data and fax mode, FAX or DATA, `+FCLASS` after each call |

The scripts whose `.expect` is this core's own are `at`, `setup`,
`fax_dte`, `data_drop`, `call_v22`, `dial`, `call_v34`, `stats`,
`adaptive` and `voice`.

**Script commands** (see the header of `test/script.cpp`): `A <text>`,
`run`, `until`, `send`, `ring`, `signal`/`cancel`, `hangup`, `state`,
and two that shape the line:

- `tone A 350+440`, or with on and off times in ms, puts a dial tone or
  ringing on A's line;
- `echo A -12 0 -30 40` sends A's signal back to it, a block (20 ms)
  plus 0 ms and plus 40 ms later, at -12 and -30 dB.

In a script `\\` is a backslash and `\xNN` any other character (`\b` is
not a backspace).

`CORE_EXPECT_WRITE=1 core_equiv` writes the `.expect` files anew, to be
read in `git diff`; an empty `.expect` starts a new script off.
`CoreScript.FaxDteUntilsCome` checks that every `until` of `fax_dte`
comes.

### core_test

`core_test` (`Core`, `CoreCall`, `CoreFax2`, `CoreText`,
`CoreContactId`) tests this core by itself:

| Area | Tests |
|---|---|
| The host and options | `Options` (the country, `ATI7`, S-registers out of range), `Countries`, `FailingLine` (a line that cannot start or go off hook), `LineDelay`, `Log` |
| Data calls | `CoreCall.DataBothWaysAndHangup` at V.32bis, V.34 and V.22, byte for byte; `EscapeKeepsData`; `RetrainOnRequest` (`ATO1`, `ATO3`); `V34MaxRate`; `ModemOnHold` |
| V.80 | `V80`, `V80Retrain` |
| Dialling and caller ID | `PulseDialling`, `CallerIdBeforeRing`, `CallerIdAfterLineReversal`, `CallerIdOnTheCall`, `CallerIdFromLine` |
| Answering | `AdaptiveAnswer`, `VoiceFaxCall`, `VoiceDataCallingTone` |
| Voice | `VoiceLineGone`, `LocalPhone`, `VoiceHandset`, `VoiceEvents` |
| Class 2 fax | `CoreFax2.TwoPages`, `Fax2Abort`, `Fax2Poll` |
| Text telephones and alarms | `CoreText.LineEachWay` (V.18's modes), `CoreContactId` (Contact ID messages kissed off, a bad checksum NAKed), `CoreSms` (fixed-line SMS: submissions, a delivery, a call with nothing to send) |
