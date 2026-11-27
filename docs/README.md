# D-Modem documentation

The main [README](../README.md) gives the overview of the project. These pages
give more information:

- [Building](building.md): how to build D-Modem.
- [Using dmodemd](usage.md): how to use it.
- [What the modem does](features.md): all the standards that it implements.
- [How it works](how-it-works.md): how a softmodem operates on a SIP call.
- [Source layout](layout.md): what each folder has.
- [Known issues](known-issues.md): the problems that are not fixed yet.

This folder tells how each part operates.

Each page uses ASD-STE100 Simplified Technical English ([STYLE.md](STYLE.md)). It tells what the part does, how
to use it, and its current status. Each page has a link to a **technical
details** page next to it (with the name `<page>-details.md`). That page has the full
technical description: the names in the code, the clause numbers of the standards,
measurements, and test lists.

## A few words used throughout

- **DTE**: the computer or program that uses the modem, for example a
  terminal, `pppd` or a fax program. It communicates with the modem with AT
  commands.
- **Data pump**: the part of the modem that changes data into sound, and sound into
  data.
- **SIP**: the standard for internet telephone calls. `dmodemd` makes
  its calls over SIP, if it does not operate modem hardware.
- **G.711**: the plain digital audio format of the telephone network
  (in two types, µ-law and A-law). Fast modem calls must have it from end to
  end, without conversion.
- **V.something**: the ITU-T standards that define modems, for example V.34
  or V.92. [The list](specs/README.md) shows which standards D-Modem implements.

## The modem

- [The data pumps](dsp/README.md): the modem standards, from 300 bit/s
  to 56k and beyond
  - [V.8 and V.34](dsp/v34.md): the agreement on a standard, and the fastest
    analogue modem
  - [V.90: the digital modem](dsp/v90.md): D-Modem as the 56k server
  - [V.90 and V.92: the analogue modem](dsp/v90a.md): D-Modem as the
    56k modem of the customer
  - [The 56k client on a SIP line](dsp/sip-client.md): V.90, V.92,
    K56flex and x2 as the modem of the customer, without hardware
  - [V.92](dsp/v92.md): quick connect, modem-on-hold and faster uploads
  - [V.91](dsp/v91.md): about 62000 bit/s between two digital modems
  - [V.cvm](dsp/vcvm.md): data through mobile phone calls (the
    proposed standard: [The V.cvm proposal](vcvm-spec.md))
  - [V.wb](dsp/vwb.md): about 149000 bit/s on a bare pair of wires
  - [K56flex](dsp/k56flex.md): the older 56k standard (as the server and
    as the modem of the customer)
  - [V.8bis](dsp/v8bis.md): the agreement on the kind of call before the
    modems start. This is the first part of voice and data together (V.70).
  - [V.61](dsp/v61.md): voice and data together on one analogue
    carrier (ASVD)
  - [Point of sale](dsp/fastpos.md): fast connect and V.29 FastPOS, the
    short start-ups of card terminals that call their host
  - [Fax](dsp/fax.md): fax modem classes 1, 2 and 2.0, a built-in fax
    machine, and fax over the internet (T.38)
- [The data link layer](link.md): error correction and compression
- [Hayes V-series protocols](hayes.md): Hayes ADC compression, the
  Hayes XID sets, AFT framing and the link rules of Express 96
- [The x2 upgrade host](x2-upgrade.md): enable x2 in a USRobotics
  Courier that calls in, as the U.S. Robotics host did in 1997
- [EBCDIC terminals and hosts](ebcdic.md): AT commands and result codes
  in EBCDIC. The modem identifies the character set from the `AT`.
- [ZMODEM with zstd](zmodem-zstd.md): the specification of the zstd
  mode of ZMODEM in the D-Modem BBS, GUI and app
