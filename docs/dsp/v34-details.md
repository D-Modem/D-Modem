# V.8 and V.34: technical details

This is the technical companion to [V.8 and V.34](v34.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

## Where the code is

| File | What it holds |
|---|---|
| `dsp/dp_v34.cpp` | the start-up: V.8, phase 2, the echo canceller, retrains, renegotiations, modem-on-hold |
| `dsp/v34_modem.cpp` | the primary channel's training signals and its receiver |
| `dsp/v34_data.cpp` | data mode: shell mapping, the trellis codes, precoding |
| `dsp/v34_tables.cpp` | the constants: the superconstellation, the rates of Table 10/V.34, the PP sequence |

## V.8

V.8 is the exchange at the start of the call in which the modems agree on a modulation.

- The answering modem sends **ANSam**: 2100 Hz, amplitude modulated at 15 Hz, with a phase reversal every 450 ms.
- The calling modem sends **CM**, the answerer replies with **JM**, and the caller ends with **CJ**. All three are sent on V.21 (300 bit/s FSK).
- Our CM offers V.34, V.32bis, V.22bis, V.23 and V.21. Our JM offers those of them that the remote's CM has.

### When V.34 is not chosen

- **Calling.** A JM without V.34 is answered with CJ. After 75 ms without signal (8.1.2/V.8), the call goes on in the best modulation the JM has, in the order V.32bis, V.22bis, V.23, V.21. An FSK caller starts its carrier at that point and the answerer waits for it. The PCTel modem takes CJ only with the gap after it.
- **Answering.** A CM without V.34 gets its JM. After CJ and 75 ms, the call goes on in the best modulation of that CM, on its pump, without the answer tone and without automode.
- A CM with none of our modulations, or a V.32 calling modem (its 1800 Hz AA tone), hands the call to the V.32bis pump.

### A forced V.21 caller

With `AT+MS=21`, a calling modem hands a 2100 Hz answer tone to the V.8 pump (`dp_v8_first()` in `dsp/dsp_glue.cpp`):

- If the tone is ANSam, the pump sends a CM of V.21 alone. A JM that has V.21 gets CJ, and the call goes on in V.21. Our carrier starts after the 75 ms gap, and we connect on 400 ms of the answerer's mark (its JM came on the same channel).
- Plain ANS (a tone that ends early, or one without the AM), no JM, or a JM without V.21 hands the call back to the V.21 caller, as before V.8 was tried.
- `DSP_V8_LOWSPEED=0` always hands it back.

Only V.21 does this. The other low speed modulations connect without V.8. After V.8, the PCTel and HSF modems, as the answerer, take opposite sides of V.23's two rates.

### Automode fallback without V.8

As in V.32bis Annex A, a caller that gets no answer falls back step by step, each step after 3 s:

1. V.32bis falls back to V.22bis.
2. V.22bis falls back to V.21 after 3 s more of unanswered USB1 (V.22's unscrambled ones).
3. V.21 falls back to V.23.
4. V.23 falls back to Bell 103.

FSK callers wait for the answerer's mark.

A Bell 103 caller usually takes V.22's unscrambled ones (2250 Hz) for its answerer's 2225 Hz mark, and answers with its own mark (1270 Hz). 100 ms of that tone, in any of these stages, switches to Bell 103 at once.

## Calling automode

- A 2225 Hz answer tone means a Bell answerer. It is told apart from V.22's 2250 Hz unscrambled ones over 200 ms. The caller runs Bell 212A, and Bell 103 when no scrambled ones come back within 1.5 s.
- After a 2100 Hz tone without ANSam, the V.32bis caller goes to V.22bis on unscrambled ones, to V.21 on a 1650 Hz mark, or to V.23 on a 1300 Hz mark.
- FSK callers check that the mark is at their own frequency, because V.21's mark demodulates as a V.23 one.

## Phase 2

Phase 2 exchanges the INFO0 and INFO1 frames, measures the round trip delay and probes the line.

- **Control channel.** INFO0 and INFO1 go on the 600 baud DPSK control channel. The receiver looks for the frames, by their sync and CRC, at every sampling phase, not only at the timing it follows. This matters because the remote's INFO1 comes at a new phase after its probing signals, and the timing could slip at its start.
- **Round trip delay.** Tones A and B and their phase reversals measure it.
- **Line probing.** L1 and L2 are 21 tones at multiples of 150 Hz (Table 17/V.34: 150 to 3750 Hz, without 900, 1200, 1800 and 2400 Hz). The probe gives the SNR (signal-to-noise ratio) after equalization for each symbol rate. It uses the received level of each tone and the noise between successive periods, so it is insensitive to clock offsets.

### Choosing the symbol rate

The answerer picks the symbol rate that allows the highest data rate at both ends, within what the caller's INFO0c offers:

- 3429 baud only when the caller supports and allows it;
- at 3000 and 3200 baud, a carrier the caller can transmit on: the low one if it can, else the high one.

A caller retrains on an INFO1a that asks for something its INFO0c ruled out.

`DSP_V34_INFO0=<hex>[,a|,c]` sets bits 12:19 of our INFO0 (the symbol rates and carriers) on both sides, or only on the answerer (`a`) or the caller (`c`). It is for tests.

2400 bit/s exists only at 2400 baud (Table 10/V.34). A symbol rate whose SNR does not carry even 4800 bit/s is not offered, so a very noisy line connects at 2400 baud rather than not at all.

### Recovering a lost INFO0

When an INFO0 does not get through, the modem that missed it hears the other's tone and repeats its own INFO0 (11.2.2/V.34, 9.2.1.2/V.90). In our code (`info0_resend()` and `info0_more()` in `dsp/dp_v34.cpp`):

- While we send our tone (B, or A when answering), either the remote's INFO0 coming again, or the remote's tone for 500 ms without its INFO0, makes ours go out again, back to back.
- Once we have the remote's INFO0, ours acknowledges it (bit 28).
- We keep repeating until an INFO0 of the remote's acknowledges ours, or 250 ms go by without one. We send at most 16 INFO0s.
- An INFO0 that acknowledges ours gets ours once more, because the HSF modem repeats its INFO0a until ours acknowledges it.
- Only the remote's INFO0s that arrive 400 ms or more after our last one count. Those it sent before ours reached it tell us nothing.
- After V.92's quick connect, the analogue modem's first INFO0a after our INFO0d is over gets INFO0d once more (see [V.92 quick connect](v92-details.md#v92-quick-connect)).
- After such a recovery, the answerer reverses tone A only once the caller's INFO0s have stopped for 100 ms (tone B, 11.2.1.2.3/V.34).

Repeated INFO0s, and what follows one, can pass for an INFO1 frame (with a correct sync and CRC). So a candidate whose first 45 bits are an INFO0 frame is not taken for INFO1.

Before this recovery was added, we sent INFO0d again only after V.92's quick connect, and a remote that had missed ours waited in phase 2 until the call gave up. No peer repeats its INFO0 in a call where nothing is lost.

`DSP_V34_LOSE_INFO0=[a|c]` sends our first INFO0 with a wrong CRC bit, on both sides, or only on the answerer or the caller. It is for tests: pump_test's `V34LoseInfo0`, and `agere_34_lose_info0`, `pctel_v90_lose_info0` and `hsf_v90_lose_info0` in interop_test. The Agere, SM56, PCTel and HSF modems, and the original library calling, all recover with us. The original library answering takes no INFO0c after a bad one; its log has "info0 CRC not received properly" for each.

## Phases 3 and 4

- **The sequences.** S, S-bar, PP, TRN, J, J', MP and E, as the original library sends them. TRN starts with a cleared scrambler, because the original's receiver trains on a copy of it.
- **The equalizer.** The receiver solves it by least squares over 1500 TRN symbols (`LS_LEN` in `dsp/v34_modem.cpp`). LMS is far too slow for the accuracy that 33600 bit/s needs on a band limited line.
- **The clock offset.** Two solutions in phase 3 and one in phase 4 measure the sample clock offset from the drift of the equalizer's delay.
- **Timing.** From then on the timing is decision directed: the error is taken against the slope of the equalizer output, which works for any constellation.

## Data mode

- **What is implemented.** Shell mapping, the superconstellation with expanded shaping, differential encoding, the 16, 32 and 64 state 4D trellis codes, high and low mapping frames, and the superframe sync inversions. This covers every rate and symbol rate of Table 10/V.34 except the auxiliary channel ones.
- **Trellis code choice.** We ask for the 64 state code (`V34_TRELLIS`). The original library sends what it is asked for, and asks for the 16 state code itself.
- **Matching the original library.** The trellis output of a 4D symbol is the low bit of the encoder state before that symbol, and the inversion pattern starts at its bit 2J - 2. The 32 and 64 state encoders take the same inputs as the original's `Convolve32` and `Convolve64` tables.
- **Viterbi decoder.** The receiver runs a Viterbi decoder over the code's states, with a decision delay of 32 4D symbols (`V34_TB`). For the equalizer and the timing it still takes the nearest point.
- **Coding gain.** The decoder gains 3 to 4 dB over deciding on the nearest points. With the 16 state code the bit error rate stays below 1e-5 from an SNR of 3 dB per bit per symbol plus 4 dB, at every symbol rate. The 32 and 64 state codes need 0.5 to 1 dB less. The rate selection asks for 2 dB more than that.

### Precoding (9.6.2/V.34)

- The transmitter precodes with the coefficients that the remote's MP asks for.
- The receiver asks for precoding when it gains at least 1 dB (`V34_PRECODE_MIN`). The coefficients are the third order linear predictor of the equalizer's error after training, so that the error through 1 + H(z) is white.
- In data mode the receiver's equalizer then includes 1 + H(z). It decides on the channel output points y(n) over the whole grid, and runs a copy of the remote's precoder on the decided points to recover u(n).
- The original library precodes when asked. Its signal decodes without errors both with the coefficients we compute and with stronger ones.
- `DSP_V34_PRECODE_MIN=<dB>` asks for precoding from that gain on (a test; the default is 1 dB).

### Non-linear encoding (9.7/V.34)

- When the remote's MP asks for it (Θ = 0.3125), the transmitter warps the precoded points. The Agere modem asks for it; the original library does not.
- The transmitter then scales the warped points back to the power they had without it, as the note in 10.1.3/V.34 asks. The scale is the square root of the constellation's mean power over that of its warped points, computed the way `frame_power` computes the mean power.
- The Agere receiver undoes that scale from a table of the same values (to 1/512) before its non-linear decoder. Without the scaling it decodes nothing above 24000 bit/s.
- Our receiver does not ask for non-linear encoding.

### Other options

- **16 point training.** A J of 0x89B0 from the remote asks for TRN, MP and E in the 16 point constellation in phase 4 (MP then takes 4 bits per symbol, and E is 5 symbols). The receiver accepts both kinds of J, and the transmitter follows the remote's. We send the 4 point J.
- **Asymmetric rates.** Both rates are kept only when both MPs set bit 50. Otherwise both directions take the lower one.
- **Pre-emphasis.** We neither ask for it nor apply it when the remote asks for it.

## Echo canceller

The echo canceller (`V34Echo` in `dsp/dp_v34.cpp`) is a real pass band NLMS filter on the line samples. It is structured like the one in Agere's V.34 (`agrmodemlib.o`, read for reference):

- a **near end** part over the last 27 ms of our signal (`EC_NEAR_LEN`, 256 samples);
- a **far end** part over 27 ms around the echo from the other end's hybrid (`EC_FAR_LEN`);
- one error, shared by both parts.

**Training.** Both parts train while our own phase 3 sequences go out and the remote is silent; the caller waits for the answerer's J to stop. The step size starts large and gets smaller: 0.5 for the first 300 ms, 0.2 up to 600 ms, then 0.05.

**Freezing.** Once the error is well above what the training has left (the remote's first signal, before we have seen it), the parts stop adapting, and they do not adapt in data mode either. Those samples are not counted in the echo the canceller took away. When they were counted, the remote's Su made a canceller that had taken 35 dB away look like one of 3 dB, and it switched itself off.

**Judging.** At the end of the training the canceller measures how much echo it took away. Under 6 dB (no echo to cancel, as on a 4-wire network, or a remote that did not stay silent) it is bypassed, since it would only add noise. `DSP_EC=0` bypasses it in any case (for tests). As V.90's analogue modem, it also solves the near part by least squares on the stored training (`ls_solve()`) and keeps that solution when it takes more away than NLMS did.

**Retrains.** After a retrain it trains again from the start.

**Finding the far echo.** The near part's residual is correlated with our scrambled signal at every delay up to 1.7 s (`EC_HIST`). Over VoIP the echo comes back after the network delay.

**Phase 2.** Phase 2's tone detectors listen exactly on a bin of the 64 sample probe period, where the echo of our own L1/L2 has nothing. The reversal detector does not depend on the symbol timing.

## Retrains

In data mode, a modem goes back to phase 2 when:

- it loses the remote's signal for 30 ms;
- it hears the remote's tone A or B instead of data; or
- its mse (mean squared error) stays above what any rate needs for a second (only from 5 s into data mode).

A retrain starts at the tones: 70 ms of silence, then tone A or B. The INFO0s stand. The modems probe the line again and train anew, and the link (V.42) carries on at the new rates without a new CONNECT. As there is no INFO0 to find the control channel's symbol timing from, the receiver takes it from the tone reversals.

`DSP_V34_RETRAIN=<ms>[a|c]` makes the answerer (`a`), the caller (`c`) or both start a retrain after that long in data mode (a test).

### Tone A against the 1800 Hz tone

The retrain's caller sends tone B and waits for tone A (2400 Hz) or INFO0a. The Agere, Broadcom and HSF modems answer with tone A plus an 1800 Hz tone of the same power. The tone detector (`rt_tone()`) takes the 1800 Hz tone (`mate_g`) out of the power that tone A has to stand above. Without that, it took their answer for no tone A and the retrain failed.

### Early reversals

A reversal that the caller sees before tone A, or within 20 ms of its start, is not taken as the answerer's (`rt_since`). The remote's data going over to tone A can look like a reversal. When the caller took it for the answerer's reversal, it stopped tone B while the answerer still waited for it (our V.90 analogue modem waits 200 ms of it), until both timed out. That happened in about one call in five of `open_v90a_retrain`.

## Retrains and renegotiations on request

The core asks for one with `MDMPRM_LINK_REQ`: 1 for a retrain, 2 for a rate renegotiation. The DTE triggers them with:

- `ATO1` (retrain) and `ATO3` (renegotiation), from online command mode (`Modem::at_O()` in `core/at.cpp`);
- V.80's `<EM><rtn>` and `<EM><rrn>` in synchronous access mode.

The pump polls the request every 160 samples in data mode (`link_req_poll()`, like the modem-on-hold request) and sets it back to 0 when it starts, so that the core counts the retrain as the DTE's. The core accepts the request only online, and only with V.34, V.90 or V.92 (`Modem::link_req()` in `core/modem.cpp`).

- A renegotiation asks for the rate that the mse calls for (`reneg_rate()`), no more than `MDMPRM_MAX_RX_RATE` (the `<rx>` of V.80's `<EM><rate>`).
- In V.90 and V.92 calls it is their own renegotiation (9.6/V.90, 9.8/V.92), as the digital or the analogue modem.

### Rate caps

- `+MS`'s highest rate (`MDMPRM_MAX_RATE`) caps the rates our MP offers to send at.
- `MDMPRM_MAX_RX_RATE` caps the rates it offers to receive at. It is `+MS`'s highest unless V.80 set one, and never below the symbol rate's lowest.
- The call runs at the lower of the two modems' MP rates.

## Loop timing

The caller sends its symbols on the answerer's clock:

- It follows the offset that its receiver measures from the remote's signal (`clock` in the log).
- The transmit clock moves towards it by at most 2e-7 a sample.
- It starts once the timing is fixed and the offset has passed 80 ppm, since less is within every modem's tolerance. It then stays on for the rest of the call.
- The connect message says so (`on the remote's clock`).

A SmartUSB56's codec runs about 140 ppm off. A USR Courier answering lost our signal a symbol's slip (2 s) into data without loop timing.

`DSP_V34_LOOP_TIMING=0` turns it off. The answerer keeps its own clock: only one side follows, as two would chase each other.

## Rate renegotiation (11.6/V.34)

**Going down.** A modem whose data mode mse stays above what its receiving rate runs at with a margin (0.2, in units of the signal points' spacing, `V34_RN_DOWN`) for 300 ms asks for a lower rate. It does not do this in the first 2 s of data mode. The sequence is:

1. S for 128 symbols, then S-bar;
2. TRN;
3. MP with the new rate it can receive, then MP';
4. E, and data at the new rates.

The equalizers stay as they are. The one from before the precoder's 1 + H(z) goes back in: the training's best, or, when its mse never got that good, the one that 1 + H(z) went into. It never puts 1 + H(z) on top of the last one.

The rate goes down by as many steps as the mse asks for (about 3 dB per bit per symbol for each step).

**Going up.** It goes back up one step at a time, up to the rate it started with, once the mse has left room for the next rate for 10 s.

**Answering.** A modem that hears the remote's S in data mode answers the same way, with the rate its own mse calls for.

**Retraining instead.** The modem retrains instead of renegotiating when:

- the mse got worse by 6 dB or more (four times the good mse): the line has changed, most likely both ways;
- it already receives at the symbol rate's lowest rate (4800 bit/s above 2400 baud, and V.90's upstream);
- the renegotiation does not end with E within 3 s plus three round trips; or
- the remote sends its tone.

**Cleardown.** An MP with no rates is a cleardown (11.7/V.34), and the call ends.

**Other modems.** The original library and the Agere modem answer our renegotiations (the original library with a type 0 MP). The Agere modem starts its own on errors.

`DSP_V34_RENEG=<ms>:<N>[a|c]` makes the answerer (`a`), the caller (`c`) or both start one after that long in data mode, to receive at N × 2400 bit/s (a test).

## Not implemented

- pre-emphasis;
- non-linear decoding (we never ask for it);
- the auxiliary channel;
- starting a cleardown.
