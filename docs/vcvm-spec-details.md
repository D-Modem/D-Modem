# The V.cvm proposal: technical details

This is the technical companion to
[The V.cvm proposal](vcvm-spec.md), the plain-English overview. Start
there. This page is a guide to the proposal
([specs/v_cvm.pdf](specs/v_cvm.pdf), LaTeX source in
[cellular/](cellular/v_cvm.tex)), clause by clause. It gives the
important values of each clause and the source file that holds the
clause.

The proposal is the reference. If this page and the proposal are
different, use the proposal. For D-Modem's code, see
[V.cvm: technical details](dsp/vcvm-details.md).

## The source files

| File | Contents |
|---|---|
| `docs/cellular/v_cvm.tex` | The main document: summary, clauses 1 to 9 and 11 to 14, the bibliography |
| `docs/cellular/modulation.tex` | Clause 10 (modulation) |
| `docs/cellular/annexes.tex` | Annexes A to F (normative) |
| `docs/cellular/appendices.tex` | Appendices I, III and IV (informative) |
| `docs/cellular/evs.tex` | Appendix II (EVS) |
| `docs/cellular/measured.tex` | The tables of Appendix I, made by `tables.py` |
| `docs/cellular/tables.py` | Makes `measured.tex` from the logs in `measure/logs/` |
| `docs/cellular/measure/` | The measurement scripts (`probe*.py`, `matrix.sh`) and their logs |
| `docs/cellular/build.sh` | Builds `docs/specs/v_cvm.pdf` |

CI checks Annexes E and F against the code. `cmake/v44_preset_check.py`
compares the text of Annex E in `annexes.tex` with `link/v44_preset.cpp`.
It also compares the dictionary header of Annex F with `link/zstd_dict.cpp`.

## Words used on this page

- **Speech path**: the path from the signal interface of one DCE to the
  signal interface of the other DCE. It includes the terminals, the
  codecs and the networks.
- **Signal interface (reference point P)**: the point where the DCE
  gives linear PCM samples to the terminal and gets them back.
- **Codec frame**: 20 ms of samples that the codec encodes together.
- **Comb**: a set of tones at one spacing.
- **Loading**: the tones in use and the modulation order of each tone.
- **Lag alphabet**: the set of pulse periods of the parametric mode.
- **Raw rate** and **coded rate**: the rate before and after the
  Reed-Solomon code.

## Clauses 1 to 5: scope, references, terms

Clause 1 gives the scope. The DCE sends data through AMR, AMR-WB and EVS,
as VoLTE and VoNR carry them, and through older circuit-switched speech
bearers. The proposal does not specify the codecs, the network or the
terminal. Packet data bearers are out of scope.

Clause 2 lists the normative references. The main references are G.711,
G.722.2, G.975, T.30, T.38, V.8, V.8bis, V.21, V.22, V.34, V.42, V.42bis,
V.44, V.250, T.35, RFC 8878 (zstd) and 3GPP TS 26.090, 26.190, 26.445 and
26.114.

Clause 5 gives the conventions. Levels are in dBFS (full scale of 16-bit
PCM). The test signals are at −12 dBFS.

## Clause 6: general description

Clause 6.1 describes the speech path as a channel. An ACELP codec sends a
spectral envelope, a pitch lag and gain for each 5 ms subframe, and a
sparse excitation. The measured effects on a modem signal are:

- The signal-to-noise ratio is about 10 to 15 dB. A higher input level
  does not make it better.
- A signal of many tones gets through better than a single tone.
- DPSK with 4 states for each tone holds over a 20 ms symbol. 8 states
  and amplitude modulation do not hold through the ACELP codecs.
- The band is the band of the codec: 200 to 3400 Hz for AMR, 50 to
  6400 Hz for AMR-WB. For EVS, the band changes with the bit rate.
- A lost packet erases a frame. The jitter buffer can repeat or drop a
  frame.
- The network changes the codec mode during the call.
- The one-way delay is 100 to 300 ms.
- The codec always sends the lag of its long-term predictor. Only AMR
  4.75 kbit/s does not carry it.

