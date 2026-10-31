# Open data pumps for D-Modem: technical details

This is the technical companion to [The data pumps: D-Modem's signal processing](README.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

## What `dsp/` is

`dsp/` is a C++ reimplementation of the interface that Smart Link's
binary-only DSP library, `dsplibs.o`, gives to Smart Link's slmodemd
modem core. It exports the same entry points, so a modem core written
for that library builds against this code unchanged.

dmodemd and libdmodem use only this code. The original library,
`dsp/test/smartlink/dsplibs.o`, is kept only as a partner for the
interop tests.

## The interface to the modem core

The interface is a set of headers in `dsp/`, with C linkage, as in the
original library:

| Header | What it holds |
|---|---|
| `modem_dp.h` | Pump registration (`modem_dp_register`) and the bit exchange with the core (`modem_get_bits`, `modem_put_bits`) |
| `modem_param.h` | The core's parameters, read and set by number |
| `dsplibs.h` | The library's other entry points, and what the core provides to it: tty input and output, debug output, memory |
| `modem_defs.h` | Types and constants, including the S-register numbers and the data pump ids |

The pumps hold a `struct modem *` only as an opaque handle. They include
none of the core's own headers: `dsp/` compiles with only itself and the
shared `include/` directory (for `dm_types.h`) on the include path.

Parameters 1 to 62 in `modem_param.h` are the original library's, with
its numbers and names. Parameters 63 and up are D-Modem's own additions,
which the original library never asks for. They include `MDMPRM_MODE`
(data, fax or voice), `MDMPRM_STATS` (the call's statistics),
`MDMPRM_VCVM`, `MDMPRM_TONES` (the country's tones), `MDMPRM_LINK_REQ`
(a retrain or renegotiation asked for by the DTE) and `MDMPRM_TEXT` (the
text telephone settings).

### Entry points

| Entry point | What it does here |
|---|---|
| `prop_dp_init` / `prop_dp_exit` | Register and deregister all the data pumps listed below |
| `dcr_create`, `dcr_process`, `dcr_reset`, `dcr_delete` | DC removal, with the same algorithm and constants as the original: skip the first 0.6 s, average the next 1 s for the first estimate, then refresh it every 2 s with 0.9/0.1 smoothing |
| `dp_runtime_create`, `dp_runtime_delete` (and `dp_runtime_set_*`) | State shared by the pumps of one call: for example, which answer tone call progress heard, the network's G.711 law, an analogue line, the V.110 rate |
| `CID_*` | Caller ID. FSK at 1200 bit/s, either Bell 202 (1200/2200 Hz, North America) or V.23 (1300/2100 Hz, Europe and the UK), with SDMF and MDMF messages and their checksums; and DTMF caller ID (ETSI TS 101 109 and the countries that use it). Reported as formatted fields (`AT+VCID=1`) or as raw hex |
| `VOICE_*` | IS-101 style voice: samples to and from the DTE in `+VSM`'s format and rate, with `<DLE>` shielding, `+VTS` tones and DTMF. The formats (`voice_codec.h`) are 8-bit signed and unsigned linear, 16-bit linear and IMA ADPCM at 7200, 8000 or 11025 samples/s, and G.711 mu-law and A-law at 8000 |
| `FAX_*`, `FAX_class1_command` | T.31 class 1 fax (`AT+FCLASS=1`): V.21 HDLC, V.27ter, V.29 and V.17 (see [Fax](fax-details.md#fax-class-1)) |
| `FAX2_*` | T.32 class 2 and 2.0 fax (`AT+FCLASS=2`, `2.0`), with T.30 run inside the modem (`fax_class2.cpp`, part of the T.30 library; see [Fax](fax-details.md#fax-class-2-and-20)) |

There is no ring detector entry point (the original library's `RD_*`).
The ring signal a modem codec hears on hook is detected by `RingDetect`
in `dsp.h`.

## Data pumps

Each pump registers under a data pump id (a `DP_ID` in `modem_defs.h`).
The DTE selects one with `AT+MS=` and either the number or a name.

| DP id (`AT+MS=`) | Modulation | Notes |
|---|---|---|
| `DP_CALLPROG` (2) | Call progress | Runs on the calling side before the modulation starts. Dial tone detection (`X2`/`X4`), DTMF or pulse dialling, the dial string's `W` (wait for a dial tone), `@` (wait for quiet after ringing) and `;` (`DPSTAT_DIALED`: the line is held for more digits), the V.25 calling tone, the busy cadence, the answer tone (2100 Hz, 2225 Hz, or V.22's unscrambled ones), and the no-answer timeout |
| 21 | ITU V.21, 300 bit/s FSK | |
| 23 | ITU V.23, 1200/75 bit/s FSK | The answering side sends the 1200 bit/s channel |
| 103 | Bell 103, 300 bit/s FSK | |
| `BAUDOT` (45) | 5-bit (Baudot) text telephone: TTY (45.45 bit/s, 1400/1800 Hz), telex, RTTY | `dp_text.cpp`. Half duplex, with its own character framing (5 bits, 1.5 stop bits, the shift codes) and conversion to and from ASCII on the core's asynchronous link. The rate, tones and figures set come from `AT+MBAUDOT` (`struct dp_text`, `MDMPRM_TEXT`) |
| `EDT` (110) | EDT text telephone: V.21 channel 1 at 110 bit/s, 7 bits, even parity | `dp_text.cpp`, half duplex |
| `CONTACTID` (5) | Contact ID (SIA DC-05) alarm reports | `dp_contactid.cpp`. The answering side is the alarm receiver and the calling side is the panel. DTMF messages of 16 digits with their checksum, the receiver's 1400/2300 Hz handshake and 1400 Hz kissoff, half duplex. Each message is one line to or from the core's asynchronous link. Call progress hands over to it as soon as dialling ends |
| `V18` (18) | V.18 text telephone automoding | `dp_text.cpp`. Listens for a 5-bit, EDT, V.21, V.23 or Bell 103 remote, probes a silent one, then runs that mode (V.21, V.23 and Bell 103 through their own pumps). Call progress hands over as soon as dialling ends, since there is no answer tone to wait for |
| 22 | ITU V.22, 1200 bit/s DPSK | |
| 212 | Bell 212A, 1200 bit/s DPSK | 2225 Hz answer tone, scrambled ones handshake |
| 122 | ITU V.22bis, 2400 bit/s 16-QAM | The full S1 handshake; falls back to V.22 |
| 32 | ITU V.32, 4800 and 9600 bit/s | Uncoded 4800, trellis coded 9600, with an echo canceller |
| 132 | ITU V.32bis, 4800 to 14400 bit/s | The rate is chosen from the measured training quality. Retrains by itself when its equalization is lost. Falls back to V.22bis when the remote does not do V.32 |
| 29 | ITU V.29, 4800 to 9600 bit/s, 4-wire leased line | `dp_4wire.cpp`. The fax modulators of `fax_modems.cpp` run as a duplex pump on a continuous carrier (`AT&L2`): each end's synchronizing signal, the far end's rate found by receivers running in parallel, resynchronizing and fallback (see [leased lines](../leased.md)) |
| 33 | ITU V.33, 12000 and 14400 bit/s, 4-wire leased line | V.17's trellis coded modulation with V.33's rate signals, in `dp_4wire.cpp` |
| 27 | ITU V.27ter, 2400 and 4800 bit/s, 4-wire leased line | In `dp_4wire.cpp` |
| 26, 201 | ITU V.26 (alternative A) and Bell 201 (alternative B), 2400 bit/s DPSK, 4-wire leased line | In `dp_4wire.cpp`. V.27ter's 2400 bit/s symbols with the data unscrambled (`FM_V26_A`, `FM_V26_B`) |
| 208 | Bell 208, 4800 bit/s 8-phase DPSK, 4-wire leased line | In `dp_4wire.cpp`. V.27ter's 4800 bit/s signal, at that rate alone |
| 202 | Bell 202, 1200 bit/s FSK both ways, 4-wire leased line | `dp_fsk.cpp`. 1200 Hz mark and 2200 Hz space; each end sends its mark at once on its own pair |
| `CLEAR` (64) | A clear 64 kbit/s digital channel, such as an ISDN B-channel's data call or a SIP call in CLEARMODE | `dp_clear.cpp`. No modulation: each octet of the line carries 8 bits each way, the first bit the most significant. D-Modem's own start-up exchanges HDLC flags, or V.110 rate adaption (`AT+CBST`, `v110.h`) runs at a user rate of 300 to 56000 bit/s (see [ISDN](../isdn.md)) |
| `VCVM` (150) | V.cvm, data over mobile speech paths, 175 to 3500 bit/s | The proposed Recommendation in [docs/cellular](../cellular/v_cvm.tex). V.8 first (with V.22 and V.21 as the fallback), then probing, then a loaded comb or the parametric mode through AMR, AMR-WB or G.711 (see [V.cvm](vcvm.md)) |
| 91 | ITU V.91, 28000 to 62667 bit/s both ways | Two digital modems on an 8000 Hz G.711 line. V.8 first, then V.34 if V.91 does not start (see [V.91](v91.md#v91-digital-to-digital)). The ids 90 and 92 offer it as well |
| `VWB` (200) | V.wb, D-Modem's wideband mode on a bare pair, up to 148656 bit/s each way | Discrete multitone (DMT) in frequency-division duplex at 48000, 96000 or 192000 Hz; refused below 48000 Hz. Made for pico-serial's WB stick. The line behaves as a leased line (no call progress; `AT&L1`). Its own start-up, per-tone bit loading from the measured SNR, RS(255,239) and a loop-timed answerer (see [V.wb](vwb.md)) |
| 8, 34, 90, 92 | V.8 and ITU V.34, 2400 to 33600 bit/s; V.90 as the digital modem | V.8 negotiation, then V.34. When the other end offers no V.34, the pump hands over to the modulation V.8 agreed on instead (V.32bis, V.22bis, V.23 or V.21), or to the V.32bis pump (with its V.22bis and V.22 fallback) when the other end does no V.8 at all. With 90 or 92 on an 8000 Hz G.711 line it is V.90's digital modem, 28000 to 56000 bit/s downstream (see [V.90](v90-details.md)) |

Two test pumps from slmodemd are also registered, but have no `AT+MS`
code; they are selected through S32, the register that holds `AT+MS`'s
modulation:

- `DP_DUMMY` (3, `ATS32=3`, `dp_dummy.cpp`, `DummyPump`): two of them
  find each other by a tone, then carry one bit in each sample.
- `DP_SINUS` (127, `ATS32=127`, `dp_sinus.cpp`, `SinusPump`): a test
  tone.

### V.90, V.92 and V.91

V.90 is there in both roles:

- **As the digital modem** (the server side, as on a G.711 VoIP line).
  It also has V.92's quick connect for V.92 analogue modems that call it
  (see [V.92](v92-details.md#v92-quick-connect)) and V.92's PCM upstream
  (see [PCM upstream](v92-details.md#v92-pcm-upstream)).
- **As the analogue modem**, on an analogue line at 9600 Hz, with id 90
  or 92 (92 is the default). It gets 56000 bit/s down and 33600 up from a
  V.90 digital modem, and V.34 or below from any other modem, with its
  retrains and rate renegotiations. `DSP_V90A=0` leaves it out (see
  [V.90 analogue modem](v90a.md)). At 8000 Hz it runs only with
  `DSP_V90A=1`.

V.92 as the analogue modem has PCM upstream (up to 48000 bit/s), quick
connect when calling, and modem-on-hold. `DSP_V92A_UP=0` leaves out PCM
upstream, together with its renegotiations and the digital modem's fast
parameter exchange.

K56flex and V.8bis are not implemented. `AT+MS=56` (K56flex) runs V.90;
what is known of K56flex is in [K56flex](k56flex.md).

Between two digital modems, V.91 (see
[V.91](v91.md#v91-digital-to-digital)) takes the place of V.90.

### Answering

1. The core starts the answering pump S92 seconds (default 2) after going
   off hook. This is V.25's silent interval of 1.8 to 2.5 s (4.3/V.25).
2. The pump stays silent for 0.2 s more, V.8's minimum (8.2/V.8).
3. It then sends its answer tone (ANS, ANSam, or Bell's 2225 Hz), 2.2 s
   after going off hook. The original library's pumps send theirs at once
   after the same delay.

### Tables taken from the original library

The V.22/V.22bis constellation, quadrant coding and phase tables come
from the original library's `SMCv22_*` and `DECv22_*` tables. The V.32bis
constellations come from its `VTBv32_*` tables.

### V.32 / V.32bis

`dp_v32.cpp` runs the start-up sequence the way the original library
does it, which was worked out from recordings of that library:

1. The answer tone.
2. The AA/AC/CA/CC exchange that measures the round trip delay.
3. S, S-bar and TRN: 256 symbols of two points, then four-point
   symbols, from the original library's fixed scrambler seeds.
4. The R1/R2/R3 rate words and E.

The receivers train an NLMS echo canceller while their own TRN is going
out. Each side then offers the rates its training quality supports:

| SNR at training | Highest rate offered |
|---|---|
| 25 dB or more | 14400 |
| 22 dB | 12000 |
| 19 dB | 9600 |
| 16 dB | 7200 |
| below 16 dB | 4800 |

A V.32bis call retrains by itself when its receiver's error rises to
4 dB short of the SNR its rate was chosen at (7 dB for 4800).

### How the QAM receiver works

The generic QAM receiver (`QamRx`, `qam.cpp`) used by V.32 and the fax
modulations works in five stages:

1. Down-conversion to complex baseband.
2. A root raised cosine matched filter, evaluated directly at the T/2
   sampling instants (from a finely tabulated pulse, with no separate
   interpolator), then AGC.
3. A Gardner timing loop. Its frequency term is clamped to ±1000 ppm of
   clock offset between the two ends.
4. A T/2 spaced normalized LMS equalizer.
5. A decision-directed carrier PLL.

V.22 and V.22bis (`dp_v22.cpp`) have a receiver of their own with the
same stages, except that it runs a fixed matched filter and then a cubic
interpolator driven by its Gardner loop, and its AGC works on every
sample. The interop tests with the other modems were tuned on it.

The equalizer and the PLL's frequency term train only on scrambled
signals. The unscrambled handshake sequences (ANS, unscrambled ones, S1)
are close to pure tones and would de-tune them.

The V.22bis receiver also tells unscrambled ones from S1 by the phase
step from one symbol to the next, not by its decisions. S1 is two points
90 degrees apart. While the carrier phase is near 45 degrees off, both
points fall in the same quadrant, where the PLL's error averages out.
The answerer has only the caller's 100 ms of S1 to find it in. Over SIP
this used to make the answerer take a V.22bis caller (one that had tried
V.34 and V.32bis first) for a V.22 caller at 1200 bit/s.

## Code structure

Everything here is in `namespace dsp`, except the C entry points of
`dsplibs.h`, which keep C linkage for the modem core.

### Data pumps

Every data pump derives from `Pump` (`pump.h`):

- `CallProgPump` (call progress), `FskPump` (V.21, V.23, Bell 103),
  `TextPump` (V.18, Baudot, EDT), `ContactIdPump`, `V22Pump`, `V32Pump`;
- `V34Pump`, which is also V.8, V.90, V.91 and V.92;
- `FourWirePump` (V.29, V.33, V.27ter), `ClearPump`, `VcvmPump`, and
  `VwbPump` (with `VwbModem`, `vwb.h`, under it);
- the test pumps `DummyPump` and `SinusPump`.

A pump has:

- a static `name`;
- `start()`, which replaces the old create function and returns 0, or -1
  if the pump cannot run;
- `process()`;
- optionally `hangup()` and a destructor.

`PumpOps<T>::ops` is the table registered with the modem core
(`modem_dp_register`). A pump is allocated with `new T()`, so its members
start at zero, as they did when the C pumps were allocated with `calloc`.

### `PumpBase`

`PumpBase<P, S>` (`pump.h`, CRTP on `Pump`) holds what the modulation
pumps share:

- the core, the sample rate and the side;
- a state of type `S`, with the samples counted in the state and in all;
- the handshake's time limit from S7 (50 s if S7 is 0) and the carrier
  loss limit from S10 (1.4 s if S10 is 0);
- the carrier run, the connect report, and the hand-over to another pump;
- the tests' settings from the environment;
- `pump_loop()`, a `process()` that works sample by sample for a pump
  with a `sample()` method (FSK, the text telephones, Contact ID, V.22,
  V.32 and the 4-wire pumps).

The call's statistics for the core's `ATI11` (`struct dp_stats` in
`modem_defs.h`, reached through `MDMPRM_STATS`) also go through
`PumpBase`:

| Method | What it records |
|---|---|
| `stat_retrain()`, `stat_reneg()` | A retrain or a rate renegotiation, ours or the remote's |
| `stat_dp()` | The modulation V.8's pump came up with: V.34, V.90, V.91 or V.92 |
| `stat_snr()`, `stat_nmse()` | The receiver's SNR and mean squared error |
| `stat_carrier()` | Carrier lost and back during data |
| `stat_rtd()` | The round trip delay |
| `stat_line()` | Each direction's symbol rate and carrier |
| `stat_coding()` | V.34's trellis code, non-linear encoder, precoding and shaping, each way |
| `stat_freq_offset()`, `stat_timing_offset()` | The receiver's carrier offset, and the remote's clock against ours |

Which pumps fill in what:

- V.34's pump fills in the line and the coding at each block while
  connected. For a V.90, V.91 or V.92 direction this is PCM's 8000
  symbols/s and no carrier, along with the network's law and the
  analogue modem's DIL (its digital loss and robbed bits).
- V.32bis and V.22bis fill in the line and the offsets. V.22bis gives no
  clock offset, because its timing loop wanders.
- V.34's echo canceller adds the near and far echo levels.
- V.34 (its receiver's point power over its mean squared error) and
  V.32bis (from its mean squared error) give the SNR.

### Mixins

Mixins (CRTP, with the host as the template parameter) hold one part of
what a host does:

- `HdlcTx` / `HdlcRx` (`fax_hdlc.h`): the HDLC framing of class 1 fax and
  of T.30's V.21 and ECM frames.
- `PcmFrames` (`v90.h`): the phase 4 frame sequencer of V.90 and of
  V.92's PCM upstream.
- V34Pump's groups in `dp_v34.cpp`: `V34V8`, `V34Qc`, `V34Info0`,
  `V34Probe`, `V34Cc`, `V34Retrain` and `V34Moh`. Each has its own fields
  and methods, and some (V.8, quick connect, retrains and renegotiations,
  modem-on-hold) their own states of `handshake()`.

### Building blocks

The building blocks are small classes of plain data with methods:

- signal processing: `Osc`, `Biquad`, `Goertzel`, `PowerMeter`,
  `Resampler`, `FskMod`, `FskDemod`, `FskBitTx`, `CarrierDetect`,
  `Bell103Detect`, `DtmfGen`;
- QAM: `QamTx`, `QamRx`, `TrellisEnc`, `Viterbi`, `EchoCanceller`;
- V.34 and V.90: `V34Tx`, `V34Rx`, `V34DataTx`, `V34DataRx`,
  `V34Precoder`, `SyncFrames`, `V90Enc` and others;
- fax: `FaxTx`, `FaxRx`, `Fax`, `T30`, `T4Page`, `T4Rx`, `TiffIn`,
  `TiffOut`.

They have no constructors. `init()` sets one up and, when it owns
memory, `release()` frees it. Because they are plain data, they can live
inside a pump that is created zeroed.

A transmitter that takes its symbols or bits from its host (`QamTx`,
`FskBitTx`) is given the source at `sample()`. The data bits of V.34,
V.91 and V.92 go through `CoreBits`, the modem core's end of the bit
stream.

### The other library objects

`DcRemover`, `CallerId` and `Voice` (`dsp_glue.cpp`) are classes behind
their C entry points (`dcr_*`, `CID_*`, `VOICE_*`).

### The pumps without a modulation

`dp_dummy.cpp` (`DummyPump`) and `dp_sinus.cpp` (`SinusPump`) are
slmodemd's two test pumps (see [Data pumps](#data-pumps)), written anew
on `Pump` and `PumpOps` like the others.

The conversion from C structs and functions to these classes kept the
output identical to the byte: every quick test produced the same debug
log before and after.
