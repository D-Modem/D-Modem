# Open data pumps for slmodemd

`dsp/` is a reimplementation (C++) of the interface that the
binary-only `dsplibs.o` (Smart Link's DSP library) gives to the slmodemd
modem core. It exports the same entry points, so the modem core builds
against it unchanged. slmodemd and libdmodem use only this code; the
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
| `DP_CALLPROG` | Call progress | Dial tone (X2/X4), DTMF dialing, V.25 calling tone, busy cadence, answer tone (2100 Hz / 2225 Hz / V.22 unscrambled ones), no-answer timeout |
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
V.90 and V.92, K56flex and V.8bis are not (`AT+MS=56`,
K56flex, runs V.90). Between two digital modems V.91 (see
[V.91](v91.md#v91-digital-to-digital)) takes the place of V.90. With the default `AT+MS=92` at 9600 Hz the modem connects at V.34 or
lower.

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
  `V22Pump`, `V32Pump` and `V34Pump` (which is also V.8 and V.90). A pump
  has a static `name`, `start()` (what was the create function: 0, or -1
  if it cannot run), `process()`, and optionally `hangup()` and a
  destructor. `PumpOps<T>::ops` is the table registered with the modem
  core (`modem_dp_register`). A pump is allocated with `new T()`, so
  members start at zero, as they did with `calloc`.
- **Building blocks** are small classes of plain data with methods:
  `Osc`, `Biquad`, `Goertzel`, `PowerMeter`, `Resampler`, `FskMod`,
  `FskDemod`, `DtmfGen`, `QamTx`, `QamRx`, `TrellisEnc`, `Viterbi`,
  `EchoCanceller`, the V.34 and V.90 blocks (`V34Tx`, `V34Rx`,
  `V34DataTx`, `V34DataRx`, `V34Precoder`, `V90Enc` and others), and the
  fax side (`FaxTx`, `FaxRx`, `Fax`, `T30`, `T4Page`, `T4Rx`, `TiffIn`,
  `TiffOut`). They have no constructors: `init()` sets one up and, when
  it owns memory, `release()` frees it. Being plain data, they can live
  inside a pump that is created zeroed.
- **The other library objects**, `DcRemover`, `RingDetector`, `CallerId`
  and `Voice` (`dsp_glue.cpp`), are classes behind their C entry points
  (`dcr_*`, `RD_*`, `CID_*`, `VOICE_*`).
- **slmodemd's own pumps without a modulation**, `dp_dummy.cpp` (the
  `DP_DUMMY` test pump) and `dp_sinus.cpp` (`DP_SINUS`, a test tone), are
  Smart Link's C code as it came with slmodemd, registered the same way.

The conversion from C structs and functions kept the output the same to
the byte: every quick test produces the same debug log before and after.
