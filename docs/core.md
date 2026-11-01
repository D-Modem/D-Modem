# The modem core

The modem core is the "brain" of D-Modem. It sits between the program
that uses the modem (a terminal, `pppd`, a fax program) and the parts
that turn data into sound. It reads the AT commands you type, keeps the
modem's settings, and walks each call through its stages: dialling,
connecting, online, and hanging up.

The code is in `core/`. For the full technical description, see
[the technical details](core-details.md).

## What it does

- **Understands AT commands.** It takes the classic Hayes-style
  commands (`ATD` to dial, `ATA` to answer, `ATH` to hang up, and so on)
  plus the extended ones for fax, voice and V.92. The full list is in the
  [AT command reference](at_commands.pdf).
- **Accepts other modems' setup strings.** Programs often send setup
  commands meant for USRobotics, Rockwell or Conexant modems. The core
  accepts these and replies `OK` instead of `ERROR`, so those programs
  work unchanged.
- **Keeps the settings.** The S-registers (numbered settings such as S0,
  the number of rings before answering), the result codes (`OK`,
  `CONNECT`, `NO CARRIER`), and each country's telephone line settings.
- **Runs the call.** It moves a call from idle, to dialling or
  answering, to connecting, to online. Typing `+++` drops you back to
  command mode without hanging up, and `ATO` returns you online.
- **Tells fax calls from data calls.** With `AT+FAA=1` the modem answers
  and then works out whether the caller is a fax machine or a modem, and
  carries on in the right mode.
- **Records call statistics.** After a call, `ATI11` (or `ATI6`, or
  `AT&V1`) shows what happened: the speed, the signal quality, how many
  times the line had to retrain, and why the call ended. `AT#UD` gives the
  same in the format of Microsoft's Unimodem diagnostics specification
  (`ATZ` clears it), and `AT+TMO` the standard way, as ITU-T V.59's
  named diagnostic objects (`AT+TMO V34 All`, `AT+TMO CallProgress`).

## How it fits with the rest

The program that runs the modem (the `dmodemd` daemon, the `libdmodem`
library, or a test) feeds the core the characters you type and the
sound samples from the phone line. The core passes the sound to the
[data pumps](dsp/README.md), which do the signal processing, and passes
the data through the [data link layer](link.md), which handles error
correction and compression.

## Where it came from

D-Modem began from Smart Link's `slmodemd`. The core keeps the same
commands, responses and timing, so it behaves the way that modem did.
None of the original code remains: it was rewritten in 2026, and a test
checks that the new core still gives the same replies as the old one.

The new core also fixes many things the old one got wrong. For example,
it now lists its settings with `AT&V`, gives proper numeric results with
`ATV0`, reads S-register numbers correctly (`ATS09?` used to fail), and
handles the dial string's `;`, `W` and `@` the way the standard says.
The [technical details](core-details.md)
list every change.

## Testing it

    ctest --test-dir build -R Core

This runs scripted calls between two modems and compares what each one
says against a recorded transcript, plus tests of the core on its own.
