# D-Modem technical documentation

The project's overview, building and usage are in the top
[README](../README.md). These are the technical documents.

## The modem

- [DSP: the open data pumps](dsp/README.md): the interface to the modem
  core, the pumps (V.21 to V.34), the QAM receiver and the code structure
  - [V.8 and V.34](dsp/v34.md)
  - [V.90 digital modem](dsp/v90.md)
  - [V.90 analogue modem](dsp/v90a.md): the plan, phases 1 to 4 and the data mode
  - [V.92](dsp/v92.md): quick connect, modem-on-hold, PCM upstream
  - [V.91 digital to digital](dsp/v91.md)
  - [Fax](dsp/fax.md): class 1 modems and the T.30 fax machine
- [The data link layer](link.md): V.42 (LAPM), MNP 2 to 4, V.42bis, V.44,
  MNP 5, V.80 synchronous access
- [The modem core](core.md): AT commands, S-registers, the call's state
  machine
- [ISDN: the clear 64 kbit/s channel](isdn.md): `AT+MS=CLEAR`, SIP in
  CLEARMODE (RFC 4040), ISDN data calls through Asterisk
- [Leased lines and dry pairs](leased.md): `AT&L1` and `AT&L2`, the 4-wire
  pumps (V.29, V.33, V.27ter), retrains, V.54's loop tests and V.52's
  self-test (`AT&T`), V.13's simulated carrier (S48)

## Telephony

- [Asterisk: app_dmodem](asterisk.md)
- [FreePBX: the D-Modem module](freepbx.md): the profiles as destinations,
  dmodem.conf and the dialplan, a check of the trunks for modem calls
- [FreeSWITCH: mod_dmodem](freeswitch.md)
- [Softmodem hardware](hardware.md): dmodemd on an AC'97 or HD-audio
  modem codec through ALSA (`-a`)
- [The SmartUSB56 under Windows](smartusb56-windows.md): what Smart Link's
  Windows drivers for the ST7554 (Creative's Modem Blaster V.92 USB) do
- [Windows](windows.md): dmodemd for Windows (MSVC, MinGW-w64), its DTE a COM
  port (com0com), a named pipe (a VM's COM port) or TCP, and its tests
  under Wine
- [Haiku](haiku.md): dmodemd for Haiku (BeOS's successor), built there or
  from Linux with a sysroot of Haiku's packages
- [macOS](macos.md): dmodemd for macOS 11 and later (Apple silicon and
  Intel, universal), its USB modems through libusb's IOKit backend

## Testing

- [Building and testing](testing.md): the test programs, netsim (the
  simulated G.711 network) and the interop calls
- [Interop against other soft modems](interop.md): Agere, spandsp,
  Motorola SM56, PCTel HSP, Conexant HSF, Broadcom (its Windows driver)

## Review

- [Code review (October 2026)](review.md): what was fixed, and the
  findings still open

## References

- [ITU-T Recommendations](specs/README.md): the ones implemented, with
  links and a script that downloads them
- [AT command reference](at_commands.pdf): the commands D-Modem's
  modem core takes (dmodemd, libdmodem, the PBX modules), its result
  codes and S-registers, on A5 pages (half an A4 sheet); the
  [booklet](at_commands-booklet.pdf) has them two to an A4 sheet, to
  print on both sides (flip on the short edge), fold and staple.  Its
  LaTeX source is [at/](at/at_commands.tex), `docs/at/build.sh` builds
  both
- [Data over mobile telephony's speech paths](v_cvm.pdf): a proposed
  Recommendation, in the form of an ITU-T Recommendation, for a modem
  through the AMR, AMR-WB and EVS codecs of VoLTE and VoNR calls, built on
  V.8, V.21, V.22, V.34's probing, V.42, V.44 and T.38, with a parametric
  mode on the codec's own pitch lag for the codec modes no waveform
  passes, and measurements of what the codecs do to modem signals
  (D-Modem's modems through them, waveform and pulse-train probes); its
  LaTeX source is [cellular/](cellular/v_cvm.tex),
  `docs/cellular/build.sh` builds it and `cellular/measure/` repeats the
  measurements
- [legacy/ST56ATCommands.pdf](legacy/ST56ATCommands.pdf): the AT command
  set of Smart Link's ST56 modems, which D-Modem's own command set began
  as
