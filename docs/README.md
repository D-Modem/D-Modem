# D-Modem technical documentation

The project's overview, building and usage are in the top
[README](../README.md). These are the technical documents.

## The modem

- [DSP: the open data pumps](dsp/README.md): the interface to the modem
  core, the pumps (V.21 to V.34), the QAM receiver and the code structure
  - [V.8 and V.34](dsp/v34.md)
  - [V.90 digital modem](dsp/v90.md)
  - [V.92](dsp/v92.md): quick connect, modem-on-hold, PCM upstream
  - [V.91 digital to digital](dsp/v91.md)
  - [Fax](dsp/fax.md): class 1 modems and the T.30 fax machine
- [The data link layer](link.md): V.42 (LAPM), MNP 2 to 4, V.42bis, V.44,
  MNP 5
- [The modem core](core.md): AT commands, S-registers, the call's state
  machine

## Telephony

- [Asterisk: app_dmodem](asterisk.md)
- [FreeSWITCH: mod_dmodem](freeswitch.md)

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
  codes and S-registers; its LaTeX source is [at/](at/at_commands.tex),
  `docs/at/build.sh` builds it
- [Data over mobile telephony's speech paths](v_cvm.pdf): a proposed
  Recommendation, in the form of an ITU-T Recommendation, for a modem
  through the AMR, AMR-WB and EVS codecs of VoLTE and VoNR calls, built on
  V.8, V.21, V.22, V.34's probing, V.42, V.44 and T.38, with measurements
  of what the codecs do to modem signals (D-Modem's modems through them,
  and waveform probes); its LaTeX source is [cellular/](cellular/v_cvm.tex),
  `docs/cellular/build.sh` builds it and `cellular/measure/` repeats the
  measurements
- [legacy/ST56ATCommands.pdf](legacy/ST56ATCommands.pdf): the AT command
  set of Smart Link's ST56 modems, which D-Modem's own command set began
  as