- [The MNP protocol suite](mnp/README.md): MNP 1 to 10 and 10EC, one class at a time
- [The modem core](core.md): AT commands, settings and how a call operates
- [ISDN](isdn.md): 64000 bit/s ISDN data calls
- [Voice and data together (V.70)](v70.md): DSVD, a telephone
  conversation and a data connection on one call (now only its data channel).
  Or the ASVD of V.61 ([V.61](dsp/v61.md)), which sends the voice as
  sound on the carrier of the modem.
- [Modem relay (V.150.1)](v150.md): data calls through a gateway that
  answers the far modem itself, and sends the data as bytes over the internet
- [Leased lines and dry pairs](leased.md): private circuits with no
  telephone exchange, and their line tests
- [The speaker of the modem](speaker.md): hear the call on a sound
  device of the computer (`--speaker`, `ATM`, `ATL`)
- ZyXEL compatibility: [encryption (S102, ATCK)](zyxel-des.md),
  [fax mode (class 6)](zyxel-fax.md) and [security](zyxel-security.md)

## Phone systems

- [The PBX modules and the login menu](modules.md): a summary of each module
- [Asterisk: app_dmodem](asterisk.md): a modem module for Asterisk
- [dmodem-ari](ari.md): the same, as a separate program together with Asterisk
- [FreePBX](freepbx.md): configure D-Modem from the web interface of FreePBX
- [FreeSWITCH: mod_dmodem](freeswitch.md): a modem module for
  FreeSWITCH

## Programs that use the modem

- [dmodemd as a library](embed.md): the modem in a thread of a program
  (libdmodem_embed), for the D-Modem desktop app

## Hardware and operating systems

- [Softmodem hardware](hardware.md): PC modem chips and USB modems on a
  real phone line
- [The Agere USB 2.0 soft modem](agere-usb.md)
- [The Apple USB Modem](apple-usb-modem.md)
- [The SmartUSB56 under Windows](smartusb56-windows.md): what we learned from the
  Windows driver of Smart Link
- [Windows](windows.md)
- [macOS](macos.md)
- [Haiku](haiku.md)

## Testing

- [Building and testing](testing.md)
- [Interop against other soft modems](interop.md): tests of D-Modem
  against the modems of other companies

## Review

- [Code review (October 2026)](review.md): the problems that a full review of the code
  found, and their fixes

## References

- [Acknowledgements](acknowledgements.md): where D-Modem comes from
- [dmodemd's command line](reference/dmodemd.md), the
  [environment variables](reference/environment.md) and the
  [CMake options](reference/cmake.md): `tools/site_reference.py` makes them
  from the source
- [ITU-T Recommendations](specs/README.md): the standards that D-Modem
  implements, with a script that downloads them
- [AT command reference](at_commands.pdf): all commands, result codes and
  S-registers, on A5 pages. The other files are for printing:
  - The [booklet](at_commands-booklet.pdf) has
    the same pages, two on each A4 sheet. Print it on the two sides (flip on the short
    edge), then fold and staple it, after a cover sheet.
  - The [cover](at_commands-cover.pdf) prints the covers and spine on one A3
    sheet.
  - The [A3 sections](at_commands-a3.pdf) print the inside
    pages, eight on each A3 sheet. Print on the two sides (flip on the long edge), fold
    two times and cut at the top.

  `docs/at/build.sh` builds all of these files from
  [the LaTeX source](at/at_commands.tex). The
  [AT manual](../.github/workflows/at-manual.yml) workflow builds them again and
  commits them each time the LaTeX changes on main.
- [Data over mobile telephony's speech paths](specs/v_cvm.pdf): the
  proposed V.cvm standard of D-Modem, written as an ITU-T Recommendation, with
  measurements of the effects of mobile codecs on modem signals. Its source is
  in [cellular/](cellular/v_cvm.tex). `docs/cellular/build.sh` builds it,
  and `cellular/measure/` does the measurements again.
  [The V.cvm proposal](vcvm-spec.md) tells what the standard says in simple words.
  [Its technical details](vcvm-spec-details.md) give a guide to each
  clause.
- [Unimodem diagnostics](specs/umud10.rtf): the specification of Microsoft,
  which `AT#UD` obeys
