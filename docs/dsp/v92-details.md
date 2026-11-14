# V.92: technical details

This page gives the technical details for [V.92](v92.md), the plain-English overview. Read the overview first. This page keeps the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

This page is about V.92 on the side of the digital modem: quick connect, modem-on-hold, and PCM upstream with its rate renegotiation and fast parameter exchange. The start-up and modem-on-hold are in `dsp/dp_v34.cpp`. PCM upstream is `V92Up` in `dsp/v92.cpp`.

Terms that this page uses:

- The **digital modem** is our side (the server, on a G.711 network at 8000 Hz). The **analogue modem** is the client on the phone line.
- **INFO0d / INFO1d** are the phase 2 frames of the digital modem. **INFO0a / INFO1a** are the phase 2 frames of the analogue modem.
- **drn** is the upstream rate code: (drn + 17) × 8000 / 6 bit/s. Thus, drn 14 is 41333 bit/s.
- **mse** is the mean squared error of the receiver.

## Switching V.92 on and off

With `AT+MS=92` (the default) on an 8000 Hz line, the digital modem announces V.92 in INFO0d (bit 27) and sends the INFO1d of V.92. INFO1d offers PCM upstream (bit 70; refer to [PCM upstream](#v92-pcm-upstream)). Without the offer, the analogue modem asks for V.90 or V.34 upstream, and the call continues as V.90.

| Variable | Effect |
|---|---|
| `DSP_V92=0` | does not use V.92 (the call is V.90) |
| `DSP_V92_QC=0` | answers QC1a with V.8 instead of quick connect |
| `DSP_V92_SHORT2=0` | does not ask for short phase 2 |
| `DSP_V92_PCMUP=0` | does not offer PCM upstream |
| `DSP_V92_MOH=<code>` | the modem-on-hold time that we grant (overrides `AT+PMHT` and `AT+PMH`) |
| `DSP_V92_MOH_REQ=<ms>[:<s>]` | asks for modem-on-hold at that time into data mode (a test) |

The PCM upstream variables are in [PCM upstream](#v92-pcm-upstream).

## V.92 quick connect

### Short phase 1 (9.2.4/V.92), answering

The analogue modem starts with **QC1a** before its CM. QC1a is its frame on V.21(L), and it carries the Ucode of QTS and LAPM. We answer:

1. **QCA1d** on V.21(H) (with ANSpcm at -12 dBm0);
2. 75 ms of silence;
3. **QTS**, **QTS-bar** and **ANSpcm**, as PCM codes. Tables 7 to 10/V.92 come from the G.711 quantization of Table 6/V.92.

The **TONEq** (980 Hz) of the analogue modem stops this. We also wait for TONEq to stop. Then we send 75 ms of silence and continue to phase 2, as after V.8. Sd of phase 3 starts in data frame interval 0 of QTS (8.3.6/V.92).

If TONEq does not come in 2 s, the call continues with V.8.

The analogue modem can come to phase 2 after our INFO0d is complete (the original library does this). After its INFO0a arrives, INFO0d goes out again with the acknowledgement, as in the INFO0 recovery of phase 2 (refer to [V.8 and V.34](v34-details.md)).

### Catching QC1a and TONEq

QC1a comes two times, and the analogue modem continues with CM after it until it hears QCA1d. The first QC1a starts when the V.21 receiver starts to receive the carrier. Thus, its sync comes with fewer than its ten leading ones. When our QCA1d came only after the second QC1a, the Agere modem (at some line delays) continued to send CM.

If the analogue modem does not hear QCA1d, we answer its subsequent CM with JM (V.8, full phase 2).

TONEq is the mark of the low channel of V.21. It counts only after 128 ms of 980 Hz without the space of that channel (1180 Hz), and only while we send ANSpcm. Thus, the series of marks of CM do not look like TONEq.

### Which modems send QC1a

- The original library sends QC1a on all calls.
- The Agere modem (with `AT+PQC=0`) sends it on a second call to a number to which it connected before.

Then the two modems connect at V.90 (`smartlink_v92_qc`, `agere_v92_qc`).

### Short phase 2 (9.4/V.92)

When the two modems ask for short phase 2 (never in a retrain), there is no probing and no INFO1d:

- The digital modem reverses tone B when tone A is present and B has been on for 50 ms.
- It measures the round trip from the reversal of A. Then it waits for INFO1a.
- The upstream rate comes from the mse of the training in phase 3 instead of from the probing.

Bit 33 of INFO1a gives the carrier of the analogue modem (Table 19/V.92). We always receive on the low carrier, because the modems that set the bit send on the low carrier:

- the SM56 modem at 3200 baud: its signal has its centre at the same position as after a full phase 2, 90 Hz below the high carrier;
- the Agere modem at 3429 baud, which has only one carrier.

With quick connect and short phase 2, the second call of the Agere modem connects in 17.5 s instead of 22.5 s. It also asks for a shorter DIL.

## V.92 modem-on-hold

### When the analogue modem asks (9.10/V.92)

When call waiting interrupts the analogue modem, it can ask us to hold the call. It sends its tone A, as for a retrain, and then MH sequences instead of the reversal. An MH sequence (Table 32/V.92) is fill, frame sync, a 4 bit signal and 4 bits of information, and a CRC, on the modulation of INFO. We look for MH sequences while we retrain, and we answer:

- **MHreq** gets **MHack** with T1 (the time for which we hold the call), or **MHnack**:
  - `AT+PMHT=<code>` sets T1 as a code of Table 33/V.92 (1 = 10 s, ..., 5 = 1 min, the default, ..., 12 = 16 min, 13 = no limit).
  - `AT+PMHT=0` or `AT+PMH=1` makes us answer MHnack.
  - `DSP_V92_MOH=<code>` overrides the two settings (0 denies).
- After MHack, when the tone of the analogue modem has been present for 100 ms or silence for 2 s, the call is on hold. We send ANSam until T1 is complete, and then disconnect. If neither comes in 35 s, we disconnect.
- On hold, we listen for QC1a or CM. Each of them starts phase 1 again, with us as the answering modem, then phases 2 to 4. The data continues without a new CONNECT. The timers of V.42 count bits, thus they also wait.
- A CM without modulations, or a QC1a with UQTS 1111, clears the call.
- **MHclrd** gets **MHcda**, then we disconnect.
- **MHfrr** (a fast reconnect, also after our MHnack) gets ANSam and phase 1 immediately (ANSam for up to 10 s).
- **MHcda** after our MHnack: we disconnect. If neither MHcda nor MHfrr comes in 12 s after MHnack, we also disconnect.

The Agere modem asks for modem-on-hold with `AT+PMHR`. It answers `+PMHR:5` when we grant 1 min, and `+PMHR:0` when we deny it. It comes back with CM after `ATO`. If we deny the request, it asks for a fast reconnect. `agere_v92_moh_grant` and `agere_v92_moh_deny` do this during the data (`MIDCALL_A` of `interop_call`), and check all of it in the two directions.

### When we ask (9.10.2.1/V.92)

We ask for it with `AT+PMHR` in online command mode on a V.92 connection. `AT+PMHR=<s>` holds the remote for that time (1 to 960 s). Plain `AT+PMHR` holds it for 10 s. `DSP_V92_MOH_REQ=<ms>[:<s>]` asks at that time into data mode (a test).

We stop data mode as for a retrain, send tone B for 600 ms, then send MHreq again and again until the answer:

- **MHack:** we stay silent (the remote is on hold) for those seconds, or until 2 s before its T1. Then we take it back. The remote sends ANSam through its T1 (9.10.2.1/V.92). When its ANSam has been present for 200 ms (`MOH_BACK`), we run V.8 as the call modem (CM), phases 2 to 4, and the data continues. If no ANSam comes in 5 s, the remote is gone and we disconnect.
- **MHnack:** we send MHfrr (for 300 ms), then ANSam and phase 1. With `=0` seconds in `DSP_V92_MOH_REQ`, we send MHcda and disconnect instead.
- **The tone A of the remote reversed (or its INFO0a):** the remote thought that our tone was the tone of a retrain, which it can do (9.10.1.1/V.92). We follow it into phase 2 immediately. Tone B is long (600 ms, where 50 ms would be sufficient). Thus, this occurs before MHreq, because the remote would think that the phase steps of MHreq are our reversal.
- **Nothing in 2.5 s** after the tone: a retrain.

Neither the original library nor the Agere modem answers MHreq. The two modems retrain. The library frames our MH sequences, but as INFO0 in its retrain, with a bad CRC. It looks for MH only in its own requests. `smartlink_v92_moh_request` (with PCM upstream) and `agere_v92_moh_request` (V.90) ask for it during the data (`MIDCALL_B`), and make sure that the call continues.

### Testing MHack and MHnack with netsim

For the MHack and MHnack branches, netsim replaces an analogue modem that answers: `NET_MOH=ack[:<T1 code>]@<s>` or `NET_MOH=nack@<s>`. From s seconds into the line, when our tone B has been present for 50 ms, netsim disconnects the modem in the two directions and sends tone A, then MHack or MHnack, until we go silent (on hold) or send ANSam. Then it gives the line back.

These tests check the sequence in our log, and expect the call to stop (`EXPECT_DROP` of `interop_call`):

| Test | Setting | What it checks |
|---|---|---|
| `smartlink_v92_moh_request_ack` | `NET_MOH=ack:1@22` (T1 10 s), `AT+PMHR=5` | MHack, 5 s on hold, then we take the remote back; no ANSam comes, and we disconnect after 5 s |
| `smartlink_v92_moh_request_nack` | `NET_MOH=nack@22` | MHnack, 300 ms of MHfrr, ANSam; nothing comes back, and we disconnect after T1 (10 s) |

netsim disconnected the original library during a retrain, and the library hangs up. Thus, nothing comes back. The method to come back (CM or QC1a after our ANSam) is the same as after the hold of the analogue modem itself. `agere_v92_moh_grant` tests this.

## V.92 PCM upstream

INFO1d tells that the channel can carry PCM upstream (bit 70). An analogue modem that wants it answers with the INFO1a of Table 18/V.92. Then `V92Up` ([v92.cpp](../../dsp/v92.cpp)) runs phases 3 and 4 of V.92 and the data mode on the 8000 Hz samples of the network, as V.91 does. Our transmitter in this direction is the PCM transmitter of V.90.

| Variable | Effect |
|---|---|
| `DSP_V92_PCMUP=0` | does not offer PCM upstream |
| `DSP_V92_UP_DRN=<n>` | asks for (n + 17) × 8000 / 6 bit/s instead of the designed rate (any n up to 19) |
| `DSP_V92_PRECODE=0` | sends no precoder: decision feedback in the receiver, margin 40 |
| `DSP_V92_TRELLIS=0` | does not decode the trellis code; `=16` or `=32` selects that code (64 states by default) |
| `DSP_V92_MARGIN=<x>` | overrides the design margin (8 with precoding and trellis decoding, else 40) |
| `DSP_V92_PREFILTER=0` | no prefilter (only the precoder) |
| `DSP_V92_SLIP=0` | no fast parameter exchange for the timing slip of the analogue modem |
| `DSP_V92_RENEG=<ms>[:<drn>]` | our own renegotiation at that time into data mode (to that rate), one time |
| `DSP_V92_FPE=<ms>[:<drn>]` | our own fast parameter exchange at that time into data mode (to that rate), one time |

### The receiver in phase 3

The analogue modem sends PAM symbols at 8000 symbol/s, one for each sample. The receiver finds them as follows:

- **Ru and Su** have a period of 6 symbols (1333 Hz). A sliding DFT of 12 samples finds them, and their phase reversals between two of its windows. Su counts after 48 of its samples have arrived, and its phase comes from those samples. In V.92, Su continues for 144T before Su-bar, but the Su of the Broadcom modem continues for 96T.
- The receiver finds **TRN1u** by its correlation with the known scrambled sequence. The correlation is the impulse response of the channel. It gives the symbol alignment (and, for the second TRN1u, the data frame alignment).
- A **decision feedback equalizer** (32 feed forward and 16 feedback taps) is fitted to TRN1u by least squares. Then it follows the signal by its decisions.
- **Ja, CPt and E1u** are 2 point signals. **TRN2u, SUVu, CPu and E2u** are 4 point signals (differential sign, scrambled).
- **E1u** is a data frame of zeros that starts after the end of the last CPt. The fill of CPt to the end of its frame can be all zeros, together with the last bit of its CRC. The CPt of our own analogue modem showed this. Also, **E2u** starts after the last SUVu or CPu.

### What we send

1. Sd, TRN1d and Jd.
2. Jp, after the receiver has heard Su, Su-bar and Su. Su-bar takes half a symbol, and Jp asks for half a symbol more. Thus, the analogue modem goes back to the phase of the first TRN1u.
3. Jp' on the next Su-bar.
4. The DIL that it asked for (or SCR).
5. Ri on CPt, and Ri-bar on E1u.
6. TRN2d, SUVd, CPd (after SUVu), SUVd' (after CPu), Ed and B1d, in the constellations of CPt and CPu through the encoder of V.90.

### The precoder and the rate (CPd)

Our CPd makes the precoder of the analogue modem do what the decision feedback of the equalizer did: p1 = -b (16 taps, Q1.14).

- **A modulo precoder.** Of the points of a class, the precoder of the analogue modem selects the point that keeps its output smallest. Thus, its output stays in the spacing of the classes also where 1 / C(z) is not stable. Here it is not stable: the edge of the channel at 4000 Hz puts a root of C on the unit circle.
- **The receiver.** It keeps the feed forward taps (fitted again by least squares to the TRN2u decisions, with the feedback that the precoder takes) and the points themselves. There is no decision feedback, and thus no error propagation, and the points are in sequence for the trellis code. A Viterbi decoder (the 64 state code of V.34 on pairs of symbols, 6.4.4/V.92) decides them before their classes.
- **The remaining parts of CPd.** The same modulus M on all 12 data frame intervals; one constellation of points (2j + 1) × 16, as many as the precoder must have (M (1 + sum |b|) positive points); the prefilter z2(0) = 1; and G for an output of unit power.
- **The rate.** The fastest rate in the DIL descriptor of the analogue modem whose half distance between points is a minimum of sqrt(8) times the rms error of the fit.
- **Shrinking the feedback.** The feedback taps can be made smaller (b_j g^j) when this lets a faster rate fit in 128 points.

| At 36000 / 38667 bit/s (µ-law) | B1u | Data |
|---|---|---|
| precoding and trellis decoding | 0 / 0 errors | 0 / 15 HDLC errors in 10 s (64 states; 33 with 16) |
| precoding only | 0 errors at 33333 | 14 HDLC errors at 33333 |
| no precoding | fails at 33333 | |

### Falling back

The call retrains without the offer, to V.90 with V.34 upstream, in these conditions:

- the analogue modem retrains instead of sending INFO1a;
- our phase 3 or 4 fails, or does not get to data mode in 40 s; or
- B1u does not come through (more than 5 % of its bits incorrect, without its first four frames).

### Against Smart Link's library

The library of Smart Link is a "V.92 Lite" build. Its runtime clears the bit that would let it use PCM upstream (byte 2, bit 7 of the object of `dp_runtime_create`). With our offer, it retrains to V.34 upstream after INFO1d ("we got PCM upstream under V.92Lite").

`SMARTLINK_V92FULL=1` sets the bit in `half_smartlink` ([test/smartlink_glue.cpp](../../dsp/test/smartlink_glue.cpp)). Then the library does PCM upstream. It must have two things in our CPd:

- Filter sections of a multiple of 4 taps. Its FIR rounds the length down, and it crashes on 0.
- An even number of positive points, a minimum of 2M. On the fourth symbol of the trellis frame, it looks for the point of a class up to 2 (K + z M) + parity, with z up to (LC - 1 - 2K) / 2M rounded toward zero. That takes one point after the end of its table (it sends 0) when LC is odd, and more when LC is less than 2M.

| Test (`smartlink_v92_pcmup_*`) | Result |
|---|---|
| `ulaw`, `alaw` | V.92 connect, 56000 down, 38666 / 40000 up (prefilter); B1u and 10 s of data without errors |
| `noprefilter` (`DSP_V92_PREFILTER=0`) | 56000 down, 34000 to 36000 up |
| `calls` (two calls, quick connect; `NET_FLOOR=3`, as V.90's µ-law tests: see [V.90](v90.md)) | both V.92, 54666 down, 38666 up |
| `lite` (the library as it is) | it retrains after INFO1d, we leave the offer out: V.90 |
| `fallback` (44000 bit/s asked for, `DSP_V92_UP_DRN=16`) | B1u does not come through: retrain, V.90 (its modulus encoder fails above 41333: see below) |

`smartlink_v92_nopcmup` (`DSP_V92_PCMUP=0`) connects at V.90 with V.34 upstream.

### The 41333 bit/s ceiling

Neither the original library nor the HSF modem can send more than 41333 bit/s upstream, but the two offer all rates in Ja. From 42666 bit/s (drn 15, K = 64 bits for each data frame), B1u does not come through. The points are decided cleanly (the mse is as low as at 38666), but a third of the bits are incorrect, in bursts. There are not more incorrect bits at 48000 than at 46666.

We ran the modulus encoder of the library itself (`V92ModulusEncoder::progress`) on random frames against our decoder. It agrees on all frames up to drn 14 (K = 62), and on only a fifth of them from drn 15. It uses signed 64 bits, and its digits come out negative.

Thus, our design has a maximum of drn 14 (`V92_DRN_MAX`). `DSP_V92_UP_DRN` can still ask for any rate.

The HSF modem connects at 41333 on the first try (`hsf_v92_pcmup_ulaw`, `hsf_v92_pcmup_alaw`; `hsf_v92_pcmup_noqc` without quick connect).

Through the converters of netsim, the error of the fit is 34.5 dB below the signal. That gives 36000 bit/s upstream with only the precoder. The rate of V.34 there is 31200, and 38667 makes some errors.

### The prefilter (6.4.2/V.92)

With the prefilter, the points of the analogue modem are the levels of G.711 itself. Thus, the network codec receives a level that it does not change. `DSP_V92_PREFILTER=0` disables it.

The design uses the last 3584 symbols of TRN2u (`V92_PF_FIT`):

1. **The channel** `ch` (24 taps before the main tap, 40 after it) is fitted by least squares from the TRN2u decisions to the received samples. The noise is also fitted: the error of the fit minus the quantisation of the codec, plus the estimation error of the fit.
2. **The prefilter** w (64 taps; z2 in Q0.15 with 4G in Q0.16) makes ch * w monic at a delay d, searched up to 40. It is fitted by least squares over the taps before d. The **precoder** (32 taps, p1 in Q1.14) takes the taps after d.
3. **For each rate**, the design selects the most widely spaced G.711 levels that keep the output power of the prefilter in 1: M (1 + sum |b|) + 2 of them, an even number. Then it simulates 4000 symbols of random data through ch, the noise and the codec. The fastest rate with 2 errors or fewer is the result.

The receiver takes the sample at the delay through 9 taps (LMS, to follow the timing of the analogue modem as it changes), minus the part of the symbols before B1u that the channel still has. It decides on a grid on which point j is at 2j + 1 (linear between the levels), for the same Viterbi decoder.

Through netsim, that gives 38666 bit/s with µ-law, and 40000 to 41333 with A-law.

### The analogue modem's timing slip

The design is correct only while the transmitter of the analogue modem keeps its timing against the codec of the network. The timing of the original library slips:

- It locks its transmitter to its estimate of our clock, which has an error of some hundredths of a ppm. The "Timing Offset" in its log is +0.02 to -0.05 ppm, but netsim has no offset. Thus, its signal moves by approximately 0.005 samples in 40 s.
- When the channel is fitted to the data symbols, it changes by that shift (about 1 % by 40 s), and the samples of the network codec move away from the levels. At levels 300 to 1000, 9 % of them come out on a neighbouring level at the start, and 44 % after 40 s.
- That occurs before the quantizer of the network. Thus, nothing in our receiver can correct it. More taps, faster LMS and taps on the output of the precoder did not help.
- The grid mse increases from about 0.012 to 0.09 in approximately 40 s, without errors.

Thus, the receiver follows the slip, and the design is done again for it, at the same rate, in a fast parameter exchange. `DSP_V92_SLIP=0` disables that.

**Measuring the slip.** In data mode, the receiver makes the line signal again from its decisions (the output of the precoder through the prefilter). It fits the residual of the received samples against the output of the channel to two things: a gain, and the derivative of the channel (band limited, from its taps), which is the slip. The fit forgets over one second.

The codec of the network passes a small change of its input only when the noise moves it across a threshold between two levels. Thus, the terms of each sample are weighed by the mean slope of the codec at that point (the steps of the levels below a Gaussian of the rms of the noise). Without the weights, the fit reads the slip at about 0.6 of its true value, and the design is not sufficient.

**Starting the exchange.** We start a fast parameter exchange when all of these conditions are true:

- the grid mse (over one second) is 0.02 above its value 3 s after the design;
- the fit has a slip of 0.001 samples or more (`V92_SLIP_MIN`); and
- the fit explains a minimum of 10 % of the residual.

The exchange carries these items:

- The channel moved by the fit (and moved more, at the rate of the slip, for the second by which the fit is late and the 3000 symbols to B1u).
- The prefilter for it at the same delay, and its precoder.
- A CPd with them and the rate and points that we have.

The original library uses the new coefficients when it gets our CPd (its log shows the precoder reset "with cfg" before SUVu'). Our receiver uses them at B1u, where the transmitter of the analogue modem starts again. The receiver makes the line before B1u again with the old coefficients.

**Measured** against netsim (the original library calls, 1500 s of line), µ-law and A-law:

- one exchange each 23 to 55 s (35 s on average), which takes 0.35 s from RM to B1u;
- B1u with errors in its first two frames, as in all fast parameter exchanges;
- no renegotiation and no retrain;
- the residual back at its start value (5.4 rms) after each exchange.

The slip comes from the original library itself, and it is steady: -0.00015 samples/s (approximately five times that after a retrain of the library). A renegotiation down is still necessary for a line that changes, for example by noise, which the fit does not see.

**RM with a slip.** With the slip, the line signal of RM (a large signal of period 4) comes in one full point off, in the same direction in each frame (A-law at a slip of 0.003: `0 34 1 35` for `35 35 0 0`). Random data only gets a larger mse. Thus, after our Rf, the receiver accepts RM with each class in one point of it, and at all times with 9 of the 11 exact. If not, the receiver does not see the exchange of the original library, and the library retrains.

| Test (`smartlink_v92_slip_*`, 30 s of data) | Result |
|---|---|
| `ulaw`, `alaw` | a fast parameter exchange for the slip (or more), the same rate after it, the data through |
| `smartlink_v92_slip_fpe` | after the slip's exchange, ours to a rate the design does not have (48000): the CPd stays as it is |

In data mode, the tone A of the analogue modem starts a retrain.

### Echo

The echo canceller is the echo canceller of the V.34 pump, at 8000 Hz. It trains while we send Sd to Jd. It must remove the echo of the hybrid at the line of the analogue modem.

That echo comes in before the network codec, and PCM upstream must have the points of the analogue modem on the levels of the codec. With echo, the codec quantizes the sum. After the canceller removes the echo, the codec error on it stays: about one level step divided by the square root of 12, independently of the level of the echo. That is approximately 26 rms at 33333 bit/s with the echo already 30 dB down, compared with 5 without echo.

The channel fit on TRN2u (not on the levels) does not see that. Thus, the simulation of the network codec in the design adds the echo (Gaussian, of the power that the canceller removes) before the codec, removes it after the codec, and adds the part of it that the canceller leaves. Then its grid mse is the same as in data mode (0.042 simulated against 0.043, and 0.039 against 0.038, with the echo 30 and 20 dB down), and the rate is also the same.

The receiver accepts Su only after we stop sending Sd to TRN1d. The analogue modem sends Su after it has heard Jd. Before that, a tone is the echo of our own Sd. A long echo delay brings it back after the canceller has started, but before the canceller has found the far echo.

| Test (`hsf_v92_echo`, HSF modem) | Result |
|---|---|
| its hybrid's echo 10 dB down after 50 ms (`NET_ECHO=-10:50`) | the canceller takes 34 dB away; V.92 connect 52000/29333 (28000 to 34666 at 20 ms; 30666 to 33333 with the echo 20 to 30 dB down; without echo the same line gives 41333 to 42666) |

### Rate renegotiation (9.8/V.92)

Rate renegotiation operates with PCM upstream.

**Our Rd.** It is R (+ + + - - -, made of the largest data mode Ucode of each interval) for 384 symbols, then R-bar for 24, on a data frame boundary. It answers the Ru and Ru-bar of the analogue modem, or starts our own renegotiation.

**Then:**

- TRN2d, SUVd and CPd go out on the training constellations of CPt.
- The receiver takes TRN2u 24 symbols after Ru-bar, on the grid of the data frames, and runs phase 4 again: SUVu, CPu, E2u, B1u, and a new CPd and precoder from TRN2u.

**When we start one:**

- down, when the margin of the data mode mse, compared with the mse at which we selected our rate, decreases below half for 0.5 s. From that time, the call stays below the rate that it had;
- up, when the margin stays above 2.5 times for 10 s, a minute or more after the last renegotiation (this uses a fast parameter exchange, refer to the subsequent section);
- `DSP_V92_RENEG=<ms>[:<drn>]` starts one at that time into data mode (to that rate).

**A silent period.** When SUVu asks for one (bit 32, Figure 18/V.92), we send Ed and then Ucode 0, and keep the data frames, until SUVu comes with bit 32 clear. Then we send Rt (of the constellations of CPt), Rt-bar and SUVd.

If a renegotiation does not end in data in 25 s, or if its B1u does not come through, the call retrains.

| Test | Result |
|---|---|
| `smartlink_v92_reneg`: ours after 3 s of data, to drn 6 | 56000/38666, then 56000/30666 |
| `smartlink_v92_reneg_noise`: noise up from 26 s (60 rms, `NET_NOISE_DIR=ab`) | ours on its own: 56000/24000 |

### Fast parameter exchange (9.9/V.92)

A fast parameter exchange sets new rates without training, all in the modulation of the data mode.

**Ours.**

1. Rf (R of + + - - in 12 symbols, the largest codes of the data constellation, 384 symbols) and Rf-bar (24 symbols), on a data frame boundary.
2. Then, from a cleared scrambler, signs and shaping filter: SUVd, CPd, SUVd', Ed, and B1d at the CPu that we get.

**Its CPd** keeps the precoder. Without the prefilter, it takes the rate from the data mode mse (with the margin of the training), up one step at most. It does this only with 2.5 times that margin at its current rate, as a renegotiation up must have. The mse at this rate makes the next rates look better than they are: without the prefilter, the B1u of the original library at 38666 and 40000 bit/s, up from 36000, came through or not because of the data before it. If not, the CPd stays without changes. With the prefilter, it takes the next rate up if its design has it. If not, the CPd stays without changes (the points stay at the current rate).

`DSP_V92_FPE=<ms>[:<drn>]` starts one at that time into data mode (to that rate), one time, when no exchange is in progress.

**Receiving it.**

1. The receiver finds the RM and RM' of the analogue modem among the decided classes (Table 25/V.92: the largest and the smallest, in pairs).
2. It takes SUVu, CPu, SUVu' and E2u from the data decoder, with its descrambler and sign started from zero again.
3. Then come FB1u (48 frames at the old parameters) and B1u at the new CPd. The front end and the delayed decisions change at that symbol.

At B1u, the analogue modem starts its scrambler, modulus encoder, convolutional encoder and precoder from zero (8.7.1/V.92). The receiver does the same:

- The paths of the Viterbi decoder are taken to their end on the best path (`Viterbi4d::flush()`, on the last symbols of FB1u). Then the decoder starts again from state 0. The descrambler and the sign start from zero at the first decision of B1u.
- With the precoder (no prefilter), the line still carries the earlier outputs of the precoder, which it does not cancel now. The receiver makes them again from the decoded points (`pq_xr`: the point plus the feedback of the outputs before it), and puts the last 16 into the feedback at B1u. The points of the slicer are not sufficient. The line passes almost nothing above 4 kHz. Thus, the few points that the slicer gets incorrect leave an alternating part in the outputs that it makes again, which the line never shows, until the restart.
- Where the recursion of the precoder increases, a rounding that is not exactly the same as the rounding of the analogue modem increases without limit (with the original library, in some thousand symbols). The analogue modem keeps its own output small by the points that it selects. When the value is more than twice the outermost point, the receiver stops the rebuild, and the first frames of B1u have errors as before.

**Results.**

- `open_v92a_fpe_precoder` has FB1u and B1u without errors. Before the rebuild, one call in two had tens of errors in the first frames of B1u.
- Against the original library, ours takes 0.5 s (a renegotiation takes about 2 s): B1d without errors, and B1u with some errors in its first two frames, as after a renegotiation.
- Our rate changes go up by fast parameter exchange (the same line with less noise) and down by renegotiation (train again).

### Fast parameter exchange with the prefilter

With the prefilter, the original library does three things differently:

- **It starts its precoder and prefilter again at SUVu** (and at B1u). Its long precoder makes RM' unreadable. Thus, the receiver finds SUVu by the time: 384T of RM and 24T of RM' after the start of RM (9.9.2.2.2/V.92). The receiver makes the line signal before it again from its decisions (the output of the precoder, through the prefilter) and removes it, as it does with the line signal of TRN2u before B1u.
- **It changes points early.** After it has our CPd, it sends SUVu', E2u and FB1u on the new points with the old moduli, and the remaining part of the CPd from B1u. We see this in the grid mse on the new points against the old points, and we slice on the new points from there.
- **Thus, the new points must suit the old moduli:** the same number of points, and not further apart. If not, its signal goes above its power, and some classes of the fourth symbol of the trellis frame have no point. Thus, with the prefilter, a fast parameter exchange never goes down (a renegotiation does). It goes up when the margin permits the next rate, or it stays at the current rate.

`smartlink_v92_fpe_prefilter` asks for 30666 and gets the exchange at 38666, through to B1u, and then the data continues.

### The analogue modem's own exchange

The original library answers our exchange, but it does not start its own: its `V92Modulator::initiateFPE` is called only on our Rf. `SMARTLINK_FPE_AT=<s>` of `half_smartlink` calls it at that time into data mode. It gets to it through the `VPcmFloModem` of the library, which `vpcm_create` keeps at 0x3574 of the data pump. That object keeps the `V92Modulator` at 0x6124.

Its RM comes without RM' (as with our exchange, the receiver finds SUVu by the time). With the prefilter, it is one full point off in each frame, as RM after our Rf is. Thus, the receiver accepts RM with each class in one point of it, for our exchange or for the exchange of the library.

`smartlink_v92_fpe_remote` (and `smartlink_v92_fpe_remote_noprefilter`) run its RM, our Rf, its SUVu, CPu, SUVu', E2u, FB1u and B1u, and the data continues, µ-law and A-law. Without the prefilter, it stays at its current rate (36000), because the margin for a step up is not available.

### Modems without the exchange

The HSF modem has no fast parameter exchange: our Rf gets no RM. If no RM comes one second after our Rf, we stop the exchange and do a renegotiation. For the remaining part of that call, we renegotiate instead. That includes the slip: a renegotiation down when the decisions become worse. `hsf_v92_fpe` tests this.

| Fast parameter exchange runs | Result |
|---|---|
| `smartlink_v92_fpe`: ours after 3 s, to drn 6 (no prefilter) | 56000/30666, data without errors |
| ours, rate from the mse (µ-law) | 36000 or 37333 |
| ours to drn 6, A-law | 56000/30666 |
| noise upstream from 26 s to 40 s (60 rms) | renegotiation down to 24000; a minute later, a fast parameter exchange up to 33333 |

Only the first row is an interop test. The other rows are measured runs.

### The original library's renegotiations

The original library starts its own renegotiations when the line becomes worse downstream (in netsim: `NET_NOISE_DIR=bacodes`, noise added to the PCM codes), and asks for the silent period. We go through it. But the CPu that it sends after this has its earlier rate, while its receiver expects one rate down. 15 ms into B1d, it retrains with tone A.

Its log and code show the cause:

- It builds the parameters of the CPu (`setV92CPpckFromParamsInfo`) when its TRN2u stops, from the design that it has at that time (56000).
- After the silence, it designs again, "one rate down on silence rrn" (54666), only for its receiver.
- The CPu that it then sends, made again when it gets our SUVd after the silence (`recivedPartTwoSilenceRrnSUV`: we send it, and it accepts it), still has the parameters of the first design.

That is the behaviour of the library itself, independently of the digital modem, and its new constellation is not in any item that it sends. The call continues after its retrain.

The `$!$` log lines of the library come from `edprintf`. Each byte is two characters: a nibble plus a key that goes round 4 6 2 7 1 9 3 5 8 7, plus '0'. `test/smartlink_log.py` decodes them.

`NET_NOISE_QUIET=<ms>` stops the noise of netsim when the signal to which it adds noise has been silent for that time. Use it to try this on a clean line.

If the analogue modem starts a renegotiation during our fast parameter exchange, we stop the exchange and run phase 4 from its TRN2u instead. The original library starts such a renegotiation when noise prevents it from reading our CPd.

## The Agere modem and the analogue loop

The V.90 training of the Agere modem failed at some line delays, in a pattern that repeats each 48 samples (the blocks of its codec), when the line gave it our PCM values without changes, sample for sample. Through an analogue loop (`NET_LOOP` of netsim: a fraction of a sample of delay and a high pass), it trains at all delays. The Agere tests use that, with 0.45 of a sample.

The value is not 0.5, because with no clock offset and no noise, our sample instants then fall exactly between the samples of the Agere. At some alignments of the call to the blocks of its codec (`AGERE_SYNC_PHASE` 96 and 288 of 4800), it judges TRN2d badly and selects 33333 bit/s downstream instead of 53333. At 0.45 and 0.55, all alignments get 53333 or 54666, and at 0.3 they get 52000. The signal is the same. Thus, this is a tie in the receiver of the Agere, not in our receiver. `agere_v90_phase96` keeps that alignment.