Clause 6.2 (Table 1) lists the parts of other Recommendations that the
proposal uses with no change. Clause 6.3 (Figure 1) shows the four phases
of a call.

| Phase | Contents |
|---|---|
| 1 | V.8 session: ANSam, CI, CM, JM, CJ |
| 2 | Probing: INFO0, L1, L2, L3, INFO1 |
| 3 | Parameter exchange: MP |
| 4 | Data: Reed-Solomon code, LAPM |

## Clause 7: reference configuration

The signal interface (clause 7.2) has these rules:

- Samples are 16-bit linear PCM at 8000 Hz (mandatory) or 16000 Hz
  (optional).
- The transmit level must not be more than −9 dBFS rms in a 20 ms
  frame. The comb modes use −12 ± 2 dBFS. The parametric mode uses
  −15 ± 2 dBFS.
- The receiver must accept −30 to −3 dBFS.
- The terminal must not apply acoustic processing between point P and
  the codec, if possible.
- The DCE must accept a clock difference of ±200 ppm.

## Clause 8: start-up

The start-up is the V.8 procedure. V.21 carries CI, CM, JM and CJ through
all codec modes. The DCE shows V.cvm in CM and JM, with V.22 and V.21 as
the fallback and LAPM as the protocol (Annex A).

If CM and JM both have V.cvm, the DCEs start phase 2. If not, the call
continues with V.22 or V.21. The answering DCE takes the end of CM
(30 ms of line at 30 dB below CM) as CJ, because a low codec mode can
corrupt CJ.

Timing (clause 8.4):

- Phase 2 starts not more than 200 ms after V.8.
- The first data symbols go not more than 21 s after V.8. The start-up
  usually takes 15 to 21 s.
- If a DCE does not receive INFO0 in 6 s, it restarts V.8 one time, for
  V.22.

## Clause 9: probing and mode selection

Each direction has its own probing and loading, because the two
directions go through different codecs.

### Phase 2 (clause 9.2, Annex B)

Each DCE sends these parts, with 100 ms of silence between them:

1. INFO0: the capabilities of the DCE.
2. L1: each comb that the DCE supports, all tones loaded, DQPSK, 0.6 s
   for each comb.
3. L2: the N200 comb with the 22.5 ms symbol, 0.5 s.
4. L3 (only with the parametric mode): 40 symbols of P20 and 80 symbols
   of P10, 1.84 s.
5. INFO1: the loading that the DCE asks for, from its measurements.

The calling DCE sends INFO0, L1, L2 and L3 first. Then the answering DCE
sends all five parts. Then the calling DCE sends its INFO1.

### The message mode (clause 9.3)

INFO0, INFO1 and MP are HDLC frames with an FCS. Each message goes two
times. The message mode is:

- P20 (the parametric mode), if both DCEs have the parametric mode.
- V.21 at 300 bit/s, if not.

### The loading (clause 9.4)

The receiver measures each tone of L1 and L2 and selects:

| Item | Rule |
|---|---|
| Band | Tones not more than 20 dB below the strongest tone |
| Comb and symbol period | The combination with the most bits per second |
| DQPSK | Phase error σ ≤ 20° |
| 8-DPSK | σ ≤ 9° |
| Two amplitude rings | Amplitude change σA ≤ 0.8 dB |
| Four amplitude rings | σA ≤ 0.4 dB |
| Code rate 7/8 | Tone symbol error ratio below 10⁻³ |
| Code rate 3/4 | Below 10⁻² |
| Code rate 1/2 | All other values |

A comb must load 4 tones or more, and must carry more than the parametric
mode. If not, the DCE uses the parametric mode:

- P10, if no symbol of the P10 part of L3 was in error.
- P20, if not more than 1 of 40 symbols of the P20 part was in error.
- No data mode, if not. The DCEs then stay in the message mode and
  retrain.

### Phase 3 and retrain (clauses 9.5 and 9.6)

Each DCE sends MP until it receives the MP of its peer with the
acknowledgement bit. Then it sends one more MP with its own
acknowledgement bit and starts phase 4.

During phase 4, a DCE asks for a renegotiation when the error ratio
changes. A renegotiation must take not more than 500 ms. A DCE asks for
a retrain when it has no valid codeword for 2 s.

