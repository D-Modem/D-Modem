# The V.cvm proposal: a standard for data over mobile phone calls

V.cvm is a proposal for a new ITU-T Recommendation. It specifies a modem
that sends data through the speech path of a mobile phone call. The
D-Modem Authors wrote it. ITU-T did not approve it, and "V.cvm" is only a
working name.

The proposal is in the form of an ITU-T Recommendation, so that it can go
to the ITU-T study group for the V series. Read it here:

- [specs/v_cvm.pdf](specs/v_cvm.pdf): the proposal ("Draft new
  Recommendation ITU-T V.cvm", October 2026).
- [cellular/v_cvm.tex](cellular/v_cvm.tex): its LaTeX source.

This page tells you what the proposal says, in plain words. For a guide
to each clause, annex and appendix, see
[the technical details](vcvm-spec-details.md). For D-Modem's
implementation of the proposal (`AT+MS=VCVM`), see
[V.cvm: data over mobile phone calls](dsp/vcvm.md).

## The problem

Mobile networks send speech through low bit rate speech codecs:

- AMR, in GSM and in narrowband VoLTE.
- AMR-WB (ITU-T G.722.2), in wideband VoLTE.
- EVS, in VoLTE and in 5G voice (VoNR).

These codecs make a model of a voice. They do not keep the waveform. Thus
the usual V-series modems do not work through them. Of V.21 to V.34, only
V.21 (300 bit/s) and V.22 (1200 bit/s) get through AMR. Mobile networks do
not supply a circuit-switched data bearer now. If a phone call is the only
path between two DCEs, the data must go through the codec.

## The solution

The proposal uses the V series where the codec lets it work. It replaces
only the parts that the codec breaks:

1. **Start-up.** The call starts as a V.8 session. The DCEs show their
   V.cvm capabilities in CM and JM. If one DCE does not have V.cvm, the
   call continues with V.22 or V.21.
2. **Probing.** The DCEs send known test signals. Each receiver measures
   the band and the noise of the codec. This is the same principle as
   V.34's line probing.
3. **Data.** The signal is a comb of tones. Each tone carries data as a
   phase difference (DPSK), one symbol for each 20 ms codec frame. The
   probing selects the tones and the number of bits on each tone.
4. **Parametric mode.** At low codec modes, no comb gets through. Then the
   data goes in the period of a pulse train. The codec must send this
   period as the lag of its long-term (pitch) predictor. This mode carries
   200 or 400 bit/s.
5. **Error correction.** A Reed-Solomon code (from ITU-T G.975) with an
   interleaver corrects the codec's errors and the lost frames. V.42 LAPM,
   V.42bis and V.44 operate above it with no change.
6. **Compression for short messages.** V.44 can start from a preset
   dictionary of common text. Between two DCEs that have it, zstd
   compression with the same dictionary replaces V.44.
7. **DTE interface.** The AT commands are those of V.250, with `+MS=V_CVM`
   and a new `+CVM` command. Fax goes through the data link as T.38
   packets.

## The speeds

The table gives the measured raw rate (before the error correction code)
through each codec mode.

| Codec mode | Mode of the proposal | Raw rate |
|---|---|---|
| AMR 12.2 kbit/s | narrowband comb | 1600 bit/s |
| AMR 7.95 and 7.4 kbit/s, AMR-WB 6.6 kbit/s | parametric | 200 to 400 bit/s |
| AMR-WB 23.85 kbit/s, EVS 24.4 kbit/s wideband | wideband comb | 2200 to 2800 bit/s |
| EVS 9.6 kbit/s | parametric | 400 bit/s |
| EVS 13.2 kbit/s narrowband | narrowband comb (N400) | 800 bit/s |
| EVS 48 kbit/s wideband | wideband comb, 8-DPSK | 4950 bit/s |
| EVS 128 kbit/s super wideband | super wideband comb, 8-DPSK | 10350 bit/s |
| AMR 4.75 kbit/s | none | no data |

No signal set can be faster than the bit rate of the codec. The
proposal gets about 10 to 15 per cent of that limit through AMR and EVS.

## Where the numbers come from

The D-Modem Authors measured each codec with D-Modem's modems and test
tools. They used these codec implementations:

- AMR and AMR-WB: OpenCORE and VisualOn, as Android uses them. A check
  against the 3GPP reference code gave bit-exact results.
- EVS: the 3GPP reference code (TS 26.442, version 16.4.0).

Appendix I of the proposal gives the AMR and AMR-WB measurements.
Appendix II gives the EVS measurements. The scripts that do the
measurements are in [cellular/measure/](cellular/measure/).

## How to build the PDF

You can make the PDF again from its LaTeX source:

1. Install TeX Live with `latexmk` (the same packages as `docs/at/build.sh`).
2. Run `docs/cellular/build.sh`.

The script writes `docs/specs/v_cvm.pdf`. It puts the temporary files in
`build/docs/cellular` (or in `$DMODEM_BUILD/docs/cellular`).

The tables of Appendix I are in `docs/cellular/measured.tex`. If you change
the measurement logs, run `docs/cellular/tables.py` to make this file
again.

## Status

- The proposal is a draft. ITU-T did not approve it.
- ITU-T must allocate a V.8 capability bit for V.cvm. Until then, the
  DCEs use the non-standard facilities field of V.8 (Annex A).
- D-Modem implements most of the proposal, but not all of it. The
  implementation page lists the parts that it does not do yet
  ([V.cvm technical details](dsp/vcvm-details.md#not-implemented)).
