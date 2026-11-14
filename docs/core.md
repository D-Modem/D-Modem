# The modem core

The modem core is the "brain" of D-Modem. It is between the program
that uses the modem (a terminal, `pppd`, a fax program) and the parts
that change data into sound. It reads the AT commands that you type, it keeps the
settings of the modem, and it moves each call through its stages:
dialling, connecting, online, and hanging up.

The code is in `core/`. For the full technical description, refer to
[the technical details](core-details.md).

## What it does

- **Understands AT commands.** It accepts the classic Hayes-style
  commands (`ATD` to dial, `ATA` to answer, `ATH` to hang up, and other commands).
  It also accepts the extended commands for fax, voice and V.92. The full list is in the
  [AT command reference](at_commands.pdf).
- **Accepts the setup strings of other modems.** Programs frequently send setup
  commands for USRobotics, Rockwell or Conexant modems. The core
  accepts these commands and replies `OK` instead of `ERROR`. Thus, those programs
  operate without changes.
- **Keeps the settings.** The core keeps these settings:
  - The S-registers (numbered settings, for example S0,
    the number of rings before the modem answers).
  - The result codes (`OK`, `CONNECT`, `NO CARRIER`).
  - The telephone line settings of each country.
- **Has built-in configurators.** These configurators operate when the modem is idle and on hook:
  - `AT!CF` opens a full-screen setup screen in the style of the Turbo Vision of Borland.
  - `AT!CT` opens a command line in the style of the IOS of Cisco
    (`show running-config`, `configure terminal`).
  - `AT!C` selects the configurator that your terminal can show.
  - `AT!M` gives the settings for programs, for example
    the desktop apps.
  - `AT!R` shows an init string of all the settings that
    you changed from the defaults. You can paste it into your next session.
- **Runs the call.** It moves a call from idle, to dialling or
  answering, to connecting, to online. When you type `+++`, you go back to
  command mode, and the modem does not hang up. `ATO` puts you online again.
- **Finds the difference between fax calls and data calls.** With `AT+FAA=1`, the modem answers.
  Then it finds if the caller is a fax machine or a modem, and it
  continues in the correct mode.
- **Records call statistics.** After a call, `ATI11` (or `ATI6`, or
  `AT&V1` in the layout of Rockwell) shows what occurred: the speed, the signal quality, the number of
  retrains of the line, and the cause of the end of the call. `AT#UD` gives the
  same data in the format of the Unimodem diagnostics specification of Microsoft
  (`ATZ` clears it). `AT+TMO` gives the data in the standard method, as the
  named diagnostic objects of ITU-T V.59 (`AT+TMO V34 All`, `AT+TMO CallProgress`).

## How it fits with the rest

The program that runs the modem (the `dmodemd` daemon, the `libdmodem`
library, or a test) gives the core the characters that you type and the
sound samples from the phone line. The core sends the sound to the
[data pumps](dsp/README.md), which do the signal processing. It sends
the data through the [data link layer](link.md), which does error
correction and compression.

## Where it came from

D-Modem started from the `slmodemd` of Smart Link. The core keeps the same
commands, responses and timing, thus it operates as that modem did.
None of the original code stays. We wrote it again in 2026, and a test
makes sure that the new core gives the same replies as the old core.

The new core also corrects many errors of the old core. For example:

- It gives its settings with `AT&V`.
- It gives correct numeric results with `ATV0`.
- It reads S-register numbers correctly (before, `ATS09?` failed).
- It uses the `;`, `W` and `@` of the dial string as the standard tells.

The [technical details](core-details.md)
give each change.

## Testing it

    ctest --test-dir build -R Core

This command runs scripted calls between two modems. It compares the text of each modem
with a recorded transcript. It also runs tests of the core alone.