In the parametric mode, a DCE retrains 30 s after it starts and then
each 60 s. It keeps c̄, the mean autocorrelation of the decided lags over
2 s:

- Below 0.85, no retrain is necessary. No codec mode with c̄ below 0.85
  carries a comb.
- If c̄ increases through 0.85, the DCE retrains immediately (not in the
  first 30 s of phase 4).
- The threshold has hysteresis: 0.83 down and 0.87 up.

## Clause 10: modulation

Clause 10 is in `modulation.tex`.

### The modes (Table 2)

| Mode | Signal | Raw rate | Support |
|---|---|---|---|
| Robust (R) | V.21 FSK | 300 bit/s | Mandatory |
| Narrowband (N) | Comb, 300 to 3400 Hz | 800 to 1600 bit/s (DQPSK) | Mandatory |
| Wideband (W) | Comb, 300 to 6900 Hz | 1700 to 3300 bit/s (DQPSK) | Optional |
| Parametric (P) | Pulse train, period is the symbol | 200 (P20) or 400 (P10) bit/s | Recommended |

### The combs (Table 3)

| Code | Comb | Spacing (Hz) | Tones | Band (Hz) | Tu (ms) | Tg (ms) |
|---|---|---|---|---|---|---|
| 0 | N200 | 200 | 16 | 400–3400 | 15 (or 20) | 5 (or 2.5) |
| 1 | N300 | 300 | 11 | 300–3300 | 10 | 10 |
| 2 | N400 | 400 | 8 | 400–3200 | 15 | 5 |
| 3 | W200 | 200 | 33 | 400–6800 | 15 | 5 |
| 4 | W300 | 300 | 23 | 300–6900 | 10 | 10 |
| 5 | W400 | 400 | 17 | 400–6800 | 15 | 5 |
| 6 | S200 | 200 | 68 | 400–13800 | 15 | 5 |
| 7 | S300 | 300 | 46 | 300–13800 | 10 | 10 |
| 8 | S400 | 400 | 35 | 400–14000 | 15 | 5 |

The S combs are for EVS super wideband, with the signal interface at
32000 Hz.

### Symbols (clauses 10.3 to 10.6)

- A symbol is a cyclic prefix (Tg) and a useful part (Tu). The symbol is
  20 ms. N200 can also use 22.5 ms.
- The symbols are not aligned to the codec frames. The measurements show
  that the alignment has no effect.
- The receiver gets the symbol timing from the cyclic prefix. It must
  find the timing again in two symbols after a step of up to ±20 ms.
- Each tone carries a phase difference (Table 6, Gray code). No carrier
  recovery or equalizer is necessary.
- The first symbol of phase 4 is the phase reference. The DCE sends it
  three times.
- Raw rate: R = (sum of bits on the loaded tones) / (Tu + Tg).

### The parametric mode (clause 10.7)

The transmitter sends one pulse each L samples, where L is the lag of the
symbol. A one-pole filter (0.7) and a 33-tap band-pass pulse make the
signal look like voiced speech to the codec.

| Alphabet | Symbol | Lags (samples at 8000 Hz) | Bits | Raw rate |
|---|---|---|---|---|
| P20 | 20 ms (160 samples) | 40, 42, ... 70 | 4 | 200 bit/s |
| P10 | 10 ms (80 samples) | 20, 21, ... 35 | 4 | 400 bit/s |

- The index of the lag is Gray coded (Table 4).
- Each transmission starts with a preamble of eight symbols: index 0 and
  index 15, one after the other.
- The receiver calculates the normalized autocorrelation c(L) for each
  lag and selects the largest. If a smaller lag is not more than 0.02
  below, it selects the smaller lag.

Clause 10.7 also explains why the proposal does not put the comb and the
pulse train in one waveform (Appendix I.8).

### Other items of clause 10

- **Control sequences (10.8).** In a comb mode, 8 symbols of 180° on all
  tones start a renegotiation. In the parametric mode, 16 symbols of
  index 0 start it.
- **Scrambler (10.9).** The V.34 scramblers (GPC and GPA) operate on the
  information octets, before the Reed-Solomon code.
