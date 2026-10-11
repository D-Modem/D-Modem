# Open data pumps for slmodemd

This directory is a reimplementation (C++) of the interface that the
binary-only `dsplibs.o` (Smart Link's DSP library) gives to the slmodemd
modem core. It exports the same entry points, so the modem core builds
against it unchanged. slmodemd and libdmodem use only this code; the
original library, `test/dsplibs.o`, is kept as a test peer for the
interop tests.

The interface between the data pumps and a modem core is in this
directory, with C linkage, as the original library has it:
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
G.711 VoIP line), with V.92's quick connect for V.92 analogue modems
calling (see [V.92](#v92-quick-connect)) and V.92's PCM
upstream (see [PCM upstream](#v92-pcm-upstream)); the analogue side of
V.90 and V.92, K56flex and V.8bis are not (`AT+MS=56`,
K56flex, runs V.90). Between two digital modems V.91 (see
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
- **Phase 2:** INFO0/INFO1 on the 600 baud DPSK control channel (the
  receiver looks for the frames, by their sync and CRC, at every
  sampling phase, not only at the timing it follows: the remote's INFO1
  comes at a new phase after its probing signals, and the timing could
  slip at its start), tones A/B
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
- **Non-linear encoding** (9.7/V.34): when the remote's MP asks for it
  (Θ = 0.3125; the Agere modem does, the original library does not), the
  transmitter warps the precoded points and scales them back to the power
  they had without it, as the note in 10.1.3 asks: by the square root of
  the mean power of the constellation over that of its warped points,
  computed the way `frame_power` computes the mean power. The Agere
  receiver undoes that scale from a table of the same values (to 1/512)
  before its non-linear decoder; without the scaling it decodes nothing
  above 24000 bit/s. Our receiver does not ask for it.
- **16 point training:** a J of 0x89B0 from the remote asks for TRN, MP and
  E in the 16 point constellation in phase 4 (MP then takes 4 bits per
  symbol, E is 5 symbols); the receiver accepts both J and the transmitter
  follows the remote's. We send the 4 point J.
- **Asymmetric rates:** both rates are kept only when both MPs set bit 50;
  otherwise both directions take the lower one.
- Pre-emphasis is neither asked for nor applied when the remote asks for
  it.

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

Rate renegotiation (11.6/V.34): a modem whose data mode mse stays
above what its receiving rate runs at with a margin (0.2, in units of
the signal points' spacing) for 300 ms asks for a lower one: S for 128
symbols, S-bar, TRN, then MP with the new rate it can receive, MP', E
and data at the new rates, with the equalizers as they are (the one
from before the precoder's 1 + H(z) goes back in). It takes the rate
down by as many steps as the mse asks for (about 3 dB per bit per
symbol a step), and back up one step at a time, to the rate it
started with, once the mse has left room for the next one for 10 s. A
modem that hears the remote's S in data mode answers the same way,
with the rate its own mse calls for. When the mse got worse by 6 dB
and more the line has changed (both ways, most likely) and the modem
retrains instead; it retrains as well when the renegotiation does not
end with E within 3 s plus the round trips, or the remote sends its
tone. An MP with no rates is a cleardown (11.7/V.34): the call ends.
The original library and the Agere modem answer our renegotiations
(the original library with type 0 MP); the Agere modem starts its own
on errors. `DSP_V34_RENEG=<ms>:<N>[a|c]` makes the answerer (a), the
caller (c) or both start one after that long in data mode, to receive
at N * 2400 bit/s (a test).

Not implemented: pre-emphasis, non-linear decoding (we never ask for
it), the auxiliary channel and starting a cleardown.

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
bit/s downstream and 31200 upstream, mu-law and A-law. Whether its
phase 4 check passes (the ratio of its equalizer's error before and after
TRN2d, limit about 1.5) depends on where the segments fall in its blocks:
the same call passes or fails every time, and a few samples more or less
anywhere change that. When it fails, it asks for a retrain (silence and
tone A) and the digital modem follows it back to phase 2 at the tones,
like a V.34 retrain; the second training passes. The Agere modem as the
analogue modem (`ctest -R agere_v90`) connects at 46666 bit/s downstream;
it trains on TRN1d for 0.6 s and decides on it for another 0.9 s before
it looks for Jd, so TRN1d lasts 1 s (it was 300 ms, enough for the
original library only). Over
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

Rate renegotiation (9.6/V.90): the analogue modem sends S, S-bar, SCR
and CP with the downstream rate it wants; we answer its S-bar with Rd
(R of each interval's largest data mode Ucode, 384 symbols, R-bar for
24, on a data frame boundary), TRN2d, MP with the upstream rate our mse
allows, MP', Ed and B1d, the encoder on CPt's constellations until B1d.
Ours start on the upstream mse as in V.34 (above), with Rd first;
`DSP_V34_RENEG=<ms>:<N>` makes one (a test). A CPs (bit 30) asks for a
silent period, to retrain the analogue modem's echo canceller: MP' until
CPs', Ed, then Ucode 0, keeping the data frames, until a CP with bit 30
clear; then Rt (CPt's largest Ucodes), Rt-bar and MP. No E 5 s after Rd
(plus the round trips): a retrain.

| Test | Result |
|---|---|
| `interop_v90_reneg`: ours after 5 s, upstream to 24000 | 56000/24000 |
| `interop_v90_reneg_silence`: noise on the PCM codes from 40 s | the library's, with CPs and the silent period: 54666 or 50666 down, B1d without errors |
| `agere_v90_reneg_open`: ours, upstream to 19200 | the Agere modem goes along |

The original library renegotiates on its own when the downstream gets
worse, always with the silent period; with the noise still there it now
and then picks 56000 again, and its data then has errors (the test stops
netsim's noise with our silence, `NET_NOISE_QUIET`). The Agere modem
renegotiates on its own as well, without silence, a step down at a time
as the noise rises (45333, 46666, 44000 in one call), but only after
40 s or so, if at all: no test.

### V.92 quick connect

With `AT+MS=92` (the default) at 8000 Hz the digital modem says V.92 in
INFO0d (bit 27) and sends V.92's INFO1d, which offers PCM upstream (bit
70, see [PCM upstream](#v92-pcm-upstream)); without it the analogue modem
asks for V.90 or V.34 upstream, and the call goes on as V.90. `DSP_V92=0` leaves V.92 out,
`DSP_V92_QC=0` answers QC1a with V.8 instead of quick connect.

Short phase 1 (9.2.4/V.92), answering: a QC1a ahead of CM (the
analogue modem's frame on V.21(L), with the Ucode of QTS and LAPM)
is answered with QCA1d on V.21(H) (ANSpcm at -12 dBm0), 75 ms of
silence, QTS, QTS-bar and ANSpcm as PCM codes (Tables 7 to 10/V.92 come
out of the G.711 quantization of Table 6). The analogue modem's TONEq
(980 Hz) ends it; we wait for TONEq to end too, then 75 ms of silence
and phase 2 as after V.8. Sd of phase 3 starts in data frame interval 0
of QTS (8.3.6/V.92). Without TONEq within 2 s, V.8 after all. The
analogue modem may come to phase 2 after our INFO0d is over (the
original library does): once its INFO0a is in, INFO0d goes out once more
with the acknowledgement.

The original library sends QC1a on every call, the Agere modem (with
`AT+PQC=0`) on a second call to a number it has connected to before.
Both then connect at V.90.

Short phase 2 (9.4/V.92): when both modems ask for it, no probing and
no INFO1d; the digital modem reverses B once A is there and B has been
for 50 ms, measures the round trip from the reversal of A and waits for
INFO1a. The upstream rate comes from the mse of the training in phase 3
instead of the probing. `DSP_V92_SHORT2=0` does not ask for it. With
quick connect and short phase 2 the Agere modem's second call connects
in 17.5 s instead of 22.5 s (it asks for a shorter DIL as well).
Modem-on-hold and V.92's fast parameter exchange are not implemented;
rate renegotiation is, with PCM upstream (below).

### V.92 PCM upstream

INFO1d says the channel can carry PCM upstream (bit 70;
`DSP_V92_PCMUP=0` leaves it out). An analogue modem that wants it answers with the INFO1a of
Table 18/V.92, and from there `V92Up` ([v92.cpp](v92.cpp)) runs phases 3
and 4 of V.92 and the data mode on the network's 8000 Hz samples, as
V.91 does; our side of it is V.90's PCM transmitter.

The analogue modem sends PAM symbols at 8000 symbol/s, one per sample:
- Ru and Su (a period of 6 symbols, 1333 Hz) are found by a sliding DFT
  of 12 samples, their phase reversals between two of its windows;
- TRN1u by its correlation with the known scrambled sequence, which is
  the channel's impulse response and gives the symbol alignment (and,
  for the second TRN1u, the data frame alignment);
- a decision feedback equalizer (32 + 16 taps) is fitted to TRN1u by
  least squares and follows the signal by its decisions;
- Ja, CPt and E1u are 2 point signals, TRN2u, SUVu, CPu and E2u 4 point
  (differential sign, scrambled).

We send Sd, TRN1d, Jd, then Jp once Su, Su-bar and Su have been heard
(Su-bar takes half a symbol, and Jp asks for half a symbol more: the
analogue modem goes back to the phase of the first TRN1u), Jp' on the
next Su-bar, the DIL it asked for (or SCR), Ri on CPt, Ri-bar on E1u,
then TRN2d, SUVd, CPd (after SUVu), SUVd' (after CPu), Ed and B1d in
CPt's and CPu's constellations through V.90's encoder.

Our CPd makes the analogue modem's precoder do what the equalizer's
decision feedback did: p1 = -b (16 taps, Q1.14). Its precoder picks, of
the points of a class, the one that keeps its output least, a modulo
precoder: the output stays within the classes' spacing even where 1 / C(z)
is not stable (as it is not here: the channel's edge at 4000 Hz puts a
root of C on the unit circle). The receiver is then left with the feed
forward taps (refitted by least squares to the TRN2u decisions with the
feedback the precoder takes) and the points themselves, without decision
feedback and its error propagation, and with the points in order for the
trellis code: a Viterbi decoder (V.34's 64 state code on the pairs of
symbols, 6.4.4/V.92) decides them before their classes. The rest of
CPd: the same modulus M on all 12 data frame intervals, one constellation
of points (2j + 1) x 16, as many as the precoder needs (M (1 + sum |b|)
positive ones), the prefilter z2(0) = 1, and G for an output of unit
power. The rate is the fastest whose half distance between points is at
least sqrt(8) times the fit's rms error, of the rates in the analogue
modem's DIL descriptor. The feedback taps may be shrunk (b_j g^j) when
that lets a faster rate fit in 128 points. `DSP_V92_UP_DRN=<n>` asks for
(n + 17) x 8000 / 6 bit/s instead; `DSP_V92_PRECODE=0` sends no precoder
(decision feedback in the receiver, margin 40) and `DSP_V92_TRELLIS=0`
does not decode the trellis code (16 or 32 choose that code).

| at 36000 / 38667 bit/s (u-law) | B1u | data |
|---|---|---|
| precoding and trellis decoding | 0 / 0 errors | 0 / 15 HDLC errors in 10 s (64 states; 33 with 16) |
| precoding only | 0 errors at 33333 | 14 HDLC errors at 33333 |
| no precoding | fails at 33333 | |

When the analogue modem retrains instead of sending INFO1a, or our phase
3 or 4 fails, or B1u does not come through (more than 5 % of its bits),
the call retrains without the offer: V.90 with V.34 upstream.

Smart Link's library is a "V.92 Lite" build: its runtime clears the bit
that would let it use PCM upstream (byte 2, bit 7 of `dp_runtime_create`'s
object), and with our offer it retrains to V.34 upstream after INFO1d
("we got PCM upstream under V.92Lite"). `BLOB_V92FULL=1` sets the bit in
`half_blob` ([test/legacy/modem.cpp](test/legacy/modem.cpp)), and the
library then does PCM upstream. Two things it needs of CPd: filter
sections of a multiple of 4 taps (its FIR rounds the length down, and
crashes on 0), and an even number of positive points, 2M at least: on
the trellis frame's fourth symbol it looks for the point of a class up to
2 (K + z M) + parity with z up to (LC - 1 - 2K) / 2M, rounded toward
zero, which takes one point past its table (it sends 0) when LC is odd,
and more when LC is less than 2M.

| Test (`interop_v92_pcmup_*`) | Result |
|---|---|
| ulaw, alaw | V.92 connect, 56000 down, 36000 up, B1u and 10 s of data without errors |
| calls (two calls, quick connect) | both V.92 |
| lite (the library as it is) | it retrains after INFO1d, we leave the offer out: V.90 |
| fallback (44000 bit/s asked for) | B1u does not come through: retrain, V.90 |

Through netsim's converters the fit's error is 34.5 dB below the signal,
which gives 36000 bit/s upstream (V.34's is 31200 there; 38667 makes a
few errors); a prefilter that shapes the analogue modem's spectrum is
not designed yet. In data mode the analogue modem's tone A retrains;
fast parameter exchange is not there. The echo canceller (the V.34 pump's,
trained while we send Sd to Jd) could not be tested: netsim's echo
(`NET_ECHO`) through its 9600 Hz converters is not one an 8000 Hz
canceller cancels, for V.90 either.

Rate renegotiation (9.8/V.92), with PCM upstream: our Rd (R, + + + - - -,
of each interval's largest data mode Ucode, 384 symbols, then R-bar for
24, on a data frame boundary) answers the analogue modem's Ru and Ru-bar,
or starts one of ours; TRN2d, SUVd and CPd go out on CPt's training
constellations, and the receiver takes TRN2u 24 symbols after Ru-bar, on
the data frames' grid, and runs phase 4 again (SUVu, CPu, E2u, B1u, a new
CPd and precoder from TRN2u). Ours start when the margin of the data mode
mse against the one our rate was chosen with drops below half for 0.5 s,
or stays above 2.5 times for 10 s (a minute or more after the last one);
`DSP_V92_RENEG=<ms>[:<drn>]` starts one that far into data mode (to that
rate). When SUVu asks for a silent period (bit 32, Figure 18/V.92), Ed
and then Ucode 0, keeping the data frames, until SUVu with bit 32 clear;
then Rt (of CPt's constellations), Rt-bar and SUVd. A renegotiation that
does not end in data within 25 s, or whose B1u does not come through,
retrains.

| Test (`interop_v92_reneg*`) | Result |
|---|---|
| ours after 3 s of data, to drn 6 | 56000/36000, then 56000/30666 |
| noise up from 26 s (60 rms, `NET_NOISE_DIR=ab`) | ours on its own: 56000/24000 |

The original library starts renegotiations of its own when the line gets
worse downstream (in netsim: `NET_NOISE_DIR=bacodes`, noise added to the
PCM codes) and asks for the silent period; we go through it, but the CPu
it sends afterwards still has its earlier rate while its receiver
expects the new one (B1d, it logs, has half its bits wrong), and it
retrains with tone A. `NET_NOISE_QUIET=<ms>` stops netsim's noise when
the signal it goes onto has been silent that long, to try that on a clean
line; the call goes on after the retrain.

The Agere modem's V.90 training failed at some delays of the line, in
a pattern that repeats every 48 samples (its codec's blocks), with the
line handing it our PCM values as they are, sample for sample. Through
an analogue loop (netsim's `NET_LOOP`, a fraction of a sample of delay
and a high pass) it trains at every delay; the Agere tests use that.

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
equalizer of the last long train at the same rate. After a short train
the original library and spandsp go on with the scrambled ones (segment
4) at once, without the 64 symbol bridge of a long train; so does this
transmitter, with the trellis encoder in the state the original library
starts segment 4 from (its receiver expects it). Its segment 4 is 240
symbols (the original library sends 144) and 80 symbols of ones follow
the data: the original receiver, depending on where its 20 ms frames
fall, starts the data before it has converged and ends it early, and
the ones take both. The receiver takes a short train with or without the
bridge: the first symbols after the training tell which. During a bridge
it decides on the nearest training point instead of the known sequence:
the Agere modem's bridge is not that sequence, and following the wrong
points wound the carrier loop up to a frequency offset of some 3 Hz that
spoiled the first seconds of the data. V.27ter, too, leads the data
with 100 ms of ones (the original library sends some 60 ms): with the 8
symbols of the standard its receiver lost the start of a page now and
then. In data mode the V.17 receiver follows a clear
error in the data's power (6 % or more, over some 500 symbols): the
original library's constellation comes about 1.4 dB below its training.

`ctest -L spandsp` (with libspandsp-dev) puts spandsp's V.17 transmitter
and T.4 encoder, as Asterisk's `SendFAX` uses them, into these receivers.

### Fax machine (T.30)

`t30.cpp` is a group 3 fax machine on these modulations (libdmodem's fax,
the modules' fax applications): the calling machine sends a document,
the answering one receives it. With error correction mode (T.30 Annex A,
T.4 Annex A), which both offer by default (DIS and DCS bit 27, frames of
256 octets), the page goes as its MH coding in blocks of up to 256 HDLC
frames (FCD) on the fast modulation, three RCP after each, then PPS on
V.21 with the page and block counters; the receiver answers MCF or PPR
with a map of the frames to send again. After four PPRs for a block the
sender asks CTC (CTR) and goes on; it answers RNR with RR, and the
receiver its RR, CTC and EOR as T.30 says. The receiver ends a block at
its RCP: the V.17 short training of the next block starts from the
receiver's state at the end of this one, and the V.21 PPS after it would
spoil that.

`fax_pair` puts two of them back to back (`FAX_ECM=1` for ECM, an SNR for
noise: at 19 dB ECM sends a few frames again and the page comes right,
without it two thousand rows come damaged); `t30_spandsp` (label
`spandsp`) runs one against spandsp's fax machine both ways, with and
without ECM, and with line hits.

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

## Building

    cmake -B build                            # in the top directory
    cmake --build build

There is no i386 requirement. It builds natively on x86_64, ARM and other
architectures, and links with `-lm`. Only the interop tests need a 32 bit
compiler (`g++ -m32`), for the original library.

## Tests

    ctest --test-dir build -L quick     # back-to-back pumps, libdmodem, fax
    ctest --test-dir build -L spandsp   # against spandsp (libspandsp-dev)
    ctest --test-dir build -L interop -j8   # calls between the original library and this one,
                                            # data and class 1 fax, in real time (needs g++ -m32)
    ctest --test-dir build -L agere -j8     # calls between the Agere soft modem and this one

The interop and agere calls run in real time and mostly wait, so they run
side by side (`-j`); each writes its modem logs to
`build/dsp/test/logs/<test>/`.

The test programs are in `build/dsp/test/`; `interop.py` and
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
linked with `test/dsplibs.o` on slmodemd's old C core (`test/legacy/`: the
original library calls into it), the other with this code on the modem
core (`../core`). Each call goes through the full modem core: AT commands,
V.42/LAPM detection and data in both directions. Results at the time of writing:

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
| V.17 7200, 9600, 12000, 14400 (long train for TCF, short for the page) | pass | pass |

The original library makes a few bit errors with some of its own signals,
so `fax_interop.py` accepts a page with a bit error rate below 1e-3 (a fax
page without ECM survives that). Its V.27ter training also contains stray
180 degree phase flips, which the open receiver follows.

`fax_loopback <mod> [snr dB] [bits] [srate]` sends random data through
one fax modulation over the simulated line. The V.17 short train codes run
a long train first, so the receiver has an equalizer to start from.

### Against the Agere soft modem

`half_agere` runs the Agere Systems soft modem library (`test/agrsm_core.o`
of agrsm 2.1.40, i386) in user space on an emulated Intel ICH modem
controller and Si3038 codec; `ctest -L agere -j8` calls it from and to the
open modem. Results at the time of writing:

| Agere `AT+MS=` / ours | Agere caller → open answerer | open caller → Agere answerer |
|---|---|---|
| V34 / 34 | CONNECT 33600 (our transmitter; we receive at 24000) | CONNECT 33600 / 24000 |
| V32B / 132, V32 / 32 | CONNECT 14400, 9600 | CONNECT 14400, 9600 |
| V22B / 122, V22 / 22, B212 / 212 | CONNECT 2400, 1200, 1200 | the same |
| V23C / 23, V21 / 21, B103 / 103 | CONNECT 1200/75, 300, 300 | the same |
| V90 / 90 (ours as the digital modem), mu-law and A-law | CONNECT 46666 | - |

Class 1 fax at 8000 Hz (`agere_fax_*`): V.27ter, V.29 and V.17 (7200 to
14400, short train pages) pass both ways.

The original library in its default V.92 mode does V.8 with automode off,
and connects to this implementation at V.34.

`MODEM_REC=<prefix>` makes `modem_half` record its line input and output
(`<prefix>.in`, `<prefix>.out`), and `DBG=<level>` sets the debug level the
interop scripts run the modems with.
