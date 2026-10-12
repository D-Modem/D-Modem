# Open data pumps for slmodemd

This directory is a reimplementation (C++) of the interface that the
binary-only `dsplibs.o` (Smart Link's DSP library) gives to the slmodemd
modem core. It exports the same entry points, so the modem core builds
against it unchanged. slmodemd and libdmodem use only this code; the
original library, `test/smartlink/dsplibs.o`, is kept as a test peer for
the interop tests.

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
bit/s downstream and 31200 upstream, mu-law and A-law, on a line without
noise. Its phase 4 check compares the error of its equalizer output
against the levels it expects for TRN2d's codes over the first 7200
symbols of TRN2d with that over the next 7200; in between, it replaces
the levels it took from the DIL with those it measured in TRN2d. More
than about 1.5 times the error before, and it retrains (silence and tone
A; the digital modem follows it back to phase 2 at the tones, like a
V.34 retrain). On a line without any noise the error is only a unit or
two of its 16 bit scale, and its levels from the DIL are off by about
that much (they are 4 times the G.711 values of the codes, give or take
2), so with mu-law the check fails every time: 1.96 against 1.23, and
1.78 on the second training, which it lets pass (one retrain at most).
The codes come through as sent; any digital modem would do the same.
With a noise floor of -81 dBFS (netsim's `NET_FLOOR=3`, still far below a
real line's) the error is that of the noise, the ratio 1.2 to 1.3 at
every alignment of the call in both laws, and the call connects at
54666/31200 without the retrain, 15 s sooner; the mu-law V.90 tests use
it. The Agere modem as the
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

QC1a comes twice, and the analogue modem goes on with CM after it until
it hears QCA1d. The first QC1a starts as the V.21 receiver picks up the
carrier, so its sync is taken with fewer than its ten leading ones: the
Agere modem kept sending CM when our QCA1d came only after the second
one (at some delays of the line). If it misses QCA1d all the same, the
CM it goes on with is answered with JM (V.8, full phase 2); TONEq, V.21's
low channel mark, counts only after 128 ms without that channel's space,
so that CM's runs of marks do not pass for it.

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
Rate renegotiation and fast parameter exchange are implemented, with PCM
upstream (below), and modem-on-hold:

### V.92 modem-on-hold

When call waiting interrupts the analogue modem, it can ask us to hold
the call (9.10/V.92): its tone A, as for a retrain, then MH sequences
(Table 32/V.92: fill, frame sync, a 4 bit signal and 4 bits of
information, CRC, on the modulation of INFO) instead of the reversal. We
look for them while we retrain and answer:

- MHreq with MHack and the time we hold the call, T1 (`AT+PMHT=<code
  of Table 33/V.92>`, 5 = 1 min by default, 13 no limit), or with MHnack
  (`AT+PMHT=0` or `AT+PMH=1`; `DSP_V92_MOH=<code>` overrides both). After MHack, when its tone has been there 100 ms or
  silence 2 s, we send ANSam until T1 is over (then we disconnect) and
  listen for QC1a or CM: phase 1 as the answering modem again, phases 2
  to 4, and the data goes on without a new CONNECT (V.42's timers count
  bits, so they wait too). A CM without modulations, or QC1a with UQTS
  1111, clears down.
- MHclrd with MHcda, then disconnect; MHfrr (also after our MHnack) with
  ANSam and phase 1 at once; MHcda after our MHnack: disconnect.

The Agere modem asks for it with `AT+PMHR` (`+PMHR:5`: 1 min granted,
`+PMHR:0` denied) and comes back with CM after `ATO`; denied, it asks for
a fast reconnect. `agere_v92_moh_grant` and `_deny` do that in the
middle of the data (`MIDCALL_A` of `interop_call`) and check all of it both
ways.

We ask for it too, with `AT+PMHR` in online command mode on a V.92
connection (`AT+PMHR=<s>`: hold the remote that long, 10 s otherwise;
`DSP_V92_MOH_REQ=<ms>[:<s>]` asks that far into data mode): out of data
mode as for a retrain, tone B for 600 ms, then MHreq repeated until the
answer (9.10.2.1/V.92):

- MHack: silence (the remote is on hold) for those seconds, or until 2 s
  before its T1, then ANSam and phase 1 as the answering modem, as after
  its own hold;
- MHnack: MHfrr, and ANSam and phase 1 after it (with `=0` seconds of
  `DSP_V92_MOH_REQ`, MHcda and a disconnect instead);
- the remote's tone A reversed (or its INFO0a): it takes our tone for a
  retrain's, which it may (9.10.1.1/V.92); we follow it into phase 2 at
  once. The tone is long so that this comes before MHreq, whose phase
  steps it would take for our reversal;
- nothing in 2.5 s: a retrain.

Neither the original library nor the Agere modem answers MHreq: both
retrain (the library frames our MH sequences, but as INFO0 in its
retrain, with a bad CRC; it looks for MH only in its own requests).
`interop_v92_moh_request` (PCM upstream) and `agere_v92_moh_request`
(V.90) ask for it in the middle of the data (`MIDCALL_B`) and check that
the call goes on.

For the MHack and MHnack branches netsim stands in for an analogue
modem that answers (`NET_MOH=ack[:<T1 code>]@<s>` or `nack@<s>`): from s
seconds into the line, once our tone B has been there 50 ms, it cuts the
modem off both ways and sends tone A, then MHack or MHnack, until we go
silent (on hold) or send ANSam, and gives the line back with ANSam.
`interop_v92_moh_request_ack` (T1 10 s, `AT+PMHR=5`) and `_nack` check
the sequence in our log (`EXPECT_DROP` of `interop_call`: the call is to
end): MHack, 5 s on hold, ANSam; MHnack, 300 ms of MHfrr, ANSam. The
original library behind it has been cut off in a retrain and hangs up,
so nothing comes back and we disconnect after T1: the way back (CM or
QC1a after our ANSam) is the same as after the analogue modem's own
hold, which `agere_v92_moh_grant` tests.

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
| ulaw, alaw | V.92 connect, 56000 down, 38666 / 40000 up (prefilter), B1u and 10 s of data without errors |
| noprefilter (`DSP_V92_PREFILTER=0`) | 56000 down, 34000 to 36000 up |
| calls (two calls, quick connect) | both V.92 |
| lite (the library as it is) | it retrains after INFO1d, we leave the offer out: V.90 |
| fallback (44000 bit/s asked for) | B1u does not come through: retrain, V.90 |

Through netsim's converters the fit's error is 34.5 dB below the signal,
which gives 36000 bit/s upstream with the precoder alone (V.34's is 31200
there; 38667 makes a few errors).

With the prefilter (6.4.2/V.92; `DSP_V92_PREFILTER=0` turns it off) the
analogue modem's points are G.711's own levels, so that what reaches the
network's codec is a level it passes unchanged. The design, from the
last 3584 symbols of TRN2u:

- the channel, ch (24 taps before the main one, 40 after), by least
  squares from the TRN2u decisions to the received samples, and the noise
  (the fit's error less the codec's quantisation, plus the fit's own
  estimation error);
- the prefilter w (64 taps, z2 in Q0.15 with 4G in Q0.16) that makes
  ch * w monic at a delay d (searched up to 40), least squares over the
  taps before d, and the precoder (32 taps, p1 in Q1.14) the taps after;
- for each rate, the most widely spaced G.711 levels that keep the
  prefilter's output power within 1 (M (1 + sum |b|) + 2 of them, even),
  and a simulation of 4000 symbols of random data through ch, the noise
  and the codec: the fastest rate with 2 errors or fewer.

The receiver takes the sample at the delay through 9 taps (LMS, for the
analogue modem's timing as it wanders), less what the channel still has
of the symbols before B1u, and decides on a grid on which point j is at
2j + 1 (linear between the levels), for the same Viterbi decoder.
Through netsim that is 38666 bit/s with mu-law, 40000 to 41333 with
A-law.

The design holds only as long as the analogue modem's transmitter keeps
its timing against the network's codec. The original library slips: it
locks its transmitter to its estimate of our clock, which is off by a few
hundredths of a ppm (its log's "Timing Offset", +0.02 to -0.05 ppm; netsim
has no offset at all), so its signal moves by some 0.005 samples in 40 s.
Fitted to the data symbols, the channel changes by just that shift (about
1 % by 40 s), and the network codec's samples stray from the levels: at
levels 300 to 1000, 9 % of them come out on a neighbouring level at the
start, 44 % after 40 s. That happens before the network's quantizer, so
nothing in our receiver can take it back (more taps, faster LMS and
taps on the precoder's output made no difference). The grid mse rises
from about 0.012 to 0.09 in some 40 s without errors; our renegotiation
down waits until then (rms 0.3 of the half distance) and designs anew for
the timing as it is: over 180 s that is a renegotiation every 37 to 40 s,
2.1 s each, back at the same rate, and all the data through. In data
mode the analogue modem's tone A retrains. The echo canceller (the V.34 pump's,
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
| ours after 3 s of data, to drn 6 | 56000/38666, then 56000/30666 |
| noise up from 26 s (60 rms, `NET_NOISE_DIR=ab`) | ours on its own: 56000/24000 |

Fast parameter exchange (9.9/V.92): new rates without training, all in
the data mode modulation. Ours: Rf (R of + + - - in 12 symbols, the data
constellation's largest codes, 384 symbols) and Rf-bar (24) on a data
frame boundary, then, from cleared scrambler, signs and shaping filter,
SUVd, CPd, SUVd', Ed and B1d at the CPu we get. Its CPd keeps the
precoder and takes the rate from the data mode mse (with the margin of
the training's); `DSP_V92_FPE=<ms>[:<drn>]` starts one that far into data
mode (to that rate). The receiver finds the analogue modem's RM and RM'
among the decided classes (Table 25/V.92: the largest and the smallest,
in pairs), takes SUVu, CPu, SUVu' and E2u from the data decoder (its
descrambler and sign from zero again), then FB1u, 48 frames at the old
parameters, and B1u at the new CPd: the front end and the delayed
decisions switch at that symbol. To the original library ours takes
0.5 s (a renegotiation about 2): B1d without errors, B1u with a few in
its first two frames, as after a renegotiation. Our renegotiations go
up by fast parameter exchange (the same line with less noise), down by
renegotiation (train again).

With the prefilter the original library does three things differently:

- it starts its precoder and prefilter anew at SUVu (and at B1u). Its
  long precoder makes RM' unreadable, so SUVu is taken by the time,
  384T of RM and 24T of RM' after RM's start (9.9.2.2.2/V.92). The
  receiver rebuilds the line signal before it from its decisions (the
  precoder's output, through the prefilter) and takes it away as it does
  TRN2u's before B1u;
- once it has our CPd it sends SUVu', E2u and FB1u on the new points,
  with the old moduli, and the rest of the CPd from B1u on. We see it in
  the grid mse on the new points against the old, and slice on the new
  ones from there;
- so the new points have to suit the old moduli: as many, no further
  apart (else its signal goes beyond its power and some classes of the
  trellis frame's 4th symbol have no point). With the prefilter a fast
  parameter exchange never goes down, then (a renegotiation does): up
  when the margin allows the next rate, or at the rate it has.
  `interop_v92_fpe_prefilter` asks for 30666 and gets the exchange at
  38666, through to B1u and on with the data.

The original library answers ours but
starts none (its `V92Modulator::initiateFPE` is only called on our Rf),
so our answer to RM is not tested.

| Test | Result |
|---|---|
| `interop_v92_fpe`: ours after 3 s, to drn 6 | 56000/30666, data without errors |
| ours, rate from the mse (u-law) | 36000 or 37333 |
| ours to drn 6, A-law | 56000/30666 |
| noise upstream from 26 s to 40 s (60 rms) | renegotiation down to 24000; a minute later, fast parameter exchange up to 33333 |

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
and a high pass) it trains at every delay; the Agere tests use that,
with 0.45 of a sample. Not 0.5: with no clock offset and no noise, our
sample instants then fall exactly between the Agere's samples, and at
some alignments of the call to its codec's blocks (`AGERE_SYNC_PHASE`
96 and 288 of 4800) it judges TRN2d badly and takes 33333 bit/s
downstream instead of 53333. At 0.45 and 0.55 every alignment gets 53333
or 54666, at 0.3 52000; the signal is the same, so this is the Agere
receiver's tie, not ours (`agere_v90_phase96` keeps that alignment).

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

`dmodem_test` puts a G.711 network between the two modems
(`ctest -R v91`):

| Network | Codes per interval | Rate |
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

`fax_test`'s `T30Pair` cases put two of them back to back, at each rate,
with ECM, and with ECM on a noisy line (at 19 dB ECM sends a few frames
again and the page comes right; without it two thousand rows come
damaged); `spandsp_test`'s `T30Spandsp` cases (label `spandsp`) run one
against spandsp's fax machine both ways, with and without ECM, and with
line hits.

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
                                            # (sm56, pctel, hsf, spanpeer: the other modems)

All the tests are GoogleTest programs (the system's libgtest-dev, or
fetched while configuring); ctest runs each of their cases as a test of
its own, named `<suite>/<Test>.<case>/<parameters>`, e.g.
`Line/PumpLoopback.DataBothWays/v34_echoes`, or for the calls
`<label>.<call>`, e.g. `agere.agere_v92_qc`. Run one by itself with
`ctest -R v34_echoes`, or the program with `--gtest_filter`:

| Program | What |
|---|---|
| `pump_test` | the data pumps back to back (`loopback.cpp`): each modulation on clean and noisy lines, 8000 Hz, clock offsets, gains, echoes; the answering automode |
| `dmodem_test` | libdmodem: pairs of its modems, retrains and renegotiations in the data, V.91 across G.711 networks, `dmodem.hpp` |
| `dmodem_fax_test` | libdmodem's fax: a data modem answering hands the call to a fax receiver |
| `fax_test` | the class 1 modulations, T.4/TIFF (also against libtiff's `tiffcp`), two T.30 machines back to back |
| `spandsp_test` | spandsp's V.17 transmitter and T.4 encoder into ours, our T.30 against spandsp's |
| `../../link/link_test`, `../../core/test/core_test`, `core_equiv` | the data link layer, the core, the new core against the old one |
| `interop_test`, `agere_test`, `sm56_test`, `pctel_test`, `hsf_test`, `spanpeer_test` (and `interop_serial_test`, `agere_serial_test`: the calls that run alone) | calls in real time between this modem and the others (`interop_test.cpp`: one table of the calls, each program a label's) |

The interop and agere calls run in real time and mostly wait, so they run
side by side (`-j`); each writes its modem logs to
`build/dsp/test/logs/<test>/`.

The test programs are in `build/dsp/test/`. The calls go through
`interop_call.cpp`; `interop_call` makes one by hand:

    interop_call <caller> <answerer> ["ATX3;AT+MS=122"]
    interop_call --fax <sender> <receiver> [mod] [AT setup]

with the call's settings in the environment (`interop_call.h`: `NET`,
`STREAM`, `CALLS`, `EXPECT_B`, ...; `NETSIM` for netsim and `LOG_DIR`
for the logs, by default the current directory).

`dsp_loopback <dp> [snr dB] [seconds] [srate] [line gain] [clock ppm]
[echo dB]` runs a caller and an answerer through a simulated line with
delay, a low-pass filter (`FLAT=1` leaves it out), white noise and an
optional sample clock offset. It then checks the bit error rate in both
directions. `DUMP=<prefix>` records what each side hears. It is the
simulation of `pump_test`, for trying a line from the command line.

V.34 between two open modems (`dsp_loopback 8 ...`, with the low-pass
filter, no bit errors): 33600 bit/s at 40 dB (also with ±80 ppm clock
offset, or with near and far echo 6 and 10 dB below our signal), 31200
at 38 dB, 28800 at 35 dB, 26400 at 33 dB, 24000 at 30 dB, 21600 at
28 dB, 16800 at 25 dB, 14400 at 22 dB. `[echo dB] [far echo dB]` add the echoes;
`FAR_DELAY=<samples>` delays the far one (200 ms: still 31200 bit/s).

`interop` connects two `modem_half` processes with pipes. One process is
linked with `test/smartlink/dsplibs.o` on slmodemd's old C core
(`test/legacy/`: the original library calls into it), the other with this code on the modem
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

`fax_interop` does the same with `AT+FCLASS=1`. `interop_call --fax` plays
the T.30 side of both fax machines: CSI/DIS, DCS, TCF, CFR, one page (for
V.17 with the short train), EOP, MCF and DCN. The modems run in real time
so that the driver keeps up with the T.30 timers. Results at the time of
writing:

| Modulation | original sender → open receiver | open sender → original receiver |
|---|---|---|
| V.27ter 2400, 4800 | pass | pass |
| V.29 7200, 9600 | pass | pass |
| V.17 7200, 9600, 12000, 14400 (long train for TCF, short for the page) | pass | pass |

The original library makes a few bit errors with some of its own signals,
so `interop_call --fax` accepts a page with a bit error rate below 1e-3 (a fax
page without ECM survives that). Its V.27ter training also contains stray
180 degree phase flips, which the open receiver follows.

`fax_test`'s `FaxModulation` cases send random data through each fax
modulation over a simulated line. The V.17 short train codes run a long
train first, so the receiver has an equalizer to start from.

### Against the Agere soft modem

`half_agere` runs the Agere Systems soft modem library
(`test/agere/agrsm_core.o` of agrsm 2.1.40, i386) in user space on an emulated Intel ICH modem
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

### Against spandsp's data modems

`half_spandsp` is a modem around spandsp's pumps (libspandsp 0.0.6, the
one Asterisk and FreeSWITCH use): V.21, V.23, Bell 103 and V.22/V.22bis,
asynchronous 8N1, a few AT commands, and V.8 by default. spandsp has no
modem of its own: the `data_modems` of its later versions is a sketch,
and their V.32bis (V.17's pumps with V.32's scramblers, no handshake) and
V.34 (no data receiver yet) cannot connect. `ctest -L spanpeer -j8`
(not in CI's `spandsp` label) calls it from and to the open modem at
each modulation it has, with `AT+MS=` forced on both sides:

| `AT+MS=` | spandsp caller → open answerer | open caller → spandsp answerer |
|---|---|---|
| 122 (V.22bis) | CONNECT 2400 | CONNECT 2400 (after our LAPM attempt, about 20 s) |
| 22 (V.22) | CONNECT 1200 | CONNECT 1200 (the same) |
| 23 (V.23) | CONNECT 1200/75 | CONNECT 1200/75 |
| 21 (V.21), 103 (Bell 103) | CONNECT 300 | CONNECT 300 |

It has no error correction, which found two faults in ours:

- Our V.42 detection phase counted T400 (750 ms) in bits at a nominal
  28800 bit/s: 9 s at 2400 bit/s, 18 s at 1200, 72 s at 300. It is at
  the pump's rate now.
- Our caller goes to LAPM without the detection phase (V.42 allows it),
  and when the answerer has no V.42 it sent XID N400 times and then
  waited for nothing. It now goes on in async (`LapmCallerAsyncRemote`).

V.8 between the two does not connect yet: ours offers only V.34 and
V.32bis in its CM, and as the answerer it goes to V.32bis automode on a
CM without V.34 instead of sending a JM for the modulation both have.
spandsp, for its part, sends a V.92 octet before its CM (a QC1a with all
fields 0, which ours answers) even with V.92 off, and 0.0.6 sends the
CM's modulations back in its JM unless the application trims them
(`half_spandsp` does).

### Against the Motorola SM56 soft modem

`half_sm56` runs Motorola's SM56 soft modem (`sm56.ko` of sm56-06.09.05,
built for Fedora Core 3's kernel 2.6.9: controller and data pumps, V.92)
in user space. The module is linked in as it is, its kernel imports
renamed `k_<name>` (`sm56_obj.cmake`) and provided by the harness, which
emulates an Intel ICH modem controller with a Motorola "MOTf" codec on
trapped port I/O, patches the module's few `current` reads, and resamples
between the line's 8000 Hz and the codec's rate (7200 Hz idle, 9600 Hz
for V.90). The module is `test/sm56/sm56.ko` as Motorola shipped it (sha256
`a757fd85...`; that build only, the patched places are checked); the
build renames its imports into `sm56k.o`. `ctest -L sm56 -j8` runs the
tests. `SM56_DEBUG=1` prints its `printk`s.

Its interrupt and deferred routine save the FPU (`fnsave`) to
`.rodata.cst4` + 0x640 and + 0x644, which runs past that section into
whatever the linker puts after it. With the module linked first, that is
its own constants, and its V.90 interpolation then reads far outside its
buffers. The harness points those four instructions at buffers of its own.

| SM56 `AT+MS=` / ours | SM56 caller → open answerer | open caller → SM56 answerer |
|---|---|---|
| V34 / 34 | CONNECT 33600/LAPM (we receive at 24000) | CONNECT 33600 / 24000 |
| V32B,0 / 132 | CONNECT 14400 | CONNECT 14400 |
| V92 (its automode) / 122; V22B,0 / 122 | CONNECT 2400/LAPM | CONNECT 2400 |
| V22,0 / 22, V21,0 / 21 | CONNECT 1200, 300 | the same |
| V90 / 90 (ours as the digital modem), mu-law and A-law | CONNECT 50666/31200 | - |

Below V.34 its `+MS` wants automode off (`AT+MS=V22` alone is an
error). Forced to V.22bis that way (`AT+MS=V22B,0`), it does not connect
as the caller to a V.22bis answerer that sends the answer tone (ours, the
Agere modem's): its unscrambled ones detector starts during the 2100 Hz
tone and takes it for them, and it sends S1 456 ms after the tone ends.
With the 75 ms silence of V.22bis that is while it already trains on the
answerer's unscrambled ones, so it misses the answerer's S1 and goes on at
1200 bit/s, sending S1 again every 2 s. With 600 ms of silence, or none
(the original library answers without the tone), it connects at 2400. In
its automode, its default, it calls our V.22bis answerer at 2400, so that
is the test.

The harness adds noise to the codec's input (`SM56_NOISE`, rms 5, about
-76 dB of full scale): on exact zeros the module's frequency estimate
stays at its initial 1800 Hz, which its answering automode takes for a
V.32 caller's AA. It then leaves ANSam within 40 ms for V.32's AC, and
a V.34 caller never hears an answer tone. Rms 3 is not enough; rms 10
costs V.90 a step (45333).

Its V.92: QC1a in V.8 on every call, and on the second call (`CALLS=2`)
quick connect with short phase 2. Its upstream stays V.34: the module has
PCM upstream (`UAPCMP3_*`), but chooses it only in quick connect's INFO1a
(`SC_QC_Phase2_Set_Info_1A`), from INFO1d's PCM upstream bit, which its
full phase 2 (`SC_Phase2_Create_Tone_A_Set_1A`) clears after reading it.
Forced on, that path crashes the module (`SC_Modulate`).

Modem-on-hold: its own request (`AT+PMHR`) we grant; it goes on hold
and comes back with quick connect. Ours it takes for a retrain, as the
Agere modem does. It answers MHreq only where its phase 2 begins, in place
of INFO0d (`SC_Phase2_Detect_Info_0` looks for both). An MHreq sent at its
retrain reversal does get an answer: MHnack with `+PMHT=0` (its default),
and MHcda then clears the call down. With `+PMHT` set, it waits for an
accept from its platform (the handset, `MO_HoldStatePlatformInput`). After
MHfrr it does not reconnect with V.8, and expects something else after
silence. Up to that point a remote that knows nothing of MH behaves the
same way, and an MHreq at the reversal stalls the original library and the
Agere modem (they take it for our reversal), so we keep the
retrain-follow.

### Against the PCTel HSP soft modem

`half_pctel` runs PCTel's HSP soft modem (pctel-0.9.7-9, "HSP56
MicroModem ... K2.4.7.V92.001 (03/26/2003)": the Intel AMR build of its
data pumps, `lib/amr/dsp.a`, and its controller, `lib/control.a`) in user
space. Unlike the Agere and SM56 modems they need nothing of a kernel: the
two objects (`test/pctel/dsp.o` and `control.o`, PCTel's files renamed)
import only their hardware layer and the virtual UART's registers and
queues, which the harness provides in C after the driver's `i8xxhal.s`
and `vuart.s`. Its interrupt runs `dspMain()` and `modem_main()` once per
block of codec samples: the pumps take the block with `RdASIC` and give
theirs with `WrASIC`, at the rate and block size they set (7200 Hz and 24
samples idle). `GlobalTimer` is the blocks' time in ms. `ctest -L pctel
-j8` runs the tests. `PCTEL_DEBUG=1` prints what the HAL is asked and the
controller's choice of pump (`sharedRam` + 0x1c, + 0x20) against the
pump running (`curModem`).

| PCTel `AT+MS=` / ours | PCTel caller → open answerer | open caller → PCTel answerer |
|---|---|---|
| V34 / 34 | CONNECT (we receive at 24000) | CONNECT 24000 |
| V32B,0 / 132 | CONNECT 14400 | CONNECT 14400 |
| (its automode) / 122 | - | CONNECT 2400 |

It reports the DTE's rate (115200) in its CONNECT. Against itself and
the SM56 modem it connects at V.34 both ways (33600 with SM56).

Its pumps run its automode whatever `+MS` says: `AT+MS=V21,0` is kept
(`AT+MS?` gives it back), but the controller gives the pumps the same
modulation for it as for `V34`, and after the answer tone they go on as
for V.8 or V.32 (as the caller its V.32 AA, 1800 Hz). So it does not
connect to our forced V.21, V.22 or V.23 answerer, and as the answerer
it waits after its answer tone, as V.32's does, for a caller that waits
for its carrier. Calling our V.22bis answerer, its 2400 bit/s connects at
once on its side, while ours takes 10 s over it (by then it has given up
V.42). These are open.

V.34 with it as the answerer failed at first: it retrained phase 2 instead
of answering our INFO1c with INFO1a, every time. Its INFO decoder (state
in `g34`) never found the frame's start: we went from L2 straight into
INFO1c, so the frame's first bit was a phase change from our last
control channel symbol before L1. We now send two symbols of B first, a
phase reference (the Agere and SM56 modems and the original library did
not mind either way).

V.90 (it as the analogue modem) gets through phase 3 now, but in phase 4
it stops its CPt about 70 ms after our Ri ends and retrains with tone A;
the call then connects at V.34 33600 (data both ways). Holding Ri longer
after CPt does not change it. Open, and so is V.92.

### Against the Conexant HSF soft modem

`half_hsf` runs Conexant's HSF soft modem (Linuxant's hsfmodem
7.80.02.06oem) in user space: its engine (`hsfengine`, the controller and
the data pumps), its AC'97 ICH codec module (`hsfmc97ich`) and its soft
DAA module (`hsfsoar`), the driver's i386 objects as they are in
`test/hsf/`, with the modem's `.inf` and country file for its NVM. CMake
renames each object's `.ctors` and `.gnu.linkonce.*` sections for the
object (`hsf_obj.cmake`): each has the same C++ names and the linker
would keep only one. The harness provides the engine's OS layer
(`cnxthsf_7800206oem_Os*`: memory, timers on the line's clock, I/O ports,
the interrupt) and its NVM (the `HKR` lines of the `.inf` and the
country's of `hsf.cty`), and models what the modules drive: an ICH's
AC'97 bus master (PCM in and out, pausing and resuming at the position it
was at), a secondary MC'97 codec (Conexant, 0x4358/0x5429) at 15999 Hz
and the SmartDAA behind its register 0x7a. The modem's `ComCtrl` is the
DTE, on a pty. The engine reads the TSC: the harness traps `rdtsc`
(`PR_SET_TSC`) and gives the line's time at 1000 MHz. `ctest -L hsf -j8`
runs the tests; `HSF_DEBUG=1` prints the modem's own messages and
the port accesses the harness does not know, `HSF_DEBUG=2` every access.

| HSF `AT+MS=` / ours | HSF caller → open answerer | open caller → HSF answerer |
|---|---|---|
| V34 / 34 | CONNECT | CONNECT |
| V32B,0 / 132 | - | CONNECT |
| V22B / 122 | CONNECT | CONNECT |
| V21,0 / 21 | CONNECT | - |

V.90 with it as the analogue modem connects at 52000/33600 over µ-law
and A-law. It failed at first: we waited for its CJ while it went on with
INFO0a. It cuts its CM short for CJ and drops its carrier right after the
last data bit of CJ's third zero octet, so the octet has no stop bit and
our V.8 receiver counted only two. Two zero octets and the start bit and
eight zero bits of a third now count as CJ (CM and JM never have two zero
octets in a row).

The ICH module ran off its ring buffer every 3.27 s at first: a word of
its device (offset 0x280, 2 once the engine runs) makes it add 8 samples
to each position it reads past 700, as for a codec that runs fast; the
harness clears it before each interrupt (`HSF_KEEP8=1` keeps it). Open:
our V.21 caller (it answers, but we never connect), its V.32bis caller,
and V.92: it does quick connect and asks for PCM upstream, which fails.

`MODEM_REC=<prefix>` makes `modem_half` record its line input and output
(`<prefix>.in`, `<prefix>.out`), and `DBG=<level>` sets the debug level the
interop calls run the modems with. `WAV=<file>` makes `interop_call` save
the call as audio: a stereo 16 bit WAV at 8000 Hz of the line where it
crosses the simulated network (after G.711; A, the caller, to B left, B to
A right), from the first dial to the end of the data (or of the call that
failed). It needs `NET` (netsim writes it, `NET_WAV`); with `CALLS` the
calls in between are in it as well. For example:

    NETSIM=build/dsp/test/netsim NET=9600:8000:ulaw BLOB_V92FULL=1 \
      WAV=call.wav build/dsp/test/interop_call build/dsp/test/half_blob \
      build/dsp/test/half_open8 "ATX3;AT+MS=92"

`PIPE_SYNC=<samples>` (`half_open`, `half_blob`), `AGERE_SYNC=<samples>`
(`half_agere`), `SM56_SYNC=<samples>` (`half_sm56`),
`SPANDSP_SYNC=<samples>` (`half_spandsp`), `PCTEL_SYNC=<samples>`
(`half_pctel`) and `HSF_SYNC=<samples>` (`half_hsf`) make a call repeat exactly from run to run: the modems take
the DTE's commands only every that many line samples (data, once
connected, at once), run their timers
between half frames and keep time by the line; without them, where the
commands meet the line depends on the scheduler. `AGERE_SYNC_PHASE` and
`BLOB_SYNC_PHASE` move one side within the period, to try the alignments
one by one.