- **Quick start (10.10).** A DCE can keep the loading of the last call
  and offer it in INFO0. Then phase 2 is shorter.
- **Amplitude rings (10.11).** A tone can also carry 1 or 2 bits as a
  change of amplitude: two rings 6 dB apart, or four rings 3 dB apart.

## Clause 11: error control and framing

The order from the DTE to the line is:

1. V.42 LAPM, with zstd, V.44 or V.42bis.
2. The Reed-Solomon code over the octets of the HDLC frames.
3. The interleaver.
4. The mapping onto the tone symbols or the lag symbols.

The code is RS over GF(2⁸) from G.975 (x⁸ + x⁴ + x³ + x² + 1), shortened
to 32 octets (Annex C):

| Rate | n | k | Errors corrected | Erasures corrected |
|---|---|---|---|---|
| 7/8 | 32 | 28 | 2 | 4 |
| 3/4 | 32 | 24 | 4 | 8 |
| 1/2 | 32 | 16 | 8 | 16 |

The receiver must mark the octets of an erased codec frame as erasures.
The mapping (clause 11.4) puts the bits on the tones from the lowest tone
to the highest.

## Clause 12: link layer and DTE interface

- LAPM starts with XID directly. The V.42 detection phase is not used.
- The DCEs negotiate V.44 (or V.42bis). If both INFO0 messages show the
  preset (Annex E), V.44 starts from the preset dictionary. If both show
  zstd, zstd replaces V.44 (Annex F).
- T401 is the round-trip delay plus 1 s, or plus 3 s in the parametric
  mode.
- N401 must not be more than 64 octets below 2400 bit/s.

The AT commands are:

| Command | Function |
|---|---|
| `+MS=V_CVM,1` | Selects V.cvm, with automode fallback to V.22 and V.21 |
| `+CVM=<modes>[,<rate>[,<report>]]` | Sets the modes permitted: 1 robust, 2 narrowband, 4 wideband, 8 8-DPSK, 16 amplitude rings, 32 parametric |
| `+CVM?` | Shows the settings and, during a call, the rates, tones, code rate, renegotiations and erasures |
| `+CVMR: <raw>,<coded>` | Unsolicited result code on each renegotiation |
| `CONNECT <rate>/CVM` | The coded rate |
| S44, S45 | The tones loaded and the renegotiations of the last call |

D-Modem uses `AT+MS=VCVM` for the `+MS` value.

## Clauses 13 and 14: interworking and performance

Interworking (clause 13):

- With a PSTN modem through a gateway, the call falls back to V.22 or
  V.21. A DCE must not show modulations above V.22 in CM, if it knows
  that a codec is in the path.
- Two codecs in tandem are one channel for the probing.
- Fax goes through Annex D.

The minimum performance (clause 14), with no frame erasures:

| Codec mode | Minimum raw rate |
|---|---|
| AMR 12.2 | 1200 bit/s (comb), code rate 3/4 or better |
| AMR 7.95, AMR 7.4, AMR-WB 6.6 | 200 bit/s (parametric) |
| EVS 9.6 narrowband | 400 bit/s |
| AMR-WB 23.85 at 16000 Hz | 2400 bit/s |
| EVS 13.2 narrowband | 800 bit/s (N400) |
| EVS 24.4 narrowband | 2000 bit/s |
| EVS 48 wideband at 16000 Hz | 4000 bit/s |

The error ratio to LAPM must be below 10⁻⁶. A frame erasure ratio of
1 per cent must not decrease the coded rate by more than one code rate
step.

## The annexes (normative)

All annexes are in `annexes.tex`.

### Annex A: capabilities in V.8 and V.8 bis

Until ITU-T allocates a V.8 bit, the DCE sends a two-octet field in the
non-standard facilities category of CM and JM. The field starts with the
identifier octet 0x43 ('C') after the T.35 country and provider codes.

