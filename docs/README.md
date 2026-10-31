# D-Modem documentation

The project's overview, how to build it and how to use it are in the
main [README](../README.md). This folder explains how each part works.

Each page is written in plain English and covers what the part does, how
to use it, and its current status. Each one links to a **technical
details** page beside it (named `<page>-details.md`) with the full
technical description: the code's names, the standards' clause numbers,
measurements, and test lists.

## A few words used throughout

- **DTE**: the computer or program that uses the modem, such as a
  terminal, `pppd` or a fax program. It talks to the modem with AT
  commands.
- **Data pump**: the part of the modem that turns data into sound and
  back.
- **SIP**: the standard for internet telephone calls. `dmodemd` makes
  its calls over SIP unless it is driving modem hardware.
- **G.711**: the plain digital audio format of the telephone network
  (in two flavours, µ-law and A-law). Fast modem calls need it all the
  way, unconverted.
- **V.something**: the ITU-T standards that define modems, such as V.34
  or V.92. [The list](specs/README.md) shows which D-Modem implements.

## The modem

- [The data pumps](dsp/README.md): the modem standards, from 300 bit/s
  to 56k and beyond
  - [V.8 and V.34](dsp/v34.md): agreeing on a standard, and the fastest
    analogue modem
  - [V.90: the digital modem](dsp/v90.md): D-Modem as the 56k server
  - [V.90 and V.92: the analogue modem](dsp/v90a.md): D-Modem as the
    customer's 56k modem
  - [V.92](dsp/v92.md): quick connect, modem-on-hold and faster uploads
  - [V.91](dsp/v91.md): about 62000 bit/s between two digital modems
  - [V.cvm](dsp/vcvm.md): data through mobile phone calls
  - [V.wb](dsp/vwb.md): about 149000 bit/s on a bare pair of wires
  - [K56flex](dsp/k56flex.md): the older 56k standard (as the server)
  - [Fax](dsp/fax.md): fax modem classes 1, 2 and 2.0, a built-in fax
    machine, and fax over the internet (T.38)
- [The data link layer](link.md): error correction and compression
- [The modem core](core.md): AT commands, settings and how a call runs
- [ISDN](isdn.md): 64000 bit/s ISDN data calls
- [Modem relay (V.150.1)](v150.md): data calls through a gateway that
  answers the far modem itself, the data sent as bytes over the internet
- [Leased lines and dry pairs](leased.md): private circuits with no
  telephone exchange, and their line tests

## Phone systems

- [Asterisk: app_dmodem](asterisk.md): a modem module for Asterisk
- [dmodem-ari](ari.md): the same, as a separate program beside Asterisk
- [FreePBX](freepbx.md): set up D-Modem from FreePBX's web interface
- [FreeSWITCH: mod_dmodem](freeswitch.md): a modem module for
  FreeSWITCH

## Hardware and operating systems

- [Softmodem hardware](hardware.md): PC modem chips and USB modems on a
  real phone line
- [The Agere USB 2.0 soft modem](agere-usb.md)
- [The SmartUSB56 under Windows](smartusb56-windows.md): what Smart
  Link's Windows driver taught us
- [Windows](windows.md)
- [macOS](macos.md)
- [Haiku](haiku.md)

## Testing

- [Building and testing](testing.md)
- [Interop against other soft modems](interop.md): D-Modem tested
  against other companies' modems

## Review

- [Code review (October 2026)](review.md): what a full review of the code
  found and fixed

## References

- [ITU-T Recommendations](specs/README.md): the standards D-Modem
  implements, with a script that downloads them
- [AT command reference](at_commands.pdf): every command, result code and
  S-register, on A5 pages. The [booklet](at_commands-booklet.pdf) has
  the same two to an A4 sheet, to print on both sides (flip on the short
  edge), fold and staple, after a cover sheet. The
  [cover](at_commands-cover.pdf) prints the covers and spine on one A3
  sheet, and the [A3 sections](at_commands-a3.pdf) print the inside
  pages eight to an A3 sheet (both sides, flip on the long edge, fold
  twice and cut at the top). `docs/at/build.sh` builds them all from
  [the LaTeX source](at/at_commands.tex).
- [Data over mobile telephony's speech paths](v_cvm.pdf): D-Modem's
  proposed V.cvm standard, written as an ITU-T Recommendation, with
  measurements of what mobile codecs do to modem signals. Its source is
  in [cellular/](cellular/v_cvm.tex); `docs/cellular/build.sh` builds it
  and `cellular/measure/` repeats the measurements.
- [Unimodem diagnostics](specs/umud10.rtf): Microsoft's specification,
  which `AT#UD` follows
