# V.92: technical details

This is the technical companion to [V.92](v92.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

This page covers V.92 on the digital modem's side: quick connect, modem-on-hold, and PCM upstream with its rate renegotiation and fast parameter exchange. The start-up and modem-on-hold live in `dsp/dp_v34.cpp`; PCM upstream is `V92Up` in `dsp/v92.cpp`.

Terms used throughout:

- The **digital modem** is our side (the server, on a G.711 network at 8000 Hz). The **analogue modem** is the client on the phone line.
- **INFO0d / INFO1d** are the digital modem's phase 2 frames, **INFO0a / INFO1a** the analogue modem's.
- **drn** is the upstream rate code: (drn + 17) × 8000 / 6 bit/s, so drn 14 is 41333 bit/s.
- **mse** is the receiver's mean squared error.

## Switching V.92 on and off

With `AT+MS=92` (the default) on an 8000 Hz line, the digital modem says V.92 in INFO0d (bit 27) and sends V.92's INFO1d. INFO1d offers PCM upstream (bit 70; see [PCM upstream](#v92-pcm-upstream)). Without the offer, the analogue modem asks for V.90 or V.34 upstream, and the call goes on as V.90.

| Variable | Effect |
|---|---|
| `DSP_V92=0` | leaves V.92 out (the call is V.90) |
| `DSP_V92_QC=0` | answers QC1a with V.8 instead of quick connect |
| `DSP_V92_SHORT2=0` | does not ask for short phase 2 |
| `DSP_V92_PCMUP=0` | does not offer PCM upstream |
| `DSP_V92_MOH=<code>` | the modem-on-hold time we grant (overrides `AT+PMHT` and `AT+PMH`) |
| `DSP_V92_MOH_REQ=<ms>[:<s>]` | asks for modem-on-hold that far into data mode (a test) |

The PCM upstream variables are listed under [PCM upstream](#v92-pcm-upstream).

## V.92 quick connect

### Short phase 1 (9.2.4/V.92), answering

The analogue modem starts with **QC1a** ahead of its CM. QC1a is its frame on V.21(L), and carries the Ucode of QTS and LAPM. We answer:

1. **QCA1d** on V.21(H) (with ANSpcm at -12 dBm0);
2. 75 ms of silence;
3. **QTS**, **QTS-bar** and **ANSpcm**, as PCM codes. Tables 7 to 10/V.92 come out of the G.711 quantization of Table 6/V.92.

The analogue modem's **TONEq** (980 Hz) ends this. We wait for TONEq to end too, then send 75 ms of silence and go on to phase 2 as after V.8. Sd of phase 3 starts in data frame interval 0 of QTS (8.3.6/V.92).

Without TONEq within 2 s, the call goes on with V.8 after all.

The analogue modem may come to phase 2 after our INFO0d is over (the original library does). Once its INFO0a is in, INFO0d goes out again with the acknowledgement, as in phase 2's INFO0 recovery (see [V.8 and V.34](v34-details.md)).

### Catching QC1a and TONEq

QC1a comes twice, and the analogue modem goes on with CM after it until it hears QCA1d. The first QC1a starts as the V.21 receiver picks up the carrier, so its sync is taken with fewer than its ten leading ones. When our QCA1d came only after the second QC1a, the Agere modem (at some line delays) kept sending CM.

If the analogue modem misses QCA1d all the same, the CM it goes on with is answered with JM (V.8, full phase 2).

TONEq is V.21's low channel mark. It counts only after 128 ms of 980 Hz without that channel's space (1180 Hz), and only while we send ANSpcm, so that CM's runs of marks do not pass for it.

### Which modems send QC1a

- The original library sends QC1a on every call.
- The Agere modem (with `AT+PQC=0`) sends it on a second call to a number it has connected to before.

Both then connect at V.90 (`smartlink_v92_qc`, `agere_v92_qc`).

### Short phase 2 (9.4/V.92)

When both modems ask for short phase 2 (never in a retrain), there is no probing and no INFO1d:

- The digital modem reverses tone B once tone A is there and B has been on for 50 ms.
- It measures the round trip from the reversal of A, then waits for INFO1a.
- The upstream rate comes from the mse of the training in phase 3 instead of from the probing.

INFO1a's bit 33 names the analogue modem's carrier (Table 19/V.92). We receive on the low carrier whatever it says, because the modems that set the bit send on the low one:

- the SM56 modem at 3200 baud: its signal is centred where it is after a full phase 2, 90 Hz below the high carrier;
- the Agere modem at 3429 baud, which has only one carrier.

With quick connect and short phase 2, the Agere modem's second call connects in 17.5 s instead of 22.5 s. It asks for a shorter DIL as well.

## V.92 modem-on-hold

### When the analogue modem asks (9.10/V.92)

When call waiting interrupts the analogue modem, it can ask us to hold the call. It sends its tone A, as for a retrain, and then MH sequences instead of the reversal. An MH sequence (Table 32/V.92) is fill, frame sync, a 4 bit signal and 4 bits of information, and a CRC, on the modulation of INFO. We look for MH sequences while we retrain, and answer:

- **MHreq** gets **MHack** with T1, the time we hold the call, or **MHnack**:
  - `AT+PMHT=<code>` sets T1 as a code of Table 33/V.92 (1 = 10 s, ..., 5 = 1 min, the default, ..., 12 = 16 min, 13 = no limit).
  - `AT+PMHT=0` or `AT+PMH=1` makes us answer MHnack.
  - `DSP_V92_MOH=<code>` overrides both (0 denies).
- After MHack, once the analogue modem's tone has been there 100 ms or silence 2 s, the call is on hold. We send ANSam until T1 is over, and then disconnect. If neither comes within 35 s, we disconnect.
- On hold we listen for QC1a or CM. Either starts phase 1 again, with us as the answering modem, then phases 2 to 4. The data goes on without a new CONNECT; V.42's timers count bits, so they wait too.
- A CM without modulations, or a QC1a with UQTS 1111, clears down.
- **MHclrd** gets **MHcda**, then we disconnect.
- **MHfrr** (a fast reconnect, also after our MHnack) gets ANSam and phase 1 at once (ANSam for up to 10 s).
- **MHcda** after our MHnack: we disconnect. Without MHcda or MHfrr within 12 s of MHnack, we disconnect too.

The Agere modem asks for modem-on-hold with `AT+PMHR`. It answers `+PMHR:5` when we grant 1 min and `+PMHR:0` when we deny it. It comes back with CM after `ATO`; denied, it asks for a fast reconnect. `agere_v92_moh_grant` and `agere_v92_moh_deny` do this in the middle of the data (`MIDCALL_A` of `interop_call`) and check all of it both ways.

### When we ask (9.10.2.1/V.92)

We ask for it with `AT+PMHR` in online command mode on a V.92 connection. `AT+PMHR=<s>` holds the remote that long (1 to 960 s); plain `AT+PMHR` holds it 10 s. `DSP_V92_MOH_REQ=<ms>[:<s>]` asks that far into data mode (a test).

We leave data mode as for a retrain, send tone B for 600 ms, then send MHreq, repeated, until the answer:

- **MHack:** we stay silent (the remote is on hold) for those seconds, or until 2 s before its T1. Then we take it back. The remote sends ANSam through its T1 (9.10.2.1/V.92). Once its ANSam has been there 200 ms (`MOH_BACK`), we run V.8 as the call modem (CM), phases 2 to 4, and the data goes on. With no ANSam within 5 s, the remote is gone and we disconnect.
- **MHnack:** we send MHfrr (for 300 ms), then ANSam and phase 1. With `=0` seconds in `DSP_V92_MOH_REQ`, we send MHcda and disconnect instead.
- **The remote's tone A reversed (or its INFO0a):** it took our tone for a retrain's, which it may do (9.10.1.1/V.92). We follow it into phase 2 at once. Tone B is long (600 ms, where 50 ms would do) so that this happens before MHreq, whose phase steps the remote would take for our reversal.
- **Nothing within 2.5 s** after the tone: a retrain.

Neither the original library nor the Agere modem answers MHreq; both retrain. The library does frame our MH sequences, but as INFO0 in its retrain, with a bad CRC; it looks for MH only in its own requests. `smartlink_v92_moh_request` (with PCM upstream) and `agere_v92_moh_request` (V.90) ask for it in the middle of the data (`MIDCALL_B`) and check that the call goes on.

### Testing MHack and MHnack with netsim

For the MHack and MHnack branches, netsim stands in for an analogue modem that answers: `NET_MOH=ack[:<T1 code>]@<s>` or `NET_MOH=nack@<s>`. From s seconds into the line, once our tone B has been there 50 ms, it cuts the modem off both ways and sends tone A, then MHack or MHnack, until we go silent (on hold) or send ANSam. Then it gives the line back.

These tests check the sequence in our log, and expect the call to end (`EXPECT_DROP` of `interop_call`):

| Test | Setting | What it checks |
|---|---|---|
| `smartlink_v92_moh_request_ack` | `NET_MOH=ack:1@22` (T1 10 s), `AT+PMHR=5` | MHack, 5 s on hold, then we take the remote back; no ANSam comes, and we disconnect after 5 s |
| `smartlink_v92_moh_request_nack` | `NET_MOH=nack@22` | MHnack, 300 ms of MHfrr, ANSam; nothing comes back, and we disconnect after T1 (10 s) |

The original library behind netsim has been cut off in a retrain and hangs up, so nothing comes back. The way back (CM or QC1a after our ANSam) is the same as after the analogue modem's own hold, which `agere_v92_moh_grant` tests.

## V.92 PCM upstream

INFO1d says the channel can carry PCM upstream (bit 70). An analogue modem that wants it answers with the INFO1a of Table 18/V.92. From there `V92Up` ([v92.cpp](../../dsp/v92.cpp)) runs phases 3 and 4 of V.92 and the data mode on the network's 8000 Hz samples, as V.91 does. Our transmitter in this direction is V.90's PCM transmitter.

| Variable | Effect |
|---|---|
| `DSP_V92_PCMUP=0` | does not offer PCM upstream |
| `DSP_V92_UP_DRN=<n>` | asks for (n + 17) × 8000 / 6 bit/s instead of the designed rate (any n up to 19) |
| `DSP_V92_PRECODE=0` | sends no precoder: decision feedback in the receiver, margin 40 |
| `DSP_V92_TRELLIS=0` | does not decode the trellis code; `=16` or `=32` chooses that code (64 states by default) |
| `DSP_V92_MARGIN=<x>` | overrides the design margin (8 with precoding and trellis decoding, else 40) |
| `DSP_V92_PREFILTER=0` | no prefilter (the precoder alone) |
| `DSP_V92_SLIP=0` | no fast parameter exchange for the analogue modem's timing slip |
| `DSP_V92_RENEG=<ms>[:<drn>]` | our own renegotiation that far into data mode (to that rate), once |
| `DSP_V92_FPE=<ms>[:<drn>]` | our own fast parameter exchange that far into data mode (to that rate), once |

### The receiver in phase 3

The analogue modem sends PAM symbols at 8000 symbol/s, one per sample. The receiver finds them as follows:

- **Ru and Su** have a period of 6 symbols (1333 Hz). They are found by a sliding DFT of 12 samples, and their phase reversals between two of its windows. Su counts once 48 of its samples are in, and its phase is taken from those: V.92 has Su last 144T before Su-bar, but the Broadcom modem's lasts 96T.
- **TRN1u** is found by its correlation with the known scrambled sequence. The correlation is the channel's impulse response, and gives the symbol alignment (and, for the second TRN1u, the data frame alignment).
- A **decision feedback equalizer** (32 feed forward and 16 feedback taps) is fitted to TRN1u by least squares, and then follows the signal by its decisions.
- **Ja, CPt and E1u** are 2 point signals. **TRN2u, SUVu, CPu and E2u** are 4 point signals (differential sign, scrambled).
- **E1u** is a data frame of zeros that starts after the last CPt's end. CPt's fill to the end of its frame can be all zeros, together with its CRC's last bit; our own analogue modem's CPt showed this. **E2u** likewise starts after the last SUVu or CPu.

### What we send

1. Sd, TRN1d and Jd.
2. Jp, once Su, Su-bar and Su have been heard. Su-bar takes half a symbol, and Jp asks for half a symbol more, so the analogue modem goes back to the phase of the first TRN1u.
3. Jp' on the next Su-bar.
4. The DIL it asked for (or SCR).
5. Ri on CPt, and Ri-bar on E1u.
6. TRN2d, SUVd, CPd (after SUVu), SUVd' (after CPu), Ed and B1d, in CPt's and CPu's constellations through V.90's encoder.

### The precoder and the rate (CPd)

Our CPd makes the analogue modem's precoder do what the equalizer's decision feedback did: p1 = -b (16 taps, Q1.14).

- **A modulo precoder.** Of the points of a class, the analogue modem's precoder picks the one that keeps its output least. So its output stays within the classes' spacing even where 1 / C(z) is not stable. Here it is not stable: the channel's edge at 4000 Hz puts a root of C on the unit circle.
- **The receiver.** It is left with the feed forward taps (refitted by least squares to the TRN2u decisions, with the feedback the precoder takes) and the points themselves. There is no decision feedback and so no error propagation, and the points are in order for the trellis code. A Viterbi decoder (V.34's 64 state code on pairs of symbols, 6.4.4/V.92) decides them before their classes.
- **The rest of CPd.** The same modulus M on all 12 data frame intervals; one constellation of points (2j + 1) × 16, as many as the precoder needs (M (1 + sum |b|) positive ones); the prefilter z2(0) = 1; and G for an output of unit power.
- **The rate.** The fastest rate, of those in the analogue modem's DIL descriptor, whose half distance between points is at least sqrt(8) times the fit's rms error.
- **Shrinking the feedback.** The feedback taps may be shrunk (b_j g^j) when that lets a faster rate fit in 128 points.

| At 36000 / 38667 bit/s (µ-law) | B1u | Data |
|---|---|---|
| precoding and trellis decoding | 0 / 0 errors | 0 / 15 HDLC errors in 10 s (64 states; 33 with 16) |
| precoding only | 0 errors at 33333 | 14 HDLC errors at 33333 |
| no precoding | fails at 33333 | |

### Falling back

The call retrains without the offer, to V.90 with V.34 upstream, when:

- the analogue modem retrains instead of sending INFO1a;
- our phase 3 or 4 fails, or does not reach data mode within 40 s; or
- B1u does not come through (more than 5 % of its bits wrong, not counting its first four frames).

### Against Smart Link's library

Smart Link's library is a "V.92 Lite" build. Its runtime clears the bit that would let it use PCM upstream (byte 2, bit 7 of `dp_runtime_create`'s object). With our offer it retrains to V.34 upstream after INFO1d ("we got PCM upstream under V.92Lite").

`SMARTLINK_V92FULL=1` sets the bit in `half_smartlink` ([test/smartlink_glue.cpp](../../dsp/test/smartlink_glue.cpp)), and the library then does PCM upstream. It needs two things of our CPd:

- Filter sections of a multiple of 4 taps. Its FIR rounds the length down, and crashes on 0.
- An even number of positive points, 2M at least. On the trellis frame's fourth symbol it looks for the point of a class up to 2 (K + z M) + parity, with z up to (LC - 1 - 2K) / 2M rounded toward zero. That takes one point past its table (it sends 0) when LC is odd, and more when LC is less than 2M.

| Test (`smartlink_v92_pcmup_*`) | Result |
|---|---|
| `ulaw`, `alaw` | V.92 connect, 56000 down, 38666 / 40000 up (prefilter); B1u and 10 s of data without errors |
| `noprefilter` (`DSP_V92_PREFILTER=0`) | 56000 down, 34000 to 36000 up |
| `calls` (two calls, quick connect; `NET_FLOOR=3`, as V.90's µ-law tests: see [V.90](v90.md)) | both V.92, 54666 down, 38666 up |
| `lite` (the library as it is) | it retrains after INFO1d, we leave the offer out: V.90 |
| `fallback` (44000 bit/s asked for, `DSP_V92_UP_DRN=16`) | B1u does not come through: retrain, V.90 (its modulus encoder fails above 41333: see below) |

`smartlink_v92_nopcmup` (`DSP_V92_PCMUP=0`) connects at V.90 with V.34 upstream.

### The 41333 bit/s ceiling

Neither the original library nor the HSF modem can send more than 41333 bit/s upstream, though both offer every rate in Ja. From 42666 bit/s (drn 15, K = 64 bits a data frame) B1u does not come through. The points are decided cleanly (the mse is as low as at 38666), yet a third of the bits are wrong, in bursts, and there are no more of them at 48000 than at 46666.

The library's own modulus encoder (`V92ModulusEncoder::progress`), run on random frames against our decoder, agrees on every frame up to drn 14 (K = 62), and on only a fifth of them from drn 15 on. It works in signed 64 bits, and its digits come out negative.

So we design for drn 14 at most (`V92_DRN_MAX`). `DSP_V92_UP_DRN` can still ask for any rate.

The HSF modem connects at 41333 on the first try (`hsf_v92_pcmup_ulaw`, `hsf_v92_pcmup_alaw`; `hsf_v92_pcmup_noqc` without quick connect).

Through netsim's converters the fit's error is 34.5 dB below the signal. That gives 36000 bit/s upstream with the precoder alone; V.34's rate there is 31200, and 38667 makes a few errors.

### The prefilter (6.4.2/V.92)

With the prefilter, the analogue modem's points are G.711's own levels, so what reaches the network's codec is a level it passes unchanged. `DSP_V92_PREFILTER=0` turns it off.

The design uses the last 3584 symbols of TRN2u (`V92_PF_FIT`):

1. **The channel** `ch` (24 taps before the main one, 40 after) is fitted by least squares from the TRN2u decisions to the received samples. So is the noise: the fit's error less the codec's quantisation, plus the fit's own estimation error.
2. **The prefilter** w (64 taps; z2 in Q0.15 with 4G in Q0.16) makes ch * w monic at a delay d, searched up to 40. It is fitted by least squares over the taps before d. The **precoder** (32 taps, p1 in Q1.14) takes the taps after d.
3. **For each rate**, the design picks the most widely spaced G.711 levels that keep the prefilter's output power within 1: M (1 + sum |b|) + 2 of them, an even number. It then simulates 4000 symbols of random data through ch, the noise and the codec. The fastest rate with 2 errors or fewer wins.

The receiver takes the sample at the delay through 9 taps (LMS, to follow the analogue modem's timing as it wanders), less what the channel still has of the symbols before B1u. It decides on a grid on which point j is at 2j + 1 (linear between the levels), for the same Viterbi decoder.

Through netsim that gives 38666 bit/s with µ-law, and 40000 to 41333 with A-law.

### The analogue modem's timing slip

The design holds only as long as the analogue modem's transmitter keeps its timing against the network's codec. The original library slips:

- It locks its transmitter to its estimate of our clock, which is off by a few hundredths of a ppm. Its log's "Timing Offset" reads +0.02 to -0.05 ppm, while netsim has no offset at all. So its signal moves by some 0.005 samples in 40 s.
- Fitted to the data symbols, the channel changes by just that shift (about 1 % by 40 s), and the network codec's samples stray from the levels. At levels 300 to 1000, 9 % of them come out on a neighbouring level at the start, 44 % after 40 s.
- That happens before the network's quantizer, so nothing in our receiver can take it back. More taps, faster LMS and taps on the precoder's output made no difference.
- The grid mse rises from about 0.012 to 0.09 in some 40 s, without errors.

So the receiver follows the slip, and the design is redone for it, at the same rate, in a fast parameter exchange. `DSP_V92_SLIP=0` turns that off.

**Measuring the slip.** In data mode the receiver rebuilds the line signal from its decisions (the precoder's output through the prefilter). It fits the residual of the received samples against the channel's output to two things: a gain, and the channel's derivative (band limited, from its taps), which is the slip. The fit forgets over a second.

The network's codec passes a small change of its input only as far as the noise carries it across a threshold between two levels. So each sample's terms are weighed by the codec's mean slope there (the levels' steps under a Gaussian of the noise's rms). Unweighed, the fit reads the slip at about 0.6 of what it is, and the design falls short.

**Starting the exchange.** We start a fast parameter exchange when all of these hold:

- the grid mse (over a second) is 0.02 above what it was 3 s into the design;
- the fit has a slip of 0.001 samples or more (`V92_SLIP_MIN`); and
- the fit explains at least 10 % of the residual.

The exchange carries the channel moved by the fit (and moved on, at the slip's rate, for the second the fit lags and the 3000 symbols to B1u), the prefilter for it at the same delay, its precoder, and a CPd with them and the rate and points we have.

The original library takes the new coefficients up when it gets our CPd (its log shows the precoder reset "with cfg" before SUVu'). Our receiver takes them up at B1u, where the analogue modem's transmitter starts anew; the line before B1u is rebuilt with the old ones.

**Measured** against netsim (the original library calling, 1500 s of line), µ-law and A-law:

- one exchange every 23 to 55 s (35 s on average), taking 0.35 s from RM to B1u;
- B1u with errors in its first two frames, as in any fast parameter exchange;
- no renegotiation and no retrain;
- the residual back at its start (5.4 rms) after each one.

The slip is the original library's own, and steady: -0.00015 samples/s (some five times that after a retrain of its own). A renegotiation down remains for a line that changes, for example by noise, which the fit does not see.

**RM with a slip.** With the slip, RM's line signal (a large one of period 4) comes in a whole point off, the same way in every frame (A-law at a slip of 0.003: `0 34 1 35` for `35 35 0 0`). Random data only gets a larger mse. So after our Rf the receiver takes RM with every class within one of it, and at any time with 9 of the 11 exact. Otherwise the original library's exchange goes by unseen and it retrains.

| Test (`smartlink_v92_slip_*`, 30 s of data) | Result |
|---|---|
| `ulaw`, `alaw` | a fast parameter exchange for the slip (or more), the same rate after it, the data through |
| `smartlink_v92_slip_fpe` | after the slip's exchange, ours to a rate the design does not have (48000): the CPd stays as it is |

In data mode, the analogue modem's tone A starts a retrain.

### Echo

The echo canceller is the V.34 pump's, at 8000 Hz, trained while we send Sd to Jd. It has to take away the echo of the hybrid at the analogue modem's line.

That echo comes in before the network codec, and PCM upstream relies on the analogue modem's points arriving on the codec's levels. With echo, the codec quantizes the sum. What is left once the canceller has taken the echo away is the codec's error on it: about a level step over the square root of 12, whatever the echo's level. That is some 26 rms at 33333 bit/s with the echo already 30 dB down, against 5 without echo.

The channel fit on TRN2u (not on the levels) does not see that. So the design's simulation of the network codec adds the echo (Gaussian, of the power the canceller takes away) before the codec, takes it away after, and adds what the canceller leaves of it. Its grid mse then comes out as data mode has it (0.042 simulated against 0.043, and 0.039 against 0.038, with the echo 30 and 20 dB down), and so does the rate.

Su is only taken once we no longer send Sd to TRN1d; the analogue modem sends Su once it has heard Jd. Before that, a tone is the echo of our own Sd, which a long echo delay brings back after the canceller has started but before it has found the far echo.

| Test (`hsf_v92_echo`, HSF modem) | Result |
|---|---|
| its hybrid's echo 10 dB down after 50 ms (`NET_ECHO=-10:50`) | the canceller takes 34 dB away; V.92 connect 52000/29333 (28000 to 34666 at 20 ms; 30666 to 33333 with the echo 20 to 30 dB down; without echo the same line gives 41333 to 42666) |

### Rate renegotiation (9.8/V.92)

Rate renegotiation works with PCM upstream.

**Our Rd.** It is R (+ + + - - -, made of each interval's largest data mode Ucode) for 384 symbols, then R-bar for 24, on a data frame boundary. It answers the analogue modem's Ru and Ru-bar, or starts a renegotiation of ours.

**Then:**

- TRN2d, SUVd and CPd go out on CPt's training constellations.
- The receiver takes TRN2u 24 symbols after Ru-bar, on the data frames' grid, and runs phase 4 again: SUVu, CPu, E2u, B1u, and a new CPd and precoder from TRN2u.

**When we start one:**

- down, when the margin of the data mode mse against the one our rate was chosen with drops below half for 0.5 s. From then on, the call stays below the rate it had;
- up, when the margin stays above 2.5 times for 10 s, a minute or more after the last renegotiation (this goes by fast parameter exchange, see below);
- `DSP_V92_RENEG=<ms>[:<drn>]` starts one that far into data mode (to that rate).

**A silent period.** When SUVu asks for one (bit 32, Figure 18/V.92), we send Ed and then Ucode 0, keeping the data frames, until SUVu comes with bit 32 clear. Then we send Rt (of CPt's constellations), Rt-bar and SUVd.

A renegotiation that does not end in data within 25 s, or whose B1u does not come through, retrains.

| Test | Result |
|---|---|
| `smartlink_v92_reneg`: ours after 3 s of data, to drn 6 | 56000/38666, then 56000/30666 |
| `smartlink_v92_reneg_noise`: noise up from 26 s (60 rms, `NET_NOISE_DIR=ab`) | ours on its own: 56000/24000 |

### Fast parameter exchange (9.9/V.92)

A fast parameter exchange sets new rates without training, all in the data mode modulation.

**Ours.**

1. Rf (R of + + - - in 12 symbols, the data constellation's largest codes, 384 symbols) and Rf-bar (24 symbols), on a data frame boundary.
2. Then, from a cleared scrambler, signs and shaping filter: SUVd, CPd, SUVd', Ed, and B1d at the CPu we get.

**Its CPd** keeps the precoder. Without the prefilter it takes the rate from the data mode mse (with the training's margin), up a step at most, and only with 2.5 times that margin at the rate it has, as a renegotiation up wants. The mse at this rate makes the next ones look better than they are: without the prefilter, the original library's B1u at 38666 and 40000 bit/s, up from 36000, came through or not by the data before it. Otherwise the CPd stays as it is. With the prefilter it takes the next rate up if its design has it, else the CPd as it is (the points stay at the rate we have).

`DSP_V92_FPE=<ms>[:<drn>]` starts one that far into data mode (to that rate), once, when no exchange is under way.

**Receiving it.**

1. The receiver finds the analogue modem's RM and RM' among the decided classes (Table 25/V.92: the largest and the smallest, in pairs).
2. It takes SUVu, CPu, SUVu' and E2u from the data decoder, with its descrambler and sign started from zero again.
3. Then come FB1u (48 frames at the old parameters) and B1u at the new CPd. The front end and the delayed decisions switch at that symbol.

At B1u the analogue modem starts its scrambler, modulus encoder, convolutional encoder and precoder from zero (8.7.1/V.92), and the receiver does the same:

- The Viterbi decoder's paths are taken to their end on the best one (`Viterbi4d::flush()`, on FB1u's last symbols), and it starts again from state 0. The descrambler and the sign start from zero at B1u's first decision.
- With the precoder (no prefilter), the line still carries the precoder's earlier outputs, which it no longer cancels. The receiver rebuilds them from the decoded points (`pq_xr`: the point plus the feedback of the outputs before it) and puts the last 16 into the feedback at B1u. The slicer's points will not do: the line hardly passes 4 kHz, so the few points the slicer gets wrong leave an alternating part in the rebuilt outputs that the line never shows, until the restart.
- Where the precoder's recursion grows, a rounding not quite the analogue modem's own runs away (the original library's, within a few thousand symbols); the analogue modem keeps its own output small by the points it picks. Past twice the outermost point the receiver gives the rebuild up, and B1u's first frames have errors as before.

**Results.**

- `open_v92a_fpe_precoder` has FB1u and B1u without errors. Before the rebuild, one call in two had tens of errors in B1u's first frames.
- Against the original library, ours takes 0.5 s (a renegotiation takes about 2 s): B1d without errors, and B1u with a few in its first two frames, as after a renegotiation.
- Our rate changes go up by fast parameter exchange (the same line with less noise) and down by renegotiation (train again).

### Fast parameter exchange with the prefilter

With the prefilter, the original library does three things differently:

- **It restarts its precoder and prefilter at SUVu** (and at B1u). Its long precoder makes RM' unreadable, so SUVu is taken by the time: 384T of RM and 24T of RM' after RM's start (9.9.2.2.2/V.92). The receiver rebuilds the line signal before it from its decisions (the precoder's output, through the prefilter) and takes it away, as it does TRN2u's before B1u.
- **It switches points early.** Once it has our CPd, it sends SUVu', E2u and FB1u on the new points with the old moduli, and the rest of the CPd from B1u on. We see this in the grid mse on the new points against the old, and slice on the new ones from there.
- **So the new points have to suit the old moduli:** as many, and no further apart. Otherwise its signal goes beyond its power, and some classes of the trellis frame's fourth symbol have no point. With the prefilter, then, a fast parameter exchange never goes down (a renegotiation does). It goes up when the margin allows the next rate, or stays at the rate it has.

`smartlink_v92_fpe_prefilter` asks for 30666 and gets the exchange at 38666, through to B1u and on with the data.

### The analogue modem's own exchange

The original library answers ours but starts none of its own: its `V92Modulator::initiateFPE` is only called on our Rf. `half_smartlink`'s `SMARTLINK_FPE_AT=<s>` calls it that far into data mode. It reaches it through the library's `VPcmFloModem`, which `vpcm_create` keeps at 0x3574 of the data pump; that object keeps the `V92Modulator` at 0x6124.

Its RM comes without RM' (as with ours, SUVu is taken by the time). With the prefilter it is a whole point off in every frame, as RM after our Rf is, so the receiver takes RM with every class within one of it, ours or not.

`smartlink_v92_fpe_remote` (and `smartlink_v92_fpe_remote_noprefilter`) run its RM, our Rf, its SUVu, CPu, SUVu', E2u, FB1u and B1u, and the data goes on, µ-law and A-law. Without the prefilter it stays at the rate it has (36000), because the margin for a step up is not there.

### Modems without the exchange

The HSF modem has no fast parameter exchange: our Rf gets no RM. A second after our Rf with no RM, we give the exchange up for a renegotiation, and renegotiate instead for the rest of that call. That includes the slip: a renegotiation down once the decisions suffer. `hsf_v92_fpe` tests this.

| Fast parameter exchange runs | Result |
|---|---|
| `smartlink_v92_fpe`: ours after 3 s, to drn 6 (no prefilter) | 56000/30666, data without errors |
| ours, rate from the mse (µ-law) | 36000 or 37333 |
| ours to drn 6, A-law | 56000/30666 |
| noise upstream from 26 s to 40 s (60 rms) | renegotiation down to 24000; a minute later, a fast parameter exchange up to 33333 |

Only the first row is an interop test; the others are measured runs.

### The original library's renegotiations

The original library starts renegotiations of its own when the line gets worse downstream (in netsim: `NET_NOISE_DIR=bacodes`, noise added to the PCM codes), and asks for the silent period. We go through it, but the CPu it sends afterwards has its earlier rate while its receiver expects one rate down, and 15 ms into B1d it retrains with tone A.

Its own log and code show why:

- It builds the CPu's parameters (`setV92CPpckFromParamsInfo`) when its TRN2u ends, from the design it has then (56000).
- After the silence it designs again, "one rate down on silence rrn" (54666), for its receiver alone.
- The CPu it then sends, rebuilt when it gets our SUVd after the silence (`recivedPartTwoSilenceRrnSUV`: we send it, and it takes it), still has the parameters of the first design.

That is the library's own behaviour, whatever the digital modem does, and its new constellation is not in anything it sends. The call goes on after its retrain.

The library's `$!$` log lines are `edprintf`'s: each byte is two characters, a nibble plus a key that goes round 4 6 2 7 1 9 3 5 8 7, plus '0'. `test/smartlink_log.py` decodes them.

`NET_NOISE_QUIET=<ms>` stops netsim's noise when the signal it goes onto has been silent that long, to try this on a clean line.

A renegotiation of the analogue modem's in the middle of our fast parameter exchange gives the exchange up, and we run phase 4 from its TRN2u instead. The original library starts one like that when noise keeps it from reading our CPd.

## The Agere modem and the analogue loop

The Agere modem's V.90 training failed at some line delays, in a pattern that repeats every 48 samples (its codec's blocks), when the line handed it our PCM values as they are, sample for sample. Through an analogue loop (netsim's `NET_LOOP`: a fraction of a sample of delay and a high pass) it trains at every delay. The Agere tests use that, with 0.45 of a sample.

Not 0.5: with no clock offset and no noise, our sample instants then fall exactly between the Agere's samples. At some alignments of the call to its codec's blocks (`AGERE_SYNC_PHASE` 96 and 288 of 4800) it judges TRN2d badly and takes 33333 bit/s downstream instead of 53333. At 0.45 and 0.55 every alignment gets 53333 or 54666, and at 0.3 it gets 52000. The signal is the same, so this is the Agere receiver's tie, not ours. `agere_v90_phase96` keeps that alignment.