| Octet | Bits | Meaning |
|---|---|---|
| 1 | 8–6 | Version: 001 |
| 1 | 5 | Signal interface at 16000 Hz |
| 1 | 4 | Fax (Annex D) |
| 1 | 3 | Amplitude rings |
| 1 | 1 | Always 0 |
| 2 | 8, 7 | Robust and narrowband modes (always 1) |
| 2 | 6, 5 | Wideband 200 Hz, wideband 400 Hz |
| 2 | 4 | 8-DPSK |
| 2 | 3 | 22.5 ms symbol |
| 2 | 2 | Quick start |
| 2 | 1 | Parametric mode |

Octet 1 bit 1 is always 0. This prevents a K56flex client from reading
the field as a K56flex server's field. V.8 bis carries the same field.

### Annex B: probing signals and messages

Annex B gives the contents of L1, L2 and L3, the measurements, and the
octets of INFO0 (Table B.1), INFO1 and MP (Table B.2). A scrambler fed
with ones makes the known phase and lag sequences.

### Annex C: inner code and interleaver

The interleaver has a depth of D = 4 codewords (128 octets) in the comb
modes and D = 1 in the parametric mode. The block delay is 0.64 s at
1600 bit/s. The decoder corrects a codeword when 2v + e ≤ 2t (v errors,
e erasures).

### Annex D: fax

The DCE terminates the T.30 session of the local fax machine. It sends
the T.38 IFP packets in LAPM I frames, with a type octet (0x01) and a
two-octet length. If the coded rate is below the fax rate, the DCE
accepts only ECM.

### Annex E: preset dictionary for V.44

Annex E gives 113 lines of text (3505 octets): HTTP, HTML, JSON, XML,
email, SMTP, POP3, NMEA and English. Both DCEs prime V.44 with this
text. For six typical short messages (1264 octets), V.44 gives 958
octets and primed V.44 gives 485 octets.

### Annex F: Zstandard compression

The DCEs negotiate zstd in the XID private parameter group (GI 0xF0),
set identification `ZST`. The dictionary 0x56434D01 is 3660 octets: 155
octets of entropy tables, then the text of Annex E. The six messages of
Annex E give 474 octets with zstd.

## The appendices (informative)

### Appendix I: the codecs as a channel

Appendix I is in `appendices.tex`, with the tables from `measured.tex`.
It gives the method and these results:

- **I.2:** the V-series modulations through each codec. Only V.21 and
  V.22 (AMR 12.2) send data with no errors.
- **I.3 to I.5:** steady tones, one carrier and the combs.
- **I.6:** tandem codecs and frame erasures.
- **I.7:** the parametric mode (Tables I.8 and I.9). P20 has no symbol
  errors through all codec modes except AMR 4.75.
- **I.8:** the comb and the pulse train in one waveform. It works, but
  it gives less than the comb alone.
- **I.9:** a check of the OpenCORE and VisualOn codecs against the 3GPP
  reference code. The output samples are identical.

To do the measurements again, use the scripts in `docs/cellular/measure/`.
Then run `docs/cellular/tables.py` and `docs/cellular/build.sh`.

### Appendix II: EVS

Appendix II is in `evs.tex`. EVS selects an ACELP core or a transform
core for each frame. Thus its effect on a comb changes with the bit rate:

- At 7.2 and 9.6 kbit/s, no comb gets through. The parametric mode gives
  400 bit/s at 9.6 kbit/s.
- At 13.2 kbit/s narrowband, N400 gets through (800 bit/s).
- At 24.4 kbit/s, all narrowband combs get through with 8-DPSK.
- At 48 kbit/s and more, the comb gets through almost as a waveform. At
  128 kbit/s, S200 with 8-DPSK gives 10350 bit/s, and 15100 bit/s with
  amplitude rings.
- An erased frame costs two symbols.

### Appendix III: terminals and networks

Appendix III tells terminals and networks how to help the DCE:

- Bypass the acoustic processing.
- Select the highest codec modes, wideband first.
- Disable DTX and VAD.
- Use a fixed jitter buffer delay.
- Do not transcode between codecs.

### Appendix IV: implementation notes

Appendix IV lists the parts that a V-series softmodem must have for
V.cvm. It also names the D-Modem files: `dsp/dp_vcvm.cpp`,
`dsp/vcvm*.cpp` and `dsp/rs.cpp`.
