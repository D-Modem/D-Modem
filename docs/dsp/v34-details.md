# V.8 and V.34: technical details

This page gives the technical details for [V.8 and V.34](v34.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

## Where the code is

| File | What it holds |
|---|---|
| `dsp/dp_v34.cpp` | the start-up: V.8, phase 2, the echo canceller, retrains, renegotiations, modem-on-hold |
| `dsp/v34_modem.cpp` | the primary channel's training signals and its receiver |
| `dsp/v34_data.cpp` | data mode: shell mapping, the trellis codes, precoding |
| `dsp/v34_tables.cpp` | the constants: the superconstellation, the rates of Table 10/V.34, the PP sequence |

## V.8

V.8 is the exchange at the start of the call. In V.8, the modems agree on a modulation.

- The answering modem sends **ANSam**: 2100 Hz, amplitude modulated at 15 Hz, with a phase reversal each 450 ms.
- The calling modem sends **CM**. The answerer replies with **JM**, and the caller ends with **CJ**. The three signals go on V.21 (300 bit/s FSK).
- Our CM offers V.34, V.32bis, V.22bis, V.23 and V.21. Our JM offers the modulations of this list that the CM of the remote modem has.

### When V.34 is not chosen

- **Calls.** The modem answers a JM without V.34 with CJ. After 75 ms without signal (8.1.2/V.8), the call continues in the best modulation that the JM has, in the order V.32bis, V.22bis, V.23, V.21. An FSK caller starts its carrier at that point, and the answerer waits for it. The PCTel modem accepts CJ only with the gap after it.
- **Answers.** A CM without V.34 gets its JM. After CJ and 75 ms, the call continues in the best modulation of that CM, on its pump, without the answer tone and without automode.
- If a CM has none of our modulations, or if a V.32 calling modem sends its 1800 Hz AA tone, the modem gives the call to the V.32bis pump.

### A forced V.21 caller

With `AT+MS=21`, a calling modem gives a 2100 Hz answer tone to the V.8 pump (`dp_v8_first()` in `dsp/dsp_glue.cpp`):

- If the tone is ANSam, the pump sends a CM with only V.21. A JM that has V.21 gets CJ, and the call continues in V.21. Our carrier starts after the 75 ms gap. We connect on 400 ms of the mark of the answerer (its JM came on the same channel).
- In these conditions, the call goes back to the V.21 caller, as before the modem tried V.8: plain ANS (a tone that ends early, or a tone without the AM), no JM, or a JM without V.21.
- `DSP_V8_LOWSPEED=0` always gives the call back.

Only V.21 does this. The other low speed modulations connect without V.8. After V.8, the PCTel and HSF modems, as the answerer, use opposite sides of the two rates of V.23.

### Automode fallback without V.8

As in V.32bis Annex A, a caller that gets no answer falls back one step at a time. Each step occurs after 3 s:

1. V.32bis falls back to V.22bis.
2. V.22bis falls back to V.21 after 3 s more of USB1 (the unscrambled ones of V.22) with no answer.
3. V.21 falls back to V.23.
4. V.23 falls back to Bell 103.

FSK callers wait for the mark of the answerer.

A Bell 103 caller usually identifies the unscrambled ones of V.22 (2250 Hz) as the 2225 Hz mark of its answerer. It answers with its own mark (1270 Hz). 100 ms of that tone, in all of these stages, changes to Bell 103 immediately.

## Calling automode

- A 2225 Hz answer tone tells that the answerer is a Bell answerer. The modem distinguishes it from the 2250 Hz unscrambled ones of V.22 over 200 ms. The caller operates Bell 212A. If no scrambled ones come back in 1.5 s, it operates Bell 103.
- After a 2100 Hz tone without ANSam, the V.32bis caller goes to V.22bis on unscrambled ones, to V.21 on a 1650 Hz mark, or to V.23 on a 1300 Hz mark.
- FSK callers do a check that the mark is at their own frequency, because the mark of V.21 demodulates as a V.23 mark.

## Phase 2

Phase 2 exchanges the INFO0 and INFO1 frames, measures the round trip delay and probes the line.

- **Control channel.** INFO0 and INFO1 go on the 600 baud DPSK control channel. The receiver looks for the frames, by their sync and CRC, at all sampling phases, not only at the timing that it follows. This is important because the INFO1 of the remote modem comes at a new phase after its probing signals, and the timing can slip at its start.
- **Round trip delay.** Tones A and B and their phase reversals measure it.
- **Line probing.** L1 and L2 are 21 tones at multiples of 150 Hz (Table 17/V.34: 150 to 3750 Hz, without 900, 1200, 1800 and 2400 Hz). The probe gives the SNR (signal-to-noise ratio) after equalization for each symbol rate. It uses the received level of each tone and the noise between successive periods. Thus, clock offsets have no effect on it.

### Choosing the symbol rate

The answerer selects the symbol rate that gives the highest data rate at the two ends, in the limits that the INFO0c of the caller offers:

- 3429 baud only when the caller supports and allows it.
- At 3000 and 3200 baud, a carrier on which the caller can transmit: the low carrier if it can, if not the high carrier.

A caller retrains on an INFO1a that asks for an item that its INFO0c did not allow.

`DSP_V34_INFO0=<hex>[,a|,c]` sets bits 12:19 of our INFO0 (the symbol rates and carriers) on the two sides, or only on the answerer (`a`) or the caller (`c`). It is for tests.

2400 bit/s is available only at 2400 baud (Table 10/V.34). The modem does not offer a symbol rate whose SNR cannot send 4800 bit/s. Thus, a very noisy line connects at 2400 baud, and does not fail.

### Recovering a lost INFO0

When an INFO0 does not get through, the modem that did not receive it hears the tone of the other modem and repeats its own INFO0 (11.2.2/V.34, 9.2.1.2/V.90). In our code (`info0_resend()` and `info0_more()` in `dsp/dp_v34.cpp`):

- While we send our tone (B, or A when we answer), our INFO0 goes out again, back to back, in these conditions: the INFO0 of the remote modem comes again, or the tone of the remote modem comes for 500 ms without its INFO0.
- When we have the INFO0 of the remote modem, ours acknowledges it (bit 28).
- We continue to repeat until an INFO0 of the remote modem acknowledges ours, or until 250 ms go by without one. We send a maximum of 16 INFO0s.
- If an INFO0 acknowledges ours, we send ours one more time, because the HSF modem repeats its INFO0a until ours acknowledges it.
- Only the INFO0s of the remote modem that arrive 400 ms or more after our last one count. The INFO0s that it sent before ours got to it tell us nothing.
- After the quick connect of V.92, the first INFO0a of the analogue modem after the end of our INFO0d gets INFO0d one more time (refer to [V.92 quick connect](v92-details.md#v92-quick-connect)).
- After such a recovery, the answerer reverses tone A only when the INFO0s of the caller have stopped for 100 ms (tone B, 11.2.1.2.3/V.34).

Repeated INFO0s, and the signal that follows one, can look like an INFO1 frame (with a correct sync and CRC). Thus, the modem does not accept a candidate whose first 45 bits are an INFO0 frame as INFO1.

Before we added this recovery, we sent INFO0d again only after the quick connect of V.92. A remote modem that did not receive ours waited in phase 2 until the call stopped. No peer repeats its INFO0 in a call where nothing is lost.

`DSP_V34_LOSE_INFO0=[a|c]` sends our first INFO0 with an incorrect CRC bit, on the two sides, or only on the answerer or the caller. It is for tests: `V34LoseInfo0` of pump_test, and `agere_34_lose_info0`, `pctel_v90_lose_info0` and `hsf_v90_lose_info0` in interop_test. The Agere, SM56, PCTel and HSF modems, and the original library as caller, all recover with us. The original library as answerer accepts no INFO0c after a bad one. Its log has "info0 CRC not received properly" for each.

## Phases 3 and 4

- **The sequences.** S, S-bar, PP, TRN, J, J', MP and E, as the original library sends them. TRN starts with a cleared scrambler, because the receiver of the original library trains on a copy of it.
- **The equalizer.** The receiver solves it by least squares over 1500 TRN symbols (`LS_LEN` in `dsp/v34_modem.cpp`). LMS is much too slow for the accuracy that 33600 bit/s needs on a band limited line.
- **The clock offset.** Two solutions in phase 3 and one in phase 4 measure the sample clock offset from the drift of the delay of the equalizer.
- **Timing.** From then on, the timing is decision directed. The error is calculated against the slope of the equalizer output. This operates for all constellations.

## Data mode

- **What is implemented.** Shell mapping, the superconstellation with expanded shaping, differential encoding, the 16, 32 and 64 state 4D trellis codes, high and low mapping frames, and the superframe sync inversions. This includes all rates and symbol rates of Table 10/V.34, with and without the auxiliary channel.
- **Trellis code choice.** We ask for the 64 state code (`V34_TRELLIS`). The original library sends the code that we ask for. It asks for the 16 state code itself.
- **Matching the original library.** The trellis output of a 4D symbol is the low bit of the encoder state before that symbol. The inversion pattern starts at its bit 2J - 2. The 32 and 64 state encoders use the same inputs as the `Convolve32` and `Convolve64` tables of the original library.
- **Viterbi decoder.** The receiver uses a Viterbi decoder over the states of the code, with a decision delay of 32 4D symbols (`V34_TB`). For the equalizer and the timing, it still uses the nearest point.
- **Coding gain.** The decoder gains 3 to 4 dB over decisions on the nearest points. With the 16 state code, the bit error rate stays below 1e-5 from an SNR of 3 dB for each bit for each symbol plus 4 dB, at all symbol rates. The 32 and 64 state codes need 0.5 to 1 dB less. The rate selection asks for 2 dB more than that.

### Precoding (9.6.2/V.34)

- The transmitter precodes with the coefficients that the MP of the remote modem asks for.
- The receiver asks for precoding when it gains 1 dB or more (`V34_PRECODE_MIN`). The coefficients are the third order linear predictor of the error of the equalizer after training. Thus, the error through 1 + H(z) is white.
- In data mode, the equalizer of the receiver then includes 1 + H(z). It makes decisions on the channel output points y(n) over the full grid. It uses a copy of the precoder of the remote modem on the decided points to recover u(n).
- The original library precodes when we ask it to. Its signal decodes without errors with the coefficients that we calculate and also with stronger ones.
- `DSP_V34_PRECODE_MIN=<dB>` asks for precoding from that gain (a test; the default is 1 dB).

### Non-linear encoding (9.7/V.34)

- When the MP of the remote modem asks for it (Θ = 0.3125), the transmitter warps the precoded points. The Agere modem asks for it. The original library does not.
- The transmitter then scales the warped points back to the power that they had without it, as the note in 10.1.3/V.34 tells. The scale is the square root of the mean power of the constellation divided by the mean power of its warped points. The modem calculates it in the same way as `frame_power` calculates the mean power.
- The Agere receiver removes that scale with a table of the same values (to 1/512) before its non-linear decoder. Without the scaling, it decodes nothing above 24000 bit/s.
- Our receiver can reverse it. It applies the inverse of the warp (Newton's method on the magnitude) before its decisions. With precoding, it adds back the p(n) of the precoder from the soft history of x(n). Its equalizer adapts against the warped decision.
- By default, our receiver does not ask for it, because over a G.711 line it measured no gain. `DSP_V34_NONLINEAR=1` asks for it in each MP, and `0` never asks for it (tests: `V34NonLinear` of pump_test over µ-law, with and without precoding).

### Pre-emphasis (5.4/V.34)

- The transmitter filters its phase 3, phase 4 and data signals with the filter that the INFO1a, INFO1c or INFOh of the remote modem asks for. Each filter is a 47 tap linear phase FIR designed on the template of Figures 1 and 2. Indices 1 to 5 tilt the band linearly in dB. Indices 6 to 10 lift its top. The filter keeps the power that the signal has without it.
- At each symbol rate, the probing of the receiver calculates the SNR after equalization that each filter gives (the harmonic mean over the probe tones in the band). It asks for the best filter when it gains 0.5 dB or more over no filter.
- On a line whose loss increases with the frequency, the 3000 Hz tone of L1 can arrive 7 dB below its share. The modem still accepts it as L1.
- `DSP_V34_PREEMPH=<index>` asks for that filter where the rate allows it, and `0` for no filter (tests: `V34PreEmphasis` of pump_test on a sloped line, and `V34PreEmphasisIndex`).

### Auxiliary channel (8.3/V.34)

- When the two MPs set bit 28, each direction sends 200 bit/s more: the rates of Table 10/V.34 for N × 2400 + 200 bit/s (`v34_aux_rates`). W bits in each data frame (8, or 7 at 2743 and 3429 baud) go in I1 of the first 4D symbol of the mapping frames that the pattern of Table 9/V.34 selects, unscrambled.
- No user of it is available now. When it is enabled, we send ones, as an idle clear secondary channel does (Annex A).
- `DSP_V34_AUX=1` sets bit 28. `2` also sends a PRBS (x^15 + x^14 + 1) in it, and logs the bits received and their errors at the end of the call (tests: `V34Aux` of pump_test). Without these settings, it stays disabled.

### Other options

- **16 point training.** A J of 0x89B0 from the remote modem asks for TRN, MP and E in the 16 point constellation in phase 4. (MP then has 4 bits for each symbol, and E is 5 symbols.) The receiver accepts the two types of J, and the transmitter follows the J of the remote modem. We send the 4 point J.
- **Asymmetric rates.** The modems keep the two rates only when the two MPs set bit 50. If not, the two directions use the lower rate.

## Echo canceller

The echo canceller (`V34Echo` in `dsp/dp_v34.cpp`) is a real pass band NLMS filter on the line samples. Its structure is the same as that of the echo canceller in the V.34 of Agere (`agrmodemlib.o`, which we read for reference):

- A **near end** part over the last 27 ms of our signal (`EC_NEAR_LEN`, 256 samples).
- A **far end** part over 27 ms around the echo from the hybrid of the other end (`EC_FAR_LEN`).
- One error, which the two parts share.

**Training.** The two parts train while our own phase 3 sequences go out and the remote modem is silent. The caller waits until the J of the answerer stops. The step size starts large and decreases: 0.5 for the first 300 ms, 0.2 up to 600 ms, then 0.05.

**Freezing.** When the error is much more than the error that the training left (the first signal of the remote modem, before we have seen it), the parts stop adapting. They also do not adapt in data mode. The canceller does not count those samples in the echo that it removed. When it counted them, the Su of the remote modem made a canceller that had removed 35 dB look like a canceller of 3 dB, and the canceller disabled itself.

**Judging.** At the end of the training, the canceller measures how much echo it removed. If this is less than 6 dB, the canceller is bypassed, because it can only add noise. (This occurs if there is no echo to cancel, as on a 4-wire network, or if the remote modem did not stay silent.) `DSP_EC=0` always bypasses it (for tests). As the analogue modem of V.90, it also solves the near part by least squares on the stored training (`ls_solve()`). It keeps that solution when it removes more echo than NLMS did.

**Retrains.** After a retrain, it trains again from the start.

**Finding the far echo.** The modem correlates the residual of the near part with our scrambled signal at all delays up to 1.7 s (`EC_HIST`). Over VoIP, the echo comes back after the network delay.

**Phase 2.** The tone detectors of phase 2 listen exactly on a bin of the 64 sample probe period, where the echo of our own L1/L2 has nothing. The reversal detector does not use the symbol timing.

## Retrains

In data mode, a modem goes back to phase 2 in these conditions:

- It loses the signal of the remote modem for 30 ms.
- It hears the tone A or B of the remote modem in place of data.
- Its mse (mean squared error) stays above the value that all rates need for a second (only from 5 s into data mode).

A retrain starts at the tones: 70 ms of silence, then tone A or B. The INFO0s do not change. The modems probe the line again and train again. The link (V.42) continues at the new rates without a new CONNECT. There is no INFO0 from which to find the symbol timing of the control channel. Thus, the receiver gets it from the tone reversals.

`DSP_V34_RETRAIN=<ms>[a|c]` makes the answerer (`a`), the caller (`c`) or the two modems start a retrain after that time in data mode (a test).

### Tone A against the 1800 Hz tone

The caller of the retrain sends tone B and waits for tone A (2400 Hz) or INFO0a. The Agere, Broadcom and HSF modems answer with tone A plus an 1800 Hz tone of the same power. The tone detector (`rt_tone()`) removes the 1800 Hz tone (`mate_g`) from the power that tone A must be above. Without this, it identified their answer as no tone A, and the retrain failed.

### Early reversals

The caller does not accept a reversal that it sees before tone A, or in 20 ms after its start, as the reversal of the answerer (`rt_since`). The change of the data of the remote modem to tone A can look like a reversal. When the caller accepted it as the reversal of the answerer, it stopped tone B while the answerer still waited for it (our V.90 analogue modem waits 200 ms for it), until the two timed out. This occurred in about one call in five of `open_v90a_retrain`.

## Retrains and renegotiations on request

The core asks for one with `MDMPRM_LINK_REQ`: 1 for a retrain, 2 for a rate renegotiation. The DTE starts them with:

- `ATO1` (retrain) and `ATO3` (renegotiation), from online command mode (`Modem::at_O()` in `core/at.cpp`).
- The `<EM><rtn>` and `<EM><rrn>` of V.80 in synchronous access mode.

The pump polls the request each 160 samples in data mode (`link_req_poll()`, as the modem-on-hold request). It sets it back to 0 when it starts. Thus, the core counts the retrain as a retrain of the DTE. The core accepts the request only online, and only with V.34, V.90 or V.92 (`Modem::link_req()` in `core/modem.cpp`).

- A renegotiation asks for the rate that the mse gives (`reneg_rate()`), not more than `MDMPRM_MAX_RX_RATE` (the `<rx>` of the `<EM><rate>` of V.80).
- In V.90 and V.92 calls, it is their own renegotiation (9.6/V.90, 9.8/V.92), as the digital or the analogue modem.

### Rate caps

- The highest rate of `+MS` (`MDMPRM_MAX_RATE`) sets the limit of the rates at which our MP offers to send.
- `MDMPRM_MAX_RX_RATE` sets the limit of the rates at which it offers to receive. It is the highest rate of `+MS` unless V.80 set a value. It is never below the lowest rate of the symbol rate.
- The call operates at the lower of the MP rates of the two modems.

## Loop timing

The caller sends its symbols on the clock of the answerer:

- It follows the offset that its receiver measures from the signal of the remote modem (`clock` in the log).
- The transmit clock moves to it by a maximum of 2e-7 for each sample.
- It starts when the timing is fixed and the offset is more than 80 ppm, because a smaller offset is in the tolerance of all modems. Then it stays on for the remaining part of the call.
- The connect message tells this (`on the remote's clock`).

The codec of a SmartUSB56 operates about 140 ppm off. Without loop timing, a USR Courier as answerer lost our signal after a slip of one symbol (2 s) into data.

`DSP_V34_LOOP_TIMING=0` disables it. The answerer keeps its own clock. Only one side follows, because two sides would follow each other.

## Rate renegotiation (11.6/V.34)

**Decrease.** A modem asks for a lower rate when its data mode mse stays above the value for its receiving rate with a margin for 300 ms. (The margin is 0.2, in units of the spacing of the signal points, `V34_RN_DOWN`.) It does not do this in the first 2 s of data mode. The sequence is:

1. S for 128 symbols, then S-bar.
2. TRN.
3. MP with the new rate that it can receive, then MP'.
4. E, and data at the new rates.

The equalizers do not change. The equalizer from before the 1 + H(z) of the precoder goes back in: the best equalizer of the training, or, if its mse never became that good, the equalizer into which 1 + H(z) went. It never puts 1 + H(z) on the last equalizer.

The rate decreases by the number of steps that the mse asks for (about 3 dB for each bit for each symbol for each step).

**Increase.** It increases one step at a time, up to the rate at which it started, when the mse has room for the next rate for 10 s.

**Answers.** A modem that hears the S of the remote modem in data mode answers in the same way, with the rate that its own mse gives.

**Retrain in place of a renegotiation.** The modem retrains and does not renegotiate in these conditions:

- The mse became worse by 6 dB or more (four times the good mse). The line has changed, most possibly in the two directions.
- It already receives at the lowest rate of the symbol rate (4800 bit/s above 2400 baud, and the upstream of V.90).
- The renegotiation does not end with E in 3 s plus three round trips.
- The remote modem sends its tone.

**Cleardown.** An MP with no rates is a cleardown (11.7/V.34), and the call ends. We start one when the core hangs up in data mode: S for 128 T, S-bar for 16 T, then MP with no rates until the MP and MP' of the remote modem are in. We answer the cleardown of the remote modem in the same way. The core waits for it (up to 2.5 s plus three round trips) before it drops the line. ATI11 shows "Remote cleardown" when the remote modem started it. With V.42, the two modems hang up when the DISC arrives, and their two cleardowns meet. `DSP_V34_CLEARDOWN=0` drops the line without a cleardown (tests: `V34Cleardown` of core_test).

**Other modems.** The original library and the Agere modem answer our renegotiations (the original library with a type 0 MP). The Agere modem starts its own renegotiations on errors.

`DSP_V34_RENEG=<ms>:<N>[a|c]` makes the answerer (`a`), the caller (`c`) or the two modems start one after that time in data mode, to receive at N × 2400 bit/s (a test).

## Not implemented

- A user of the auxiliary channel (the secondary channel of Annex A).
- The selection of non-linear encoding from the line (we ask for it only when a setting tells us to).

## Half-duplex (clause 12, for fax)

`v34_hdx_create()` (`v34.h`) makes a half-duplex V.34 modem for T.30
Annex F fax. Its host gives and receives the bits through `V34HdxHost`.
For class 2 and 2.0, the host is the T.30 machine (`t30_v34.cpp`; refer
to [the fax details](fax-details.md)). In half-duplex mode, V.8 never
falls back to a different pump. The T.30 machine continues with V.21 in
its place.

The call has these steps:

1. **V.8.** CM and JM send the half-duplex bit (0x80 in the first
   modulation octet) and the T.30 call function: 0x81 to send, 0xA1 to
   poll. The modem that sends the pages is the *source*. The other modem
   is the *recipient*.
2. **Phase 2.** The source and the recipient probe in one direction
   only. INFOh gives the source the symbol rate, the carrier, the
   pre-emphasis and the training length (12.3/V.34).
3. **Phase 3.** The source sends S, S-bar, PP and TRN, and the recipient
   trains on them.
4. **The control channel** (`v34_cc.cpp`, 10.2.4/V.34) operates at
   1200 bit/s. It starts with PPh, ALT, MPh and E (12.4/V.34). It comes
   back with Sh, S-bar h, ALT and E after each turn of the primary
   channel (12.6/V.34).
5. **The primary channel.** `v34_hdx_primary()` disables the control
   channel (4 symbols of ones). After 70 ms, the source sends S, S-bar,
   PP, two frames of ones (B1), then the bits of the host until the host
   returns -1, then 35 ms of ones (12.5/V.34). The recipient finds S with
   the receiver that it trained in phase 3. It accepts 20 ms without the
   signal as the end.

The receiver of the control channel keeps its gain, timing and equaliser
while the primary channel has the line. It adapts only on decisions that
look correct. Thus, the signal of the primary channel does not disturb
it.

**Tests.** `V34Hdx.CallerSends` and `V34Hdx.AnswererSendsToAPoll` operate
two modems back to back: messages on the control channel in the two
directions, and two pages, each about 3 s at 31200 bit/s.

**Not implemented now:** class 1 fax on V.34 (the V.34 mode of T.31), and
a change of the rate of the primary channel between pages.
