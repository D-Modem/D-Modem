# The modem core: technical details

This page gives the technical details for [The modem core](core.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

`core/` is the part of the modem between the DTE and the data pumps.
(The DTE, data terminal equipment, is the program on the other side of
the serial port or pty.) It is C++, in `namespace dcore`, declared in
`dcore.h`.

## What the core holds

### The AT command interpreter

The interpreter is in `at.cpp`. [at_commands.pdf](at_commands.pdf) is
the reference for all commands.

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
| Configurators | `!C !CF !CT` (idle and on hook), `!R` (the init string), `!M` (the settings model, for programs) |

**The setup strings of other modems.** The interpreter also accepts the
commands that the setup strings of other modems send (those of V.250,
Rockwell and Conexant, and USRobotics):

- The basic commands of `taken_cmds`, which have no effect.
- The extended parameters of `kept_params`, which keep their values for
  `?` and `=?`. Most of them also have no effect. Some are D-Modem
  settings that `modem.cpp` reads: `+ES`, `+ESA`, `+ITF` and `+IFC`
  (V.80), `+DS`, `+DS44` and `+DSZ` (compression), `+B2`, `+CBST` and
  `+CRC` (ISDN clear channels), `+FAA` (adaptive answer) and `+CVM`
  (V.cvm).

**Command lines.** `;` can separate commands. You can edit command lines
(backspace). The modem echoes them, and `A/` repeats them.

### Settings and results

- **The S-registers.**
- **The result codes**: verbose or numeric, quiet, and the connect
  messages of `&A`, `X` and `\V`. With `V0`, the number of a CONNECT
  gives its rate, as in the modem of Smart Link.
- **The country parameters** (`homolog_data.cpp`, `+GCI`).

### The configurators

`config.h` holds one settings model (`Config::model()`) that `AT!R`,
`AT!CF`, `AT!CT` and `AT!C` share (`config.cpp`, `config_tui.cpp`,
`config_cli.cpp`). Each `Setting` has:

- Its key on the command line, its group, a label and a line of help.
- Its AT command (`form`, `%s` the value).
- Its values: choices (each with its AT text, a command line name and a
  label), a number from `lo` to `hi`, or the list of an extended
  parameter.
- How to read it from the modem (`get`).

The model includes the settings of the basic commands, the S-registers
with no command of their own (timers, characters), `+GCI`, `+MS` (its
four values as four settings, `ms` 0 to 3), `+FCLASS`, `#CID`, `+PMH`,
`+PMHT` and all kept parameters (`kept_params`).

- **A change of a setting** runs its AT command (`Config::apply()`,
  through `at_command()`). Thus, the configurators accept exactly what
  the AT interpreter accepts and nothing more. (The configurator first
  compares a value with the model. Thus, a configurator never runs a
  command that is not the command of a setting.)
- **The defaults** (`Config::defaults()`) are the values that `ATZ` sets.
  For the time that is necessary to read them, the fields of the settings
  are set as `sregs_init()`, `kept_init()` and `reset()` set them. Then
  the old values are put back. S8 and S24 are those of the country. The
  default country is the USA.
- **`AT!R`** (`Config::init_lines()`) gives the commands of the settings
  whose values are different from their defaults. It has these
  properties:
  - The commands are in the order of the model (`+GCI` first, because it
    sets S8 and S24; `+MS` as one command with its four values).
  - An extended command is followed by `;` when another command comes
    after it on the line.
  - The lines have a maximum of 63 characters (`LINE_CHARS`).
  - It runs in all states.

- **`AT!M`** (`Modem::model_listing()`) gives the model, for the own
  settings page of a program (the desktop apps in D-Modem/gui):
  - A line `model`, its format (1) and the number of settings.
  - For each setting, a line `setting`, key, kind (`pick`, `number`,
    `text`), value, default, lowest, highest (of a number), form, group,
    label and help.
  - After each pick, its `choice` lines (value, name, label).
  - Tabs separate the fields. It runs in all states.

**How to run one.** `AT!C`, `AT!CF` and `AT!CT` give ERROR unless the
modem is idle and on hook (`conf_idle()`: no call, the line not started,
on hook, no voice, fax or loop test, no held dial). The modem does not
read the remaining part of the line. While a configurator runs (`conf_`),
`dte_input()` gives it the characters of the DTE one at a time
(`conf_feed()`), in place of the AT interpreter, and `poll()` gives it
its timers. When it stops, it gives OK. After that, the characters that
the DTE types are commands again. A result (a RING), `hangup()` (DTR off)
and the destructor stop it immediately (`conf_close()`), without OK. The
S0 answer of a leased line waits until it stops.

**Keys.** `Config::input()` changes the characters into keys:

- The escape sequences of ANSI and VT100 (arrows, Home, End, PgUp, PgDn,
  Ins, Del, F1 to F10, Shift-Tab).
- ESC and a character as Alt and that character.
- A single ESC after 120 ms.
- CR LF as one Enter.
- The cursor position report (`ESC [r;cR`).

**`AT!C`** (and `AT!CF`) send `ESC [6n`. If no answer comes in 1.5 s,
`AT!C` starts the command line, and `AT!CF` starts the full screen at 80
by 24 in UTF-8. If an answer comes, the cursor goes to row and column 999
and the configurator asks again (this gives the size of the terminal, 80
by 24 to 160 by 66). Then it sends a CR and an é in UTF-8 and asks again.
If the result is column 2, the terminal accepts UTF-8. If not, the screen
is drawn in CP437.

**`AT!CF`** draws each frame into cells (a code point and a CGA colour).
It sends the cells that changed after the last frame, with the cursor
moves and SGR colours that they need. It never sends the last cell,
because this would scroll some terminals. It keeps the changes until F2
(or the question when you leave) runs them. It runs the changes of `+MS`
as one `+MS`. If the modem refuses a value, a red message box shows it.

**`AT!CT`** parses each line against the words that its mode accepts
(exec, configuration). You can cut a word short where only one word
starts with those characters. It gives the errors of IOS
(`% Invalid input detected at '^' marker.`, `% Ambiguous
command`, `% Incomplete command.`), and it has `?` and Tab. A setting
changes immediately.

### The call's statistics

`ATI11` and `ATI6` give the same statistics (`call_stats()`). `AT&V1`
gives them as the modems of Rockwell do (`rockwell_stats()`):

- Its labels are padded with dots to 28 columns.
- PCM rates are rounded as Rockwell rounds them (53333).
- The termination is given as the reasons of Rockwell (`LOCAL REQUEST`,
  `GSTN CLEARDOWN`, `CARRIER LOSS`, `NO CONNECT`, ...).
- The line quality is NMSE x 128.
- The pump states and EQM sum of Rockwell are `N/A`.

`AT#UD` gives them as Unimodem diagnostics (`unimodem_diag()`).

**What is kept.** The modem keeps the statistics (`CallStats`) from the
start of one call to the start of the next call:

- The modulation and the rates at CONNECT and at the end of the call.
- The power of the line in each direction while the call was up
  (measured on the samples in `process()`).
- Why the call ended (`Cause`). The first event that tells this is
  used: the hangup of the DTE, the release or error of the link, the
  failure of the pump before or after CONNECT, or `line_dropped()` (the
  call of dmodemd when the SIP call ends).
- The items that the pumps fill in (`dp_stats_`, `MDMPRM_STATS`):
  retrains, renegotiations, SNR, round trip delay, echoes. The symbol
  rate and carrier of each direction. The coding of V.34 in each
  direction (trellis code, non-linear encoder, precoding, shaping). The
  carrier and clock offsets of the receiver. On a PCM call, the law of
  the network and, in the direction of the analogue modem, its digital
  loss and robbed bits.
- The counts of the link.

At the end of each call, the modem logs a line of them.

**`AT#UD`** follows the Unimodem Diagnostics Command Reference
Specification 1.0 of Microsoft (`docs/specs/umud10.rtf`):

- Each key is a line `DIAG <2A4D3263 key=value>`, with the key in
  hexadecimal.
- The modulation, error control, compression and the reason for the end
  of the call use the codes of V.58.
- `ATZ` clears the log. After `ATZ` has ended a call, only the keys 0, 1
  and `60=52` remain.

**`AT+TMO`** (V.250 6.9) reads the same data as the managed objects of
V.59 (`v59.cpp`). Each object has a name and a four-digit tag. The high
two digits give its high-level object (`V34Diag` 0C, `CnxDiag` 2C,
`CallProgress` 2E, ...). The values 40 to 5F in the low two digits are
the "common" objects that all modulations can have (`txDataHistory` 4C:
0C4C for V.34).

- `+TMO 0=?` gives a list of the tags (`+TMO 0N=?` the names).
  `+TMO 4=?` gives `(0,1)` (names and tags).
- `+TMO 0C` or `+TMO V34 All` gives all of the objects of V.34, each as a
  line `tag{value}` or `name{value}`. `+TMO txDataHistory` alone gives
  that object of the modulation of the last call. `+TMO` alone repeats
  the last command.
- An object that does not apply to the last call is `NULL`. The values
  are those of V.59 (the codes of its tables, levels with their units,
  `TRUE` and `FALSE`, octets in hexadecimal separated by `;`).
- The modem keeps the rates in each direction from CONNECT when they
  change (the last 16), for `txDataHistory` and `rxDataHistory`.
- The objects that are made of exchanged sequences that the core does not
  record (the CI of V.8, the INFO and MP of V.34, the CP of V.90, the QC
  of V.92, the sequences of V.8 bis) are not available.

### The call's state machine

The state machine is in `modem.cpp`. A call goes through these states:
idle, connecting, the data link starts, online, online command mode
(after `+++`), and disconnecting. The state machine also controls
dialling and answering, rings (of a SIP call, or ring signals on a line,
with their cadence) and auto answer, caller ID, and the fax and voice
modes.

**Adaptive answer** (`+FAA=1`). The data answer operates, in data mode or
in fax mode, until the call is identified as one of these:

- **A fax call**, identified by one of these items:
  - The CNG of the caller (the `CngDetect` of `dsp.h`), or a burst of it
    that stops when our answer tone starts.
  - The V.8 CM of a fax call, which the V.8 pump reports with
    `DPSTAT_FAX`.
  - T.38 from the far end.
  - A caller that stays silent through the full automode chain. The last
    stage of the chain, Bell 103, reports `DPSTAT_FAX` after 3 s.

  Then the modem gives FAX and continues in class 1. The CED of the fax
  engine is cut short, because the answer tone was already sent
  (`FAX_answer_tone()`).
- **A data call**, identified by the CONNECT of its pump: DATA, then its
  CONNECT.

With `Options::fax_detect`, the program receives a fax caller itself (the
fax receiver of libdmodem) through `fax_caller()`, while the modem stays
in data mode.

**Voice mode.**

- The voice engine reports calling tones: `<DLE>c` for CNG and `<DLE>e`
  for the 1300 Hz tone of V.25 (the `cng` and `ct` of `VOICE_tones()`).
- `+FCLASS=0` or `1` while off hook keeps the call (`voice_hold()`,
  `voice_held_`: the line continues, silent) for an `ATA` or `ATD` on it
  (`start_call(true)`).
- With `+FAA=1`, a calling tone does that automatically in the voice
  command state (`voice_calling_tones()`).

**The `;` of a dial string** holds the call in command state after the
modem dials it (the `DPSTAT_DIALED` of call progress).

- Then `ATD` dials more digits on it, or starts the handshake. Call
  progress reads `dialing_` (the dial string plus the added digits).
  SIP and `ATDL` use the number that the first `ATD` gave.
- `ATA` answers it (the call-back of V.250).

### The timers

The escape guard time and the ring cadence are deadlines on the clock of
the program (`poll()`, `next_timer_ms()`). All other timers count
samples. There are no signals and no global timer list.

## How it connects to the rest

**The host.** A program that operates a modem (dmodemd, libdmodem, a
test) is its `Host`. The host starts and stops the audio of the line,
gets the characters for the DTE, and gives the time. It gives the modem
the characters of the DTE (`dte_input()`) and the samples of the line
while the modem is started (`process()`), and it calls `poll()`.

**The data pumps** (`../dsp`) see the core through their C interface
(`interface.cpp`: `modem_dp.h`, `modem_param.h`, `dsplibs.h`): a
`struct modem` that leads back to the `Modem`.

**The data link layer** (`../link`: V.42 detection, LAPM, MNP 2 to 4,
V.44, V.42bis, MNP 5, async) operates between the characters of the DTE
and the bits of the pumps. On calls with no error correction, two more
layers are between the link and the pumps:

- The simulated carrier of V.13 (`v13.cpp`, S48).
- The loop tests of V.54 with the self-test of V.52 (`v54.cpp`, `AT&T`;
  and the `+T` commands of V.250 on the test objects of V.58, `v58.cpp`).

[leased.md](leased-details.md) gives a description of the two.

**Logging.** `log.cpp` writes the debug output (to stderr or a hook) and
the binary `dmodem.log`.

## Where it came from

The behaviour started as that of the slmodemd core of Smart Link
(`modem.c`, `modem_at.c`, `modem_param.c`): the same commands, responses,
states and timing. dmodemd, libdmodem (`dmodem.h`), modem_test and the
modems of the interop tests use it. The original DSP library of Smart
Link also uses it (`half_smartlink`, [interop.md](interop.md)).

The old core is not in the tree now. `core_equiv` compares this core with
the transcripts that we recorded from the old core. No code of slmodemd
remains. We wrote `at.cpp`, `modem.cpp`, `interface.cpp` and the country
data again in 2026 from specifications of their functions. They give the
same transcripts.

### Where this core differs from the old one

This core does more than the old core where the old core was not
sufficient:

- **Version banner.** The program prints the banner (`Modem::product`,
  `version`). dmodemd and modem_test print it at the start.
- **`AT&A1` in fax mode** prints `Modulation: NONE`. The old core
  dereferenced a null data pump there.
- **Each command answers for itself.** `AT*B` (blacklisted numbers; the
  modem keeps none) and `AT\A<0-3>` (the MNP block size) answer OK. An
  `AT+M<x>` that the core does not know gives ERROR. The old core
  answered each of these with the result of the command before it on the
  line (ERROR at the start of the line). The `+M` commands that the core
  knows are `+MS`, `+MR` (kept), and the `+MV18...` and `+MBAUDOT` of
  V.18.
- **After the data of a fax or voice command.** Fax and voice data stops
  at the `<DLE><ETX>` of the DTE. The characters that the DTE sends after
  it are its next commands, and they run when the command ends. The old
  core ran them only when more characters came.
- **A call that ends online** discards the data that still waits to go
  out. The old core kept it, and its AT interpreter later accepted it as
  commands: a line of the data that started with AT ran.
- **The escape keeps the data** that still waits to go out, with the
  `+++` (`esc_held_`), for the line when `ATO` is back online. The AT
  interpreter starts with none of it. The old core discarded it. Thus,
  the `+++` got to the remote modem only if the line took it in the
  guard time (`EscapeKeepsData`).
- **`AT&V`** gives a list of the active settings (the old core: ERROR).
  `AT\V1` puts the protocol after the rate of CONNECT. With `ATV0`, the
  CONNECT of a data call is the number of its rate (the old core gave 1
  for all calls).
- **S-register numbers.** `ATS<n>?` and `ATS<n>=<v>` read `n` and `v` in
  decimal, also with a leading 0 (`ATS09?` is S9), as in V.250. The old
  core read them as the `strtoul()` of C does with base 0, octal after a
  leading 0. Thus, `ATS08=1` and `ATS09?` gave ERROR and `ATS0=010` set 8.
- **Statistics.** `ATI11` gives the statistics of the last call (the old
  core: ERROR). `ATI6` (of USRobotics; in the old core, its second stored
  profile, empty) and `AT&V1` (in the form of Rockwell; the old core:
  ERROR) do the same. `AT#UD` gives them as the diagnostics of Unimodem.
  The `Modulation:` of `&A` is the modulation that the call used (`V91`
  between two D-Modems). The old core gave the pump that `AT+MS` selected
  (`V92`).
- **Dial string modifiers.** `;` (back to command state), `W` (wait for a
  dial tone) and `@` (wait for quiet after the ring signal) do what V.250
  tells. The old core skipped `;` and `@`, and paused 2 s for `W`.
  `S=<n>` (a stored number; there are none) gives ERROR.
- **`+FAA=1`** answers adaptively: FAX or DATA, then the call continues
  as a fax or a data call. The old core accepted only `+FAA=0`.
- **The detection phase of V.42.** When the core calls on a modulation
  older than V.8 (V.32bis, V.22bis, ...), it sends its ODPs and waits for
  the ADP of the answerer before LAPM. Thus, a Courier that answers uses
  V.42. The old core went to LAPM immediately. Those calls now connect
  about 120 ms later. After V.8, on a leased line and on a clear channel,
  it still goes immediately to LAPM.

## Tests

    ctest --test-dir build -R Core

### core_equiv

`core_equiv` (`CoreEquiv`) runs each script in `test/scripts/` through
`core_script`. This is two modems back to back, which the script operates
as a DTE does. The transcript (what each modem tells its DTE, with the
time in 20 ms steps) must be equal to the `<name>.expect` of the script.

Most `.expect` files are the transcripts of the old core. We recorded them
from it when it was removed from the tree (at that time, the two cores
gave the same transcripts). Where this core is intentionally different
(the list above), the `.expect` is the transcript of this core.

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

The scripts whose `.expect` is the transcript of this core are `at`,
`setup`, `fax_dte`, `data_drop`, `call_v22`, `call_v32bis`, `dial`,
`call_v34`, `stats`, `rings`, `adaptive` and `voice`.

**Script commands** (refer to the header of `test/script.cpp`):
`A <text>`, `run`, `until`, `send`, `ring`, `signal`/`cancel`, `hangup`,
`state`, and two commands that change the line:

- `tone A 350+440`, or with on and off times in ms, puts a dial tone or a
  ring signal on the line of A.
- `echo A -12 0 -30 40` sends the signal of A back to it, a block (20 ms)
  plus 0 ms and plus 40 ms later, at -12 and -30 dB.

In a script, `\\` is a backslash and `\xNN` is all other characters (`\b`
is not a backspace).

`CORE_EXPECT_WRITE=1 core_equiv` writes the `.expect` files again, so
that you can read them in `git diff`. An empty `.expect` starts a new
script. `CoreScript.FaxDteUntilsCome` does a check that each `until` of
`fax_dte` comes.

### core_test

`core_test` (`Core`, `CoreCall`, `CoreFax2`, `CoreText`,
`CoreContactId`) tests this core alone:

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
