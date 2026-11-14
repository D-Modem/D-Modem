# Open data pumps for D-Modem: technical details

This page gives the technical details for [The data pumps: D-Modem's signal processing](README.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

## What `dsp/` is

`dsp/` is a new C++ implementation of the interface that the binary-only
DSP library of Smart Link, `dsplibs.o`, gives to the slmodemd modem core
of Smart Link. It exports the same entry points. Thus, a modem core that
was written for that library builds with this code with no changes.

dmodemd and libdmodem use only this code. We keep the original library,
`dsp/test/smartlink/dsplibs.o`, only as a partner for the interop tests.

## The interface to the modem core

The interface is a set of headers in `dsp/`, with C linkage, as in the
original library:

| Header | What it holds |
|---|---|
| `modem_dp.h` | Pump registration (`modem_dp_register`) and the bit exchange with the core (`modem_get_bits`, `modem_put_bits`) |
| `modem_param.h` | The parameters of the core, which the pumps read and set by number |
| `dsplibs.h` | The other entry points of the library, and the items that the core gives to it: tty input and output, debug output, memory |
| `modem_defs.h` | Types and constants, which include the S-register numbers and the data pump ids |

The pumps hold a `struct modem *` only as an opaque handle. They include
none of the headers of the core. `dsp/` compiles with only itself and the
shared `include/` directory (for `dm_types.h`) on the include path.

Parameters 1 to 62 in `modem_param.h` are the parameters of the original
library, with its numbers and names. Parameters 63 and higher are
additions of D-Modem, which the original library never asks for. They
include `MDMPRM_MODE` (data, fax or voice), `MDMPRM_STATS` (the
statistics of the call), `MDMPRM_VCVM`, `MDMPRM_TONES` (the tones of the
country), `MDMPRM_LINK_REQ` (a retrain or renegotiation that the DTE
asked for) and `MDMPRM_TEXT` (the text telephone settings).

### Entry points

| Entry point | What it does here |
|---|---|
| `prop_dp_init` / `prop_dp_exit` | Register and deregister all of the data pumps in the list below |
| `dcr_create`, `dcr_process`, `dcr_reset`, `dcr_delete` | DC removal, with the same algorithm and constants as the original: skip the first 0.6 s, average the next 1 s for the first estimate, then refresh it each 2 s with 0.9/0.1 smoothing |
| `dp_runtime_create`, `dp_runtime_delete` (and `dp_runtime_set_*`) | State that the pumps of one call share. Examples: the answer tone that call progress heard, the G.711 law of the network, an analogue line, the V.110 rate |
| `CID_*` | Caller ID. FSK at 1200 bit/s, Bell 202 (1200/2200 Hz, North America) or V.23 (1300/2100 Hz, Europe and the UK), with SDMF and MDMF messages and their checksums. Also DTMF caller ID (ETSI TS 101 109 and the countries that use it). The result is formatted fields (`AT+VCID=1`) or raw hex |
| `VOICE_*` | IS-101 type voice: samples to and from the DTE in the format and rate of `+VSM`, with `<DLE>` shielding, `+VTS` tones and DTMF. The formats (`voice_codec.h`) are 8-bit signed and unsigned linear, 16-bit linear and IMA ADPCM at 7200, 8000 or 11025 samples/s, and G.711 mu-law and A-law at 8000 |
| `FAX_*`, `FAX_class1_command` | T.31 class 1 fax (`AT+FCLASS=1`): V.21 HDLC, V.27ter, V.29 and V.17 (refer to [Fax](fax-details.md#fax-class-1)) |
| `FAX2_*` | T.32 class 2 and 2.0 fax (`AT+FCLASS=2`, `2.0`), with T.30 in the modem (`fax_class2.cpp`, part of the T.30 library; refer to [Fax](fax-details.md#fax-class-2-and-20)) |

There is no ring detector entry point (the `RD_*` of the original
library). `RingDetect` in `dsp.h` detects the ring signal that a modem
codec hears on hook.

## Data pumps

Each pump registers under a data pump id (a `DP_ID` in `modem_defs.h`).
The DTE selects one with `AT+MS=` and the number or a name.

| DP id (`AT+MS=`) | Modulation | Notes |
|---|---|---|
| `DP_CALLPROG` (2) | Call progress | Operates on the calling side before the modulation starts. Dial tone detection (`X2`/`X4`), DTMF or pulse dialling, the `W` of the dial string (wait for a dial tone), `@` (wait for quiet after the ring signal) and `;` (`DPSTAT_DIALED`: the line is held for more digits), the V.25 calling tone, the busy cadence, the answer tone (2100 Hz, 2225 Hz, or the unscrambled ones of V.22), and the no-answer timeout |
| 21 | ITU V.21, 300 bit/s FSK | |
| 23 | ITU V.23, 1200/75 bit/s FSK | The answering side sends the 1200 bit/s channel |
| 103 | Bell 103, 300 bit/s FSK | |
| `BAUDOT` (45) | 5-bit (Baudot) text telephone: TTY (45.45 bit/s, 1400/1800 Hz), telex, RTTY | `dp_text.cpp`. Half duplex, with its own character framing (5 bits, 1.5 stop bits, the shift codes) and conversion to and from ASCII on the asynchronous link of the core. The rate, tones and figures set come from `AT+MBAUDOT` (`struct dp_text`, `MDMPRM_TEXT`) |
| `EDT` (110) | EDT text telephone: V.21 channel 1 at 110 bit/s, 7 bits, even parity | `dp_text.cpp`, half duplex |
| `DTMF` (118) | DTMF text telephone (Annex B/V.18) | `dp_text.cpp`. Half duplex: each character is one to four DTMF digits (75 ms on, 55 ms off). The receiver does not listen for 300 ms after it |
| `V21TXT` (121), `B103TXT` (203), `V23TXT` (123) | The V.21 (Annex F), Bell 103 (Annex D) and V.23 Videotex (Annex E: Minitel, Prestel) text telephones | `dp_text.cpp`. Duplex FSK on a continuous carrier, 7 bits, even parity sent and ignored when received. The calling side sends in the low channel (the 75 bit/s channel of V.23). The answering side sends in the high channel |
| `CONTACTID` (5) | Contact ID (SIA DC-05) alarm reports | `dp_contactid.cpp`. The answering side is the alarm receiver and the calling side is the panel. DTMF messages of 16 digits with their checksum, the 1400/2300 Hz handshake and 1400 Hz kissoff of the receiver, half duplex. Each message is one line to or from the asynchronous link of the core. Call progress gives control to it immediately when dialling ends |
| `SMS` (12) | Fixed-line SMS (ETSI ES 201 912, protocol 1) | `dp_sms.cpp`, with the coding of the messages in `sms_pdu.cpp`. The 1200 bit/s channel of V.23 (1300/2100 Hz) in the two directions, half duplex. The answering side receives. It sends SMS_EST, then SMS_ACK for each SMS_DATA. Each message goes to the DTE as `+CMT:` (a delivery) or `+SUBMIT:` (a submission) and its text. The calling side sends. Each line that the DTE types becomes an SMS-SUBMIT, an SMS-DELIVER or a raw TPDU. SMS_REL ends the call when the DTE has no more data for 3 s. Text is in the 7-bit alphabet of GSM, or in UCS2 when it has a character that this alphabet does not have. Call progress gives control to it immediately when dialling ends |
| `V18` (18) | V.18 text telephone automoding | `dp_text.cpp`. When it calls, it sends CI and XCI in the cycle of V.18. When it answers, it listens and then probes a silent remote modem. It finds a 5-bit, DTMF, EDT, V.21, V.23 or Bell 103 remote modem and operates that mode. With another V.18 modem, it exchanges the answer tone and TXP and operates the own mode of V.18 (V.21, 8 bits, no parity). It does not use the CM of V.8 or V.8 bis. Call progress gives control to it immediately when dialling ends, because there is no answer tone to wait for |
| 22 | ITU V.22, 1200 bit/s DPSK | With S49, point of sale fast connect: a short answer tone and handshake (refer to [Point of sale](fastpos-details.md)) |
| 212 | Bell 212A, 1200 bit/s DPSK | 2225 Hz answer tone, scrambled ones handshake; fast connect as V.22 |
| 122 | ITU V.22bis, 2400 bit/s 16-QAM | The full S1 handshake; falls back to V.22; fast connect as V.22 |
| 126 | ITU V.26 ter, 2400 and 1200 bit/s DPSK, duplex on the switched network | `dp_v26.cpp`. 1200 baud on an 1800 Hz carrier in the two directions with an echo canceller; the rate patterns and the three start-up sequences of 6.3; no retrain (refer to [V.26 ter](v26-details.md)). A number of D-Modem |
| 32 | ITU V.32, 4800 and 9600 bit/s | Uncoded 4800, trellis coded 9600, with an echo canceller |
| 132 | ITU V.32bis, 4800 to 14400 bit/s | The pump selects the rate from the measured training quality. It retrains automatically when it loses its equalization. It falls back to V.22bis when the remote modem does not do V.32 |
| 168 | USRobotics HST, 4800 to 16800 bit/s one way, 300 bit/s the other | `dp_hst.cpp`. The forward channel of the Courier (2400 baud, 1800 Hz, the constellations and trellis code of V.32bis, its own 256 points at 16800) and its 350 Hz 8-PSK back channel. The start-up and rate request are designs of D-Modem (refer to [HST](hst-details.md)). A number of D-Modem |
| 128 | Rockwell's V.FC, 14400 to 28800 bit/s | `vfc_data.cpp`, in the pump of V.34. The start-up of V.34, then the data mode of V.FC (the frames, points and 8 or 32 state 2D trellis codes of the Courier) when the two MPs ask for it in bit 34. If not, the data mode of V.34 (refer to [V.FC](vfc-details.md)). A number of D-Modem |
| 29 | ITU V.29, 4800 to 9600 bit/s, 4-wire leased line | `dp_4wire.cpp`. The fax modulators of `fax_modems.cpp` operate as a duplex pump on a continuous carrier (`AT&L2`): the synchronizing signal of each end, parallel receivers that find the rate of the far end, resynchronizing and fallback (refer to [leased lines](../leased.md)) |
| 229 | V.29 FastPOS, 9600 (7200, 4800) bit/s half duplex on the switched network | `dp_v29fp.cpp`. The point of sale modem of Hypercom. Call progress sends a steady 980 Hz calling tone, and the answerer sends a short 2225 Hz answer tone. Then bursts of the V.29 of fax (`fax_modems.cpp`), each with its training, go when the link of the side has data and the line is quiet (refer to [Point of sale](fastpos-details.md)). A number of D-Modem |
| 33 | ITU V.33, 12000 and 14400 bit/s, 4-wire leased line | The trellis coded modulation of V.17 with the rate signals of V.33, in `dp_4wire.cpp` |
| 27 | ITU V.27ter, 2400 and 4800 bit/s, 4-wire leased line | In `dp_4wire.cpp` |
| 226 | ITU V.26 bis, 2400 or 1200 bit/s DPSK one way, the 75 baud backward channel the other | `dp_v26bis.cpp`. The answering side sends the forward channel (the alternative B of V.26 at 2400, 90 and 270 degrees at 1200), unscrambled, after the answer tone and 1 s of the synchronizing signal. The calling side finds the rate from that signal and sends the 390/450 Hz backward channel (refer to [V.26 bis](v26-details.md#v26-bis)). A number of D-Modem |
| 26, 201 | ITU V.26 (alternative A) and Bell 201 (alternative B), 2400 bit/s DPSK, 4-wire leased line | In `dp_4wire.cpp`. The 2400 bit/s symbols of V.27ter with the data unscrambled (`FM_V26_A`, `FM_V26_B`) |
| 208 | Bell 208, 4800 bit/s 8-phase DPSK, 4-wire leased line | In `dp_4wire.cpp`. The 4800 bit/s signal of V.27ter, only at that rate |
| 202 | Bell 202, 1200 bit/s FSK both ways, 4-wire leased line | `dp_fsk.cpp`. 1200 Hz mark and 2200 Hz space. Each end sends its mark immediately on its own pair |
| `DP_RELAY` (151) | The modem relay of V.150.1: the host relays the data of the call as octets (the SIP line of dmodemd, SPRT), with no modulation | `dp_relay.cpp`. You do not select it with `AT+MS`. The core operates it on a call that the host relays (`Host::relay_line()`). It answers immediately, and calls after call progress hears the answer tone. It `CONNECT`s at the rates of the relay and moves start-stop characters (8N1) between the link of the core and the host (`struct dp_relay`, `MDMPRM_RELAY`) at those rates. It sends silence (refer to [Modem relay](../v150-details.md)) |
| `DP_V8BIS` (108) | The transactions of V.8 bis at the start of a data call, before V.8 | `dp_v8bis.cpp`, `v8bis.cpp`. You do not select it with `AT+MS`. With `AT+A8E=1,1,,1`, the core operates it first when it answers a call that starts with V.8. Call progress gives control to it when it hears an initiating signal (MRe, CRe ...). It agrees on the data mode (MRe, CRd, CL, MS, ACK(1)) and gives control to V.8 (refer to [V.8bis](v8bis-details.md)) |
| `CLEAR` (64) | A clear 64 kbit/s digital channel, for example the data call of an ISDN B-channel or a SIP call in CLEARMODE | `dp_clear.cpp`. No modulation: each octet of the line sends 8 bits in each direction, the first bit the most significant. The own start-up of D-Modem exchanges HDLC flags, or V.110 rate adaption (`AT+CBST`, `v110.h`) operates at a user rate of 300 to 56000 bit/s (refer to [ISDN](../isdn.md)) |
| `VCVM` (150) | V.cvm, data over mobile speech paths, 175 to 3500 bit/s | The proposed Recommendation in [docs/cellular](../cellular/v_cvm.tex). V.8 first (with V.22 and V.21 as the fallback), then probing, then a loaded comb or the parametric mode through AMR, AMR-WB or G.711 (refer to [V.cvm](vcvm.md)) |
| 91 | ITU V.91, 28000 to 62667 bit/s both ways | Two digital modems on an 8000 Hz G.711 line. V.8 first, then V.34 if V.91 does not start (refer to [V.91](v91.md#v91-digital-to-digital)). The ids 90 and 92 also offer it |
| `VWB` (200) | V.wb, D-Modem's wideband mode on a bare pair, up to 148656 bit/s each way | Discrete multitone (DMT) in frequency-division duplex at 48000, 96000 or 192000 Hz. It does not start below 48000 Hz. It is for the WB stick of pico-serial. The line operates as a leased line (no call progress; `AT&L1`). It has its own start-up, bit loading for each tone from the measured SNR, RS(255,239) and a loop-timed answerer (refer to [V.wb](vwb.md)) |
| 8, 34, 90, 92 | V.8 and ITU V.34, 2400 to 33600 bit/s; V.90 as the digital modem | V.8 negotiation, then V.34. When the other end offers no V.34, the pump gives control to the modulation on which V.8 agreed (V.32bis, V.22bis, V.23 or V.21). When the other end does not do V.8, the pump gives control to the V.32bis pump (with its V.22bis and V.22 fallback). With 90 or 92 on an 8000 Hz G.711 line, it is the digital modem of V.90, 28000 to 56000 bit/s downstream (refer to [V.90](v90-details.md)) |

Two test pumps from slmodemd are also registered, but have no `AT+MS`
code. You select them through S32, the register that holds the
modulation of `AT+MS`:

- `DP_DUMMY` (3, `ATS32=3`, `dp_dummy.cpp`, `DummyPump`): two of them
  find each other by a tone, then send one bit in each sample.
- `DP_SINUS` (127, `ATS32=127`, `dp_sinus.cpp`, `SinusPump`): a test
  tone.

### V.90, V.92 and V.91

V.90 is available in the two roles:

- **As the digital modem** (the server side, as on a G.711 VoIP line).
  It also has the quick connect of V.92 for V.92 analogue modems that
  call it (refer to [V.92](v92-details.md#v92-quick-connect)) and the PCM
  upstream of V.92 (refer to [PCM upstream](v92-details.md#v92-pcm-upstream)).
- **As the analogue modem**, on an analogue line at 9600 Hz, with id 90
  or 92 (92 is the default). It gets 56000 bit/s down and 33600 up from a
  V.90 digital modem, and V.34 or lower from all other modems, with its
  retrains and rate renegotiations. `DSP_V90A=0` removes it (refer to
  [V.90 analogue modem](v90a.md)). At 8000 Hz, it operates only with
  `DSP_V90A=1`.

V.92 as the analogue modem has PCM upstream (up to 48000 bit/s), quick
connect when it calls, and modem-on-hold. `DSP_V92A_UP=0` removes PCM
upstream, together with its renegotiations and the fast parameter
exchange of the digital modem.

K56flex operates on the V.34 pump at the two ends (`DP_K56`,
`dsp/v34_k56.h`):

- As the digital modem, when it answers with `AT+MS=56` on an 8000 Hz
  line, V.8bis (MRe, CRd, CL, MS, ACK(1)) comes before V.8. Phases 3 and
  4 and the data mode are those of K56flex when the caller has it. If
  not, they are those of V.90.
- As the analogue modem, when it calls with `AT+MS=56` on a 9600 Hz line,
  the call progress accepts the MRe of the digital modem as its answer
  (`ANSTONE_V8BIS`). The pump answers it with CRd and MS, and the PCM
  receiver is `K56Rx` (`dsp/k56flex_rx.cpp`). When the answer is a plain
  ANSam, it uses V.90.

Refer to [K56flex](k56flex-details.md).

Between two digital modems, V.91 (refer to
[V.91](v91.md#v91-digital-to-digital)) replaces V.90.

### Answering

1. The core starts the answering pump S92 seconds (default 2) after it
   goes off hook. This is the silent interval of V.25, 1.8 to 2.5 s
   (4.3/V.25).
2. The pump stays silent for 0.2 s more, the minimum of V.8 (8.2/V.8).
3. Then it sends its answer tone (ANS, ANSam, or the 2225 Hz of Bell),
   2.2 s after it goes off hook. The pumps of the original library send
   theirs immediately after the same delay.

### Tables taken from the original library

The V.22/V.22bis constellation, quadrant coding and phase tables come
from the `SMCv22_*` and `DECv22_*` tables of the original library. The
V.32bis constellations come from its `VTBv32_*` tables.

### V.32 / V.32bis

`dp_v32.cpp` does the start-up sequence in the same way as the original
library. We found this sequence from recordings of that library:

1. The answer tone.
2. The AA/AC/CA/CC exchange that measures the round trip delay.
3. S, S-bar and TRN: 256 symbols of two points, then four-point symbols,
   from the fixed scrambler seeds of the original library.
4. The R1/R2/R3 rate words and E.

The receivers train an NLMS echo canceller while their own TRN goes out.
Then each side offers the rates that its training quality supports:

| SNR at training | Highest rate offered |
|---|---|
| 25 dB or more | 14400 |
| 22 dB | 12000 |
| 19 dB | 9600 |
| 16 dB | 7200 |
| below 16 dB | 4800 |

A V.32bis call retrains automatically when the error of its receiver
increases to 4 dB below the SNR at which the modem selected its rate
(7 dB for 4800).

### How the QAM receiver works

The generic QAM receiver (`QamRx`, `qam.cpp`) that V.32 and the fax
modulations use has five stages:

1. Down-conversion to complex baseband.
2. A root raised cosine matched filter, calculated directly at the T/2
   sampling instants (from a finely tabulated pulse, with no separate
   interpolator), then AGC.
3. A Gardner timing loop. Its frequency term has a limit of ±1000 ppm of
   clock offset between the two ends.
4. A T/2 spaced normalized LMS equalizer.
5. A decision-directed carrier PLL.

V.22 and V.22bis (`dp_v22.cpp`) have their own receiver with the same
stages. The differences are that it uses a fixed matched filter and then
a cubic interpolator that its Gardner loop controls, and that its AGC
operates on each sample. We tuned the interop tests with the other modems
on it.

The equalizer and the frequency term of the PLL train only on scrambled
signals. The unscrambled handshake sequences (ANS, unscrambled ones, S1)
are almost pure tones and would de-tune them.

The V.22bis receiver also distinguishes unscrambled ones from S1 by the
phase step from one symbol to the next, not by its decisions. S1 is two
points 90 degrees apart. While the carrier phase is near 45 degrees off,
the two points fall in the same quadrant, where the error of the PLL
averages out. The answerer has only the 100 ms of S1 of the caller in
which to find it. Over SIP, this problem caused the answerer to identify a
V.22bis caller (a caller that tried V.34 and V.32bis first) as a V.22
caller at 1200 bit/s.

## Code structure

All of the code here is in `namespace dsp`, except the C entry points of
`dsplibs.h`. These keep C linkage for the modem core.

### Data pumps

All data pumps derive from `Pump` (`pump.h`):

- `CallProgPump` (call progress), `FskPump` (V.21, V.23, Bell 103),
  `TextPump` (V.18 and its text modes), `ContactIdPump`, `V22Pump`, `V26Pump`, `V26bisPump`,
  `V32Pump`.
- `V34Pump`, which is also V.8, V.90, V.91 and V.92.
- `FourWirePump` (V.29, V.33, V.27ter), `ClearPump`, `VcvmPump`, and
  `VwbPump` (with `VwbModem`, `vwb.h`, under it).
- The test pumps `DummyPump` and `SinusPump`.

A pump has:

- A static `name`.
- `start()`, which replaces the old create function. It returns 0, or -1
  if the pump cannot operate.
- `process()`.
- As an option, `hangup()` and a destructor.

`PumpOps<T>::ops` is the table that is registered with the modem core
(`modem_dp_register`). The code allocates a pump with `new T()`. Thus,
its members start at zero, as they did when the C pumps were allocated
with `calloc`.

### `PumpBase`

`PumpBase<P, S>` (`pump.h`, CRTP on `Pump`) holds the items that the
modulation pumps share:

- The core, the sample rate and the side.
- A state of type `S`, with the samples counted in the state and in
  total.
- The time limit of the handshake from S7 (50 s if S7 is 0) and the
  carrier loss limit from S10 (1.4 s if S10 is 0).
- The carrier run, the connect report, and the change to a different
  pump.
- The settings of the tests from the environment.
- `pump_loop()`, a `process()` that operates sample by sample for a pump
  with a `sample()` method (FSK, the text telephones, Contact ID,
  fixed-line SMS, V.22, V.32 and the 4-wire pumps).

The statistics of the call for the `ATI11` of the core (`struct dp_stats`
in `modem_defs.h`, through `MDMPRM_STATS`) also go through `PumpBase`:

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

The pumps fill in these items:

- The pump of V.34 fills in the line and the coding at each block while
  it is connected. For a V.90, V.91 or V.92 direction, this is the 8000
  symbols/s of PCM and no carrier, together with the law of the network
  and the DIL of the analogue modem (its digital loss and robbed bits).
- V.32bis and V.22bis fill in the line and the offsets. V.22bis gives no
  clock offset, because its timing loop is not stable.
- The echo canceller of V.34 adds the near and far echo levels.
- V.34 (the point power of its receiver divided by its mean squared
  error) and V.32bis (from its mean squared error) give the SNR.

### Mixins

Mixins (CRTP, with the host as the template parameter) hold one part of
the functions of a host:

- `HdlcTx` / `HdlcRx` (`fax_hdlc.h`): the HDLC framing of class 1 fax and
  of the V.21 and ECM frames of T.30.
- `PcmFrames` (`v90.h`): the phase 4 frame sequencer of V.90 and of the
  PCM upstream of V.92.
- The groups of V34Pump in `dp_v34.cpp`: `V34V8`, `V34Qc`, `V34Info0`,
  `V34Probe`, `V34Cc`, `V34Retrain` and `V34Moh`. Each group has its own
  fields and methods. Some groups (V.8, quick connect, retrains and
  renegotiations, modem-on-hold) have their own states of `handshake()`.

### Building blocks

The building blocks are small classes of plain data with methods:

- Signal processing: `Osc`, `Biquad`, `Goertzel`, `PowerMeter`,
  `Resampler`, `FskMod`, `FskDemod`, `FskBitTx`, `CarrierDetect`,
  `Bell103Detect`, `DtmfGen`.
- QAM: `QamTx`, `QamRx`, `TrellisEnc`, `Viterbi`, `EchoCanceller`.
- V.34 and V.90: `V34Tx`, `V34Rx`, `V34DataTx`, `V34DataRx`,
  `V34Precoder`, `SyncFrames`, `V90Enc` and others.
- Fax: `FaxTx`, `FaxRx`, `Fax`, `T30`, `T4Page`, `T4Rx`, `TiffIn`,
  `TiffOut`.

They have no constructors. `init()` sets one up. When it owns memory,
`release()` frees it. Because they are plain data, they can be in a pump
that is created zeroed.

A transmitter that gets its symbols or bits from its host (`QamTx`,
`FskBitTx`) gets the source at `sample()`. The data bits of V.34, V.91
and V.92 go through `CoreBits`, the end of the bit stream in the modem
core.

### The other library objects

`DcRemover`, `CallerId` and `Voice` (`dsp_glue.cpp`) are classes behind
their C entry points (`dcr_*`, `CID_*`, `VOICE_*`).

### The pumps without a modulation

`dp_dummy.cpp` (`DummyPump`) and `dp_sinus.cpp` (`SinusPump`) are the
two test pumps of slmodemd (refer to [Data pumps](#data-pumps)). We wrote
them again on `Pump` and `PumpOps`, as the other pumps.

The conversion from C structs and functions to these classes kept the
output identical to the byte. All quick tests gave the same debug log
before and after.
