# Open data pumps for slmodemd

This directory is a reimplementation (C++) of the interface that the
binary-only `dsplibs.o` (Smart Link's DSP library) gives to the slmodemd
modem core, declared with C linkage in `../dsplibs.h`. It exports the same
entry points, so the modem core builds against it unchanged. slmodemd and
libdmodem use only this code; the original library, `test/dsplibs.o`, is
kept as a test peer for the interop tests:

| Entry point | What it does here |
|---|---|
| `prop_dp_init` / `prop_dp_exit` | Register / deregister the data pumps below |
| `dcr_create/process/reset/delete` | DC removal. Same algorithm and constants as the original (skip 0.6 s, average 1 s, then 0.9/0.1 smoothing every 2 s) |
| `dp_runtime_create/delete` | State shared by the pumps of one call (for example, which answer tone call progress heard) |
| `RD_*` | Ring detector for snooping codecs (low frequency, high amplitude bursts) |
| `CID_*` | Caller ID: Bell 202 FSK, SDMF and MDMF, formatted (`AT+VCID=1`) or raw hex |
| `VOICE_*` | IS-101 style voice: 8 bit linear at 8000 Hz with `<DLE>` shielding, `+VTS` tones and DTMF |
| `FAX_*`, `FAX_class1_command` | T.31 class 1 fax (`AT+FCLASS=1`): V.21 HDLC, V.27ter, V.29 and V.17 (see [Fax](#fax-class-1)) |

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
| 91 | ITU V.91, 28000 to 62667 bit/s both ways | Two digital modems on an 8000 Hz G.711 line; V.8, then V.34 if V.91 does not start (see [V.91](#v91-digital-to-digital)). 90 and 92 offer it as well |
| 8, 34, 90, 92 | V.8 and ITU V.34, 2400 to 33600 bit/s; V.90 as the digital modem | V.8 negotiation, then V.34; hands over to the V.32bis pump (with its V.22bis/V.22 fallback) when the other end does not offer V.34. With 90 or 92 on an 8000 Hz G.711 line: V.90 as the digital modem, 28000 to 56000 bit/s downstream (see [V.90](#v90-digital-modem)) |

V.90 is there as the **digital** modem only (the server side, as on a
G.711 VoIP line); the analogue side of V.90, V.92, K56flex and V.8bis are
not (`AT+MS=56`, K56flex, runs V.90). Between two digital modems V.91 (see
[V.91](#v91-digital-to-digital)) takes the place of V.90. With the default `AT+MS=92` at 9600 Hz the modem connects at V.34 or
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

### V.8 and V.34

`dp_v34.cpp` runs the start-up, `v34_modem.cpp` the primary channel training
signals and receiver, `v34_data.cpp` the data mode, `v34_tables.cpp` the
constants (the superconstellation, Table 10 rates, the PP sequence).

- **V.8:** ANSam (2100 Hz, 15 Hz AM, reversals every 450 ms), CM/JM/CJ on
  V.21. A CM or JM without V.34, or a V.32 calling modem (1800 Hz AA),
  hands the call to the V.32bis pump. As in V.32bis Annex A, that falls
  back to V.22bis after 3 s, and V.22bis to V.21 after 3 s more of
  unanswered USB1, V.21 to V.23 and V.23 to Bell 103 after 3 s more
  each (FSK callers wait for the answerer's mark). A Bell 103 caller
  usually takes V.22's unscrambled ones (2250 Hz) for its answerer's
  2225 Hz mark and answers with its own (1270 Hz): 100 ms of that tone
  in any of these stages switches to Bell 103 at once.
- **Calling automode:** a 2225 Hz answer tone (told from V.22's 2250 Hz
  unscrambled ones over 200 ms) means a Bell answerer: Bell 212A, and Bell
  103 when no scrambled ones come back within 1.5 s. After a 2100 Hz tone
  without ANSam, the V.32bis caller goes to V.22bis on unscrambled ones,
  V.21 on a 1650 Hz mark or V.23 on a 1300 Hz mark. FSK callers check that
  the mark is at their own frequency (V.21's mark demodulates as a V.23
  one).
- **Phase 2:** INFO0/INFO1 on the 600 baud DPSK control channel, tones A/B
  and their reversals for the round trip delay, and line probing with L1/L2
  (21 tones, 150 Hz apart). The probe gives the SNR after equalization for
  each symbol rate, from the received level of each tone and the noise
  between successive periods (insensitive to clock offsets). The answerer
  picks the symbol rate that allows the highest data rate at both ends.
- **Phases 3 and 4:** S, S-bar, PP, TRN, J, J', MP, E as the original library
  sends them. TRN starts with a cleared scrambler (the original's receiver
  trains on a copy of it). The receiver solves the equalizer by least
  squares over 1500 TRN symbols, since LMS is far too slow for the accuracy
  33600 bit/s needs on a band limited line. Two solutions in phase 3 and one
  in phase 4 measure the sample clock offset from the drift of the
  equalizer's delay. From then on the timing is decision directed: the error
  against the slope of the equalizer output, for any constellation.
- **Data mode:** shell mapping, the superconstellation with expanded
  shaping, differential encoding, the 16, 32 and 64 state 4D trellis
  codes (we ask for the 64 state one; the original library sends what it
  is asked for and asks for 16), high and
  low mapping frames and the superframe sync inversions, at every rate and
  symbol rate of Table 10 except the auxiliary channel ones. As the original
  library does: the trellis output of a 4D symbol is the low bit of the
  encoder state before that symbol, and the inversion pattern starts at its
  bit 2J - 2. The 32 and 64 state encoders take the same inputs as
  the original's `Convolve32` and `Convolve64` tables. The receiver runs a
  Viterbi decoder over the states of the code with
  a decision delay of 32 4D symbols (for the equalizer and timing it still
  takes the nearest point). It gains 3 to 4 dB over deciding on the nearest
  points: with the 16 state code the bit error rate stays below 1e-5 from
  an SNR of 3 dB per bit per symbol plus 4 dB, at every symbol rate (the
  32 and 64 state codes need 0.5 to 1 dB less), and the rate selection
  asks for 2 dB more than that.
- **Precoding** (9.6.2/V.34): the transmitter precodes with the
  coefficients the remote's MP asks for. The receiver asks for them when
  they gain at least 1 dB: the third order linear predictor of the
  equalizer's error after training, so that the error through 1 + H(z) is
  white. In data mode its equalizer then includes 1 + H(z), decides on the
  channel output points y(n) over the whole grid, and runs a copy of the
  remote's precoder on the decided points to recover u(n). The original
  library precodes when asked, and its signal decodes without errors with
  the coefficients we compute and with stronger ones.
- Non-linear encoding and pre-emphasis are neither asked for nor applied
  when the remote asks for them.

- **Echo canceller:** real pass band NLMS on the line samples, structured
  like the one in Agere's V.34 (`agrmodemlib.o`, read for reference): a
  near end part over the last 27 ms of our signal and a far end part over
  27 ms around the echo from the other end's hybrid, with one shared error.
  Both train while our own phase 3 sequences go out and the remote is
  silent (the caller waits for the answerer's J to stop), fast, then
  slower. Where the far echo is gets measured: the near part's residual
  is correlated with our scrambled signal at every delay up to 1.7 s (over
  VoIP the echo comes back after the network delay). The tone detectors of
  phase 2 listen exactly on a bin of the 64 sample probe period, where the
  echo of our own L1/L2 has nothing, and the reversal detector does not
  depend on the symbol timing.

Retrains: in data mode a modem that loses the remote's signal for 30 ms,
hears its tone A or B instead of data, or sees its error (mse) stay above
what any rate needs for a second goes back to phase 2 at the tones (70 ms
of silence, then A or B; INFO0 stands), probes the line again and trains
anew; the link (V.42) carries on at the new rates without a new CONNECT.
As there is no INFO0 to find the control channel's symbol timing with,
the receiver takes it from the tone reversals. `DSP_V34_RETRAIN=<ms>[a|c]`
makes the answerer (a), the caller (c) or both start one after that long
in data mode (a test).

Not implemented: non-linear encoding, pre-emphasis, the auxiliary
channel and rate renegotiation.

### V.90 digital modem

`v90.cpp` is the digital modem's PCM transmitter, `dp_v34.cpp` runs its
start-up on top of the V.34 pump. The modem has to run at 8000 Hz on a
G.711 network (`MODEM_RATE=8000`): the pump sends exact codewords, as
their linear values, that the network's encoder passes on unchanged. The
law is mu-law, or A-law with `DSP_G711=alaw`.

- **Phases 1 and 2:** PCM availability (digital) in CM or JM, then the
  part of the V.34 call modem whichever side called: INFO0d, tone B,
  probing, INFO1d; INFO1a asks for V.90 (symbol rate 6, UINFO) or V.34.
- **Phase 3:** the analogue modem's S, PP, TRN and Ja (with the DIL
  descriptor) go into the V.34 receiver; Sd, TRN1d, Jd, Jd' and the DIL
  it asked for come out as PCM. While it takes in the DIL the analogue
  modem is silent or sends TRN1a; the S that ends the DIL is looked for
  after either.
- **Phase 4:** CPt, CP and E come in; Ri, TRN2d, MP, Ed and B1d go out
  through the encoder with the CPt and then the CP constellations.
- **Data mode:** the encoder of 5.4/V.90: modulus encoder, mappers, sign
  coding and the spectral shaper with its look-ahead (the exact choice of
  5.4.5.5: the analogue modem checks TRN2d against it). Upstream is the
  V.34 data mode receiver, with the trellis code and precoding our MP asks
  for. The echo canceller takes our PCM signal (converted to 9600 Hz)
  and trains while Sd, TRN1d and Jd go out (the analogue modem is meant
  to be silent then). It stays off unless that training took at least
  6 dB away: on a 4-wire network there is no echo, and a canceller
  trained on the remote's signal would add noise. With the hybrid echo of netsim 20 dB down, or 12 dB down after
  200 ms, calls still connect at 56000/31200; 12 dB down after 20 ms is
  too much at times: the echo of the DIL's loudest codes hides the
  analogue modem's S.

The original library, as an analogue V.90 modem calling through the
simulated network (`ctest -R interop_v90`), connects at 56000
bit/s downstream and 31200 upstream, mu-law and A-law (A-law is
marginal there: the original library retrains in phase 4 at times). Over
SIP, through Asterisk and DModem (`asterisk/test/sip.sh AT+MS=90`, with
`CODEC=alaw` for A-law), it connected at 56000/31200 in both laws while
slmodemd still ran the original library at 9600 Hz; its SIP side then did
the 8000/9600 Hz conversion itself (the resampler of pjmedia cuts below
4000 Hz, which takes away the part of the PCM signal the analogue modem
recovers its timing from). slmodemd now runs at the call's 8000 Hz. The
original library's own debug
output (its `edprintf` lines are hidden behind a key) helped: it reports
the levels it measures in the DIL, the digital pad it concludes and why
it retrains.

### V.91 digital to digital

`v91.cpp` is ITU-T V.91 (05/99), started by the V.34 pump when both CM and
JM have V.91 in their PCM modem availability octet (b7): two digital
modems, as D-Modem calling D-Modem or the Asterisk module over SIP. Both
send PCM codes; both run the same start-up (8.2.1/V.91):

- **Ez, INFO, INFO':** INFO (Table 6/V.91) coded differentially into the
  sign of Ucode 66; INFO' once the remote's INFO is in. We always ask for
  the default DIL (Table 5/V.91) and never for transparent mode (through
  pjsua's 16 bit linear audio the mu-law codes +0 and -0 are one).
- **Eu, or J, PHIL and Em:** the DIL the remote asked for: the default one
  after Eu, or the one described in its J (Table 12/V.90, as V.90's Ja)
  after Em.
- **DIL:** the receiver notes, for each of the 6 data frame intervals,
  the value each code and sign arrives as. A code is usable when both of
  its signs arrive the same each time, different from each other and from
  the codes kept already; where codes become one (robbed bits, a pad, a
  conversion between the laws), one of them is kept. Those are the
  constellations it asks for in CP, and data are decoded by the nearest
  of their received values.
- **SCR, CP, CP', Es, B1:** CP has the layout of V.90's CP (Table 4/V.91),
  with the rate the receiver can take: K bits through the modulus encoder,
  K up to 41 (62667 bit/s; 64000 would need all 128 codes, and
  transparent mode).
- **Data mode:** the V.90 encoder with Sr = 0 and S = 6, and its inverse.
  The control channel (bit 27 of INFO, every 64th frame's sign bits) is
  sent when the remote asks for it (`DSP_V91_CC=1` asks).
- **Retrain:** a retrain is INFO again: the modem sends it when more than
  5% of the symbols in a second are off the constellation, and answers the
  remote's INFO in the data. V.42 carries on.
- **Escape to V.34:** no INFO' in 1.5 s or no B1 in 10 s: V.34 phase 2
  from INFO0 on (8.3/V.91).

The core's DC remover leaves the received codes alone while V.91 runs
(`dp_runtime_pcm`). `DSP_V91=0` leaves V.91 out of V.8, `DSP_V91_K`
limits K.

`dmodem_pair` puts a G.711 network between the two modems with
`DMODEM_NET` (`ctest -R v91`):

| `DMODEM_NET` | Codes per interval | Rate |
|---|---|---|
| `g711`, mu-law | 124 (+0 and -0 are one) | 62667 |
| `g711`, A-law | 125 | 62667 |
| `rbs` (LSB of every 6th octet set) | 124 and once 62 | 61333 |
| `pad` (3 dB), mu-law / A-law | 99 / 105 | 60000 / 61333 |
| `xlaw` (mu-law to A-law) | 117 | 62667 |

Through V.42, 200 kB that do not compress go across at about 58700 bit/s
(V.34 at 33600: 31400). Over SIP, `asterisk/test/sip.sh` (slmodemd calling
DModem in Asterisk, also with `CODEC=alaw` and `INCOMING=1`) connects at
62666/62666.

### Fax (class 1)

`fax_class1.cpp` implements the T.31 class 1 commands on top of the
modulations in `fax_modems.cpp`:

| Command | |
|---|---|
| `+FTH=3` / `+FRH=3` | V.21 channel 2 HDLC frames: flags, bit stuffing, CRC-16, `<DLE>` shielding, `OK`/`ERROR` per received frame |
| `+FTM=` / `+FRM=` | 24, 48 (V.27ter), 72, 96 (V.29), 73, 74, 97, 98, 121, 122, 145, 146 (V.17, long and short train) |
| `+FTS=` / `+FRS=` | Silence for / wait for silence of n * 10 ms |

The answering side sends CED (2100 Hz for 3 s) and then starts `+FTH=3` by
itself; the calling side sends CNG while it waits for an answer and starts
`+FRH=3` when the answer tone is heard. Any character from the DTE stops
`+FRH`/`+FRM` once they reported `CONNECT`.

The receivers find the training by matching the received phase steps
against the known sequence. The V.29 and V.27ter training sequences repeat
every 127 symbols, so the end of the alternations before them tells which
repetition is being received. A V.17 short train continues from the
equalizer of the last long train at the same rate. The original library
sends the 64 symbol bridge after a short train as after a long one,
spandsp goes on with the scrambled ones at once: the first symbols after
the training tell which. In data mode the V.17 receiver follows a clear
error in the data's power (6 % or more, over some 500 symbols): the
original library's constellation comes about 1.4 dB below its training.

`ctest -L spandsp` (with libspandsp-dev) puts spandsp's V.17 transmitter
and T.4 encoder, as Asterisk's `SendFAX` uses them, into these receivers.

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
are close to pure tones and would de-tune them.

## Code structure

Everything here is in `namespace dsp`, except the C entry points of
`../dsplibs.h`, which keep C linkage for the modem core.

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

The conversion from C structs and functions kept the output the same to
the byte: every quick test produces the same debug log before and after.

## Building

    cmake -B build                            # in the top directory
    cmake --build build

There is no i386 requirement. It builds natively on x86_64, ARM and other
architectures, and links with `-lm`. Only the interop tests need a 32 bit
compiler (`g++ -m32`), for the original library.

## Tests

    ctest --test-dir build -L quick     # back-to-back pumps, libdmodem, fax
    ctest --test-dir build -L spandsp   # against spandsp (libspandsp-dev)
    ctest --test-dir build -L interop   # calls between the original library and this one,
                                        # data and class 1 fax, in real time (needs g++ -m32)

The test programs are in `build/slmodemd/dsp/test/`; `interop.py` and
`fax_interop.py` take the two modem programs as arguments (`NETSIM` and
`LOG_DIR` in the environment for netsim and the logs).

`dsp_loopback <dp> [snr dB] [seconds] [srate] [line gain] [clock ppm]
[echo dB]` runs a caller and an answerer through a simulated line with
delay, a low-pass filter (`FLAT=1` leaves it out), white noise and an
optional sample clock offset. It then checks the bit error rate in both
directions. `DUMP=<prefix>` records what each side hears.

V.34 between two open modems (`dsp_loopback 8 ...`, with the low-pass
filter, no bit errors): 33600 bit/s at 40 dB (also with ±80 ppm clock
offset, or with near and far echo 6 and 10 dB below our signal), 31200
at 38 dB, 28800 at 35 dB, 26400 at 33 dB, 24000 at 30 dB, 21600 at
28 dB, 16800 at 25 dB, 14400 at 22 dB. `[echo dB] [far echo dB]` add the echoes;
`FAR_DELAY=<samples>` delays the far one (200 ms: still 31200 bit/s).

`interop` connects two `modem_half` processes with pipes. One process is
linked with `test/dsplibs.o` and the other with this code. Each call goes through
the full modem core: AT commands, V.42/LAPM detection and data in both
directions. Results at the time of writing:

| `AT+MS=` | original caller → open answerer | open caller → original answerer |
|---|---|---|
| 34 (V.34) | CONNECT 33600 | CONNECT 33600 |
| 132 (V.32bis) | CONNECT 14400 | CONNECT 14400 |
| 122 (V.22bis) | CONNECT 2400 | CONNECT 2400 |
| 22 (V.22) | CONNECT 1200 | CONNECT 1200 |
| 212 (Bell 212A) | CONNECT 1200 | CONNECT 1200 |
| 103 (Bell 103) | CONNECT 300 | CONNECT 300 |
| 23 (V.23) | CONNECT 1200 | CONNECT 1200 |
| 21 (V.21) | CONNECT 300 | fails: the original V.21 answerer only sends 2225 Hz and does not connect to itself either |

`NET=<rate A>:<rate B>:<ulaw|alaw>` puts `netsim`, a simulated G.711
network, between the two: each direction goes through the law at 8000 Hz,
and to a modem at 9600 Hz through a converter that acts like a codec on
an analogue line (a raised cosine at 4000 Hz, so that PCM samples stay
free of intersymbol interference). `half_blob8`/`half_open8` are the
modems at 8000 Hz (the original library does not work at 8000 Hz). `NET_GAIN`
sets an analogue gain on the way to the 9600 Hz modem, and
`NET_ECHO=<dB>:<ms>` the echo of a hybrid at its line (in its own sample
clock: what it receives comes back with what it sends).

`fax_interop` does the same with `AT+FCLASS=1`. `fax_interop.py` plays
the T.30 side of both fax machines: CSI/DIS, DCS, TCF, CFR, one page (for
V.17 with the short train), EOP, MCF and DCN. The modems run in real time
so that the script keeps up with the T.30 timers. Results at the time of
writing:

| Modulation | original sender → open receiver | open sender → original receiver |
|---|---|---|
| V.27ter 2400, 4800 | pass | pass |
| V.29 7200, 9600 | pass | pass |
| V.17 7200, 9600 (long and short train) | pass | pass |
| V.17 12000 | pass | pass |
| V.17 14400 | fails: the original transmitter's signal is too noisy at this rate (it cannot receive itself either) | pass |

The original library makes a few bit errors with some of its own signals,
so `fax_interop.py` accepts a page with a bit error rate below 1e-3 (a fax
page without ECM survives that). Its V.27ter training also contains stray
180 degree phase flips, which the open receiver follows.

`fax_loopback <mod> [snr dB] [bits] [srate]` sends random data through
one fax modulation over the simulated line. The V.17 short train codes run
a long train first, so the receiver has an equalizer to start from.

The original library in its default V.92 mode does V.8 with automode off,
and connects to this implementation at V.34.

`MODEM_REC=<prefix>` makes `modem_half` record its line input and output
(`<prefix>.in`, `<prefix>.out`), and `DBG=<level>` sets the debug level the
interop scripts run the modems with.
