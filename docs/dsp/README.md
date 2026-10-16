# Open data pumps for D-Modem

`dsp/` is a reimplementation (C++) of the interface that the
binary-only `dsplibs.o` (Smart Link's DSP library) gives to Smart Link's
slmodemd modem core. It exports the same entry points, so the modem core builds
against it unchanged. dmodemd and libdmodem use only this code; the
original library, `dsp/test/smartlink/dsplibs.o`, is kept as a test peer for
the interop tests.

The interface between the data pumps and a modem core is in `dsp/`, with C linkage, as the original library has it:
`modem_dp.h` (pumps register with `modem_dp_register` and exchange bits
with `modem_get_bits`/`modem_put_bits`), `modem_param.h` (the core's
parameters and S-registers by number), `dsplibs.h` (the library's other
entry points, and what the core gives it: tty I/O, debug output, memory)
and `modem_defs.h` (types and constants). The pumps hold a
`struct modem *` only as a handle and include none of the core's own
headers; `dsp/` compiles with nothing else on the include path. The
interface has one addition the original library does not use:
`MDMPRM_MODE` (data, fax or voice).

| Entry point | What it does here |
|---|---|
| `prop_dp_init` / `prop_dp_exit` | Register / deregister the data pumps below |
| `dcr_create/process/reset/delete` | DC removal. Same algorithm and constants as the original (skip 0.6 s, average 1 s, then 0.9/0.1 smoothing every 2 s) |
| `dp_runtime_create/delete` | State shared by the pumps of one call (for example, which answer tone call progress heard) |
| `RD_*` | Ring detector for snooping codecs (low frequency, high amplitude bursts) |
| `CID_*` | Caller ID: Bell 202 FSK, SDMF and MDMF, formatted (`AT+VCID=1`) or raw hex |
| `VOICE_*` | IS-101 style voice: 8 bit linear at 8000 Hz with `<DLE>` shielding, `+VTS` tones and DTMF |
| `FAX_*`, `FAX_class1_command` | T.31 class 1 fax (`AT+FCLASS=1`): V.21 HDLC, V.27ter, V.29 and V.17 (see [Fax](fax.md#fax-class-1)) |

## Data pumps

| DP id (`AT+MS=`) | Modulation | Notes |
|---|---|---|
| `DP_CALLPROG` | Call progress | Dial tone (X2/X4), DTMF dialing, the dial string's `W` (a dial tone), `@` (quiet after ringing) and `;` (`DPSTAT_DIALED`, the call held for more digits), V.25 calling tone, busy cadence, answer tone (2100 Hz / 2225 Hz / V.22 unscrambled ones), no-answer timeout |
| 21 | ITU V.21, 300 bit/s FSK | |
| 23 | ITU V.23, 1200/75 bit/s FSK | The answering side sends the 1200 bit/s channel |
| 103 | Bell 103, 300 bit/s FSK | |
| 22 | ITU V.22, 1200 bit/s DPSK | |
| 212 | Bell 212A, 1200 bit/s DPSK | 2225 Hz answer tone, scrambled ones handshake |
| 122 | ITU V.22bis, 2400 bit/s 16-QAM | Full S1 handshake, falls back to V.22 |
| 32 | ITU V.32, 4800/9600 bit/s | Uncoded 4800, trellis coded 9600, echo canceller |
| 132 | ITU V.32bis, 4800 to 14400 bit/s | Rate chosen from the measured training quality |
| 91 | ITU V.91, 28000 to 62667 bit/s both ways | Two digital modems on an 8000 Hz G.711 line; V.8, then V.34 if V.91 does not start (see [V.91](v91.md#v91-digital-to-digital)). 90 and 92 offer it as well |
| 8, 34, 90, 92 | V.8 and ITU V.34, 2400 to 33600 bit/s; V.90 as the digital modem | V.8 negotiation, then V.34; hands over to the V.32bis pump (with its V.22bis/V.22 fallback) when the other end does not offer V.34. With 90 or 92 on an 8000 Hz G.711 line: V.90 as the digital modem, 28000 to 56000 bit/s downstream (see [V.90](v90.md#v90-digital-modem)) |

V.90 is there as the **digital** modem only (the server side, as on a
G.711 VoIP line), with V.92's quick connect for V.92 analogue modems
calling (see [V.92](v92.md#v92-quick-connect)) and V.92's PCM
upstream (see [PCM upstream](v92.md#v92-pcm-upstream)); the analogue side of
V.90 is under way (phases 1 and 2 so far, with `DSP_V90A=1`: see
[V.90 analogue modem](v90a.md)); V.92's, K56flex and V.8bis are not (`AT+MS=56`,
K56flex, runs V.90). Between two digital modems V.91 (see
[V.91](v91.md#v91-digital-to-digital)) takes the place of V.90. With the default `AT+MS=92` at 9600 Hz the modem connects at V.34 or
lower.

Answering: the core starts the answering pump S92 seconds (2) after
going off hook, which is V.25's silent interval (1.8 to 2.5 s, 4.3/V.25);
the pump stays silent 0.2 s more (V.8's least, 8.2/V.8) and then sends
its answer tone (ANS, ANSam, or Bell's 2225 Hz), 2.2 s after the off
hook, as the original library's pumps do at once after the same delay.

The V.22/V.22bis constellation, quadrant coding and phase tables come from
the original library's `SMCv22_*` and `DECv22_*` tables, and the V.32bis
constellations from its `VTBv32_*` tables.

### V.32 / V.32bis

`dp_v32.cpp` runs the start-up sequence the way the original library does it
(worked out from recordings of it): answer tone, the AA/AC/CA/CC round trip
delay measurement, S, S-bar and TRN (256 two point symbols, then four point
symbols, from the original's fixed scrambler seeds), the R1/R2/R3 rate
words and E. The receivers train an NLMS echo canceller while their own
TRN goes out. Each side offers the rates its training quality supports
(14400 from about 25 dB SNR, 12000 from 22 dB, 9600 from 19 dB, 7200 from
16 dB, else 4800).


### How the QAM receiver works

1. Down-conversion to complex baseband.
2. Root raised cosine matched filter, evaluated at the T/2 sampling
   instants themselves (from a finely tabulated pulse, no interpolator),
   then AGC.
3. Gardner timing loop. It tracks about ±300 ppm of clock offset between
   the two ends.
4. T/2 spaced normalized LMS equalizer.
5. Decision-directed carrier PLL.

The equalizer and the PLL's frequency term only train on scrambled
signals. The unscrambled handshake sequences (ANS, unscrambled ones, S1)
are close to pure tones and would de-tune them. The V.22bis receiver also
tells unscrambled ones and S1 by the phase step from one symbol to the
next, not by its decisions. S1 is two points 90 degrees apart, and while
the carrier phase is near 45 degrees off they fall in the same quadrant,
where the PLL's error averages out. The answerer has only the caller's
100 ms of S1, and over SIP it took a V.22bis caller (one that had tried
V.34 and V.32bis first) for V.22 at 1200 bit/s.

## Code structure

Everything here is in `namespace dsp`, except the C entry points of
`dsplibs.h`, which keep C linkage for the modem core.

- **Data pumps** derive from `Pump` (`pump.h`): `CallProgPump`, `FskPump`,
  `V22Pump`, `V32Pump` and `V34Pump` (which is also V.8 and V.90), and
  the test pumps `DummyPump` and `SinusPump`. A pump has a static
  `name`, `start()` (what was the create function: 0, or -1 if it cannot
  run), `process()`, and optionally `hangup()` and a destructor.
  `PumpOps<T>::ops` is the table registered with the modem core
  (`modem_dp_register`). A pump is allocated with `new T()`, so members
  start at zero, as they did with `calloc`.
- **`PumpBase<P, S>`** (`pump.h`, CRTP on `Pump`) is what the modulation
  pumps share: the core, the sample rate and side, a state of `S` with
  the samples counted in it and in all, the S7 and S10 limits, the
  carrier run, the connect report, the hand over to another pump, the
  tests' settings from the environment, and `pump_loop()`, `process()`
  by the sample for a pump with a `sample()` (FSK, V.22, V.32). The
  call's statistics for the core's `ATI11` (`struct dp_stats`,
  `modem_defs.h`, by `MDMPRM_STATS`) go through it too: `stat_retrain()`
  and `stat_reneg()` (ours or the remote's), `stat_dp()` (the modulation
  V.8's pump came up with: V.34, V.90, V.91, V.92), `stat_snr()` and
  `stat_rtd()`. V.34's echo canceller adds the near and far echo levels,
  V.34 (its receiver's point power over its mse) and V.32bis (its mse)
  the SNR.
- **Mixins** (CRTP, the host the template parameter) hold what a host
  has a part of: `HdlcTx` / `HdlcRx` (`fax_hdlc.h`, the HDLC framing of
  class 1 and of T.30's V.21 and ECM frames), `PcmFrames` (`v90.h`, the
  phase 4 frame sequencer of V.90 and of V.92's PCM upstream), and
  V34Pump's groups in `dp_v34.cpp`: `V34V8`, `V34Qc`, `V34Info0`,
  `V34Probe`, `V34Cc`, `V34Retrain`, `V34Moh`, each with its fields, its
  methods and (V.8, quick connect, retrains and renegotiations,
  modem-on-hold) its states of `handshake()`.
- **Building blocks** are small classes of plain data with methods:
  `Osc`, `Biquad`, `Goertzel`, `PowerMeter`, `Resampler`, `FskMod`,
  `FskDemod`, `FskBitTx`, `CarrierDetect`, `Bell103Detect`, `DtmfGen`,
  `QamTx`, `QamRx`, `TrellisEnc`, `Viterbi`, `EchoCanceller`, the V.34
  and V.90 blocks (`V34Tx`, `V34Rx`, `V34DataTx`, `V34DataRx`,
  `V34Precoder`, `SyncFrames`, `V90Enc` and others), and the fax side
  (`FaxTx`, `FaxRx`, `Fax`, `T30`, `T4Page`, `T4Rx`, `TiffIn`,
  `TiffOut`). They have no constructors: `init()` sets one up and, when
  it owns memory, `release()` frees it. Being plain data, they can live
  inside a pump that is created zeroed. A transmitter that takes its
  symbols or bits from its host (`QamTx`, `FskBitTx`) is given the
  source at `sample()`; the data bits of V.34, V.91 and V.92 go through
  `CoreBits`, the modem core's end.
- **The other library objects**, `DcRemover`, `CallerId` and `Voice`
  (`dsp_glue.cpp`), are classes behind their C entry points (`dcr_*`,
  `CID_*`, `VOICE_*`).
- **The pumps without a modulation**, `dp_dummy.cpp` (`DummyPump`, the
  `DP_DUMMY` test pump: two of them find each other by a tone, then carry
  a bit in each sample) and `dp_sinus.cpp` (`SinusPump`, `DP_SINUS`, a
  test tone), slmodemd's two, written anew on `Pump` and `PumpOps` like
  the others (`ATS32=3`, `ATS32=127`).

The conversion from C structs and functions kept the output the same to
the byte: every quick test produces the same debug log before and after.
