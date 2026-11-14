# V.90 analogue modem: technical details

This page gives the technical data for [V.90 and V.92: the analogue modem](v90a.md), which is the overview in plain English. Read the overview first. This page gives all the data: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

The open DSP has V.90 as the **digital modem** (refer to [V.90](v90.md)):
the end of the call on the 8000 Hz G.711 network. This page is about
the other end, the **analogue modem**: a softmodem on a telephone line
(dmodemd `-a`, refer to [hardware](../hardware.md)) that calls the V.90
server of an ISP.

## Terms

- **PCM**: the G.711 codes of the digital modem. They come to the
  analogue modem as voltages on its line.
- **Ucode**: a PCM code number from 0 to 127.
- **UINFO**: the Ucode of the two points of TRN1d. The analogue modem
  selects it in INFO1a.
- **DIL** (digital impairment learning): a sequence of codes, selected
  by the analogue modem, that shows what the network does to each code.
- **Data frame interval**: one of the 6 symbol positions of a data
  frame. Robbed bits and some pads have an effect on only some
  intervals.
- **T**: one symbol period. **T/2**: half of one.
- **mse**: mean square error. **rms**: root mean square, here on a
  16-bit scale.
- **NLMS**: normalised least mean squares, an adaptive filter update.

## Stages

The work was done in stages, each with its tests:

| Stage | What | State |
|---|---|---|
| A | Phases 1 and 2: V.8, INFO0a, INFO0d, INFO1d, INFO1a for V.90 | done |
| B | Phase 3: S, PP, TRN, Ja out; the PCM receiver (timing, equaliser) on TRN1d, Jd, Jd' | done |
| C | Phase 3: the DIL and what it shows (levels, robbed bits, pads) | done |
| D | Phase 4: Ri, the CPt and CP constellations, TRN2d, MP, E, B1 | done |
| E | Data mode: the PCM decoder, V.34 upstream; CONNECT; on by default on analogue lines | done (V.90 at 9600 Hz on by default; `DSP_V90A=0` removes it) |
| F | Retrains and rate renegotiation as the analogue modem (9.5.2, 9.6.2/V.90) | done (no CPs or cleardown) |
| G | V.92 as the analogue modem: V.92 with V.34 upstream and modem-on-hold | done |
| G2 | PCM upstream of V.92, phase 3: INFO1a (Table 18), the PAM transmitter, Ru to E1u | done |
| G3 | PCM upstream of V.92, phase 4 and data mode | done (on by default; `DSP_V92A_UP=0` removes it) |
| G4 | Quick connect of V.92 as the analogue modem: short phase 1 calling, short phase 2 | done (calling; `DSP_V92A_QC=0` removes QC1a, `DSP_V92_SHORT2=0` short phase 2) |
| G5 | Rate renegotiation and fast parameter exchange of PCM upstream, in the two directions | done |

## When it is used

`V34Pump::start()` (`dp_v34.cpp`) selects the mode of the analogue
modem:

- **At 9600 Hz**, with `+MS` set to V.90 or V.92 (V.92 is the default),
  the modem offers V.90 as the analogue modem. `DSP_V90A=0` removes PCM
  (then the call is V.34).
- **With V.92**, it also asks for PCM upstream when the digital modem
  offers it. `DSP_V92A_UP=0` removes only PCM upstream.
- **At 8000 Hz** (`-r 8000`), V.90 as the analogue modem needs
  `DSP_V90A=1`. If not, such a line runs V.34. Refer to
  [8000 Hz lines](#8000-hz-lines).
- The modem does not offer it when V.8 is used only to get a low-speed
  modulation.
- **V.91** is between two digital modems. Thus on an analogue line it is
  always V.34: the core changes `+MS=V91` to V.34 there.

A dmodemd `-a` line is an analogue line at the two rates. The line
tells the core through `Host::analog_line()`, and the core tells the
pump through `dp_runtime_set_analog()`. A 9600 Hz line is always
analogue.

## Phases 1 and 2 (stage A)

In `dp_v34.cpp`, with `AT+MS=90` or `92` on an analogue line (refer to
above):

### V.8

- CM and JM have PCM in the modulation octet, and the PCM modem
  availability octet with the bit of the analogue modem
  (`V8_PCM_ANALOGUE`).
- A JM keeps that bit only when the CM had the bit of the digital
  modem. In the same way, the JM of the digital modem keeps its own bit
  only for an analogue modem.
- The "digital connection" bit of the PSTN access octet is only for the
  digital modem.
- V.90 is agreed (`v90a_on`) when the CM or JM of the other end has the
  bit of the digital modem (`v8_v90a()`).

### The roles

From phase 2, the analogue modem is the V.34 *answer* modem, for the two
sides that can call (9.2.2/V.90).

- When it answers, it is already the answer modem.
- When it calls, as an analogue modem calls an ISP, it becomes the
  answer modem at the end of V.8, 75 ms after CJ. From CJ, its control
  channel sends at 2400 Hz and receives at 1200 Hz (the INFO0d of the
  digital modem can come immediately). The tone detectors, which include
  the detectors of the retrain, are mirrored. The phase 2 of the
  answerer (`P2A_*`) starts with INFO0a.

This is the swap of the digital modem, in the opposite direction: the
digital modem becomes the call modem.

### INFO0a and INFO0d

- **INFO0a** is the INFO0 of V.34 (Table 8/V.90): our symbol rates and
  carriers, with bits 26 and 27 clear.
- **INFO0d** (Table 7/V.90) is 13 bits longer than INFO0: 58 bits to
  the end of its CRC, which is at bit 42. It has the phase 2 power of
  the digital modem, its maximum power and the point where it measures
  it, the law of the network (bit 39) and if it accepts 3429 baud
  upstream (bit 40).
- The receiver looks for frames of the length of INFO0d:
  - while it waits for INFO0 (`info0_rx_len()`);
  - for the INFO0d repeats of the recovery of phase 2;
  - when it finds the difference between INFO1d and the INFO0d before
    it.
- The law in INFO0d becomes our law (`alaw`): the PCM receiver will
  decode it.

### Probing and INFO1d

Probing is the same as in V.34: L1 and L2 in the two directions. INFO1d
(Table 9/V.90) is INFO1c: for each symbol rate, the carrier, the
pre-emphasis and the maximum that the digital modem can receive from
us.

### INFO1a

INFO1a (Table 10/V.90, `make_info1a_v90()`) asks for V.90 with 6 in
bits 37:39 (the 8000 symbols each second of the digital modem).

- **Bits 34:36** hold the upstream symbol rate that `v90a_choose()`
  selects. Of 3000, 3200 and 3429 baud (3429 only if INFO0d permits
  it), it selects the rate for which INFO1d gives the most. It uses the
  carrier that INFO1d asks for, if our INFO0a has it.
- **If INFO1d offers none of them**, we send the INFO1a of V.34
  (`choose_bauds()`), and the call continues as V.34 with us as its
  answer modem (9.2.2.1.9/V.90).
- **UINFO** (bits 25:31) is the loudest Ucode in the maximum power of
  INFO0d, and a minimum of 67. 0 dBm0 is 3.17 dB below the sine at the
  overload point of µ-law (3.14 dB for A-law). Thus the −12 dBm0 of
  INFO0d (what our digital modem sends) gives UINFO 79 (−12.3 dBm0;
  −12.0 for A-law). The analogue modems in the interop tests ask for 77
  to 79 with the same INFO0d.

Then phase 3 starts.

## Phase 3 (stages B and C)

This is the `P3V_*` states of `dp_v34.cpp` and the PCM receiver of
`v90a.cpp` (`V90Rx`).

### What we send

`v90a_start()` sets up the V.34 transmitter at the upstream symbol
rate, on the carrier that INFO1d asked for, with the scrambler of the
answer modem (GPA). The sequence is:

1. 70 ms of silence.
2. S for 128 T, then S-bar for 16 T.
3. PP, then TRN for 2048 T. (A minimum of 512 T is necessary: the
   digital modem trains on PP and TRN.)
4. Ja, again and again (`V34_TX_JA`). Ja is the DIL descriptor
   (`V90Dil::build()`, the inverse of `parse()` of the digital modem).
   It is sent as MP is: differentially on 4 points, scrambled.
5. Ja stops at the Sd-bar of the digital modem, then silence. If Sd-bar
   has not come in 3 s from the start of Ja, we retrain.
6. S when Jd is in, until Jd'; then S-bar for 16 T.
7. Silence while the DIL comes in.
8. S for 128 T and S-bar for 16 T, which end the DIL (9.3.2.10/V.90: in
   5 s).

### The receiver's input

The receiver operates on the line at 16000 Hz, two samples for each
PCM symbol.

- `V90Rx::line()` takes the 9600 Hz samples of the pump through
  `V90Interp`: a Blackman-windowed sinc over 128 samples, cut at
  4800 Hz (the Nyquist frequency of the line), tabulated at 256 phases
  between two samples and interpolated linearly between those.
- On an 8000 Hz line, it takes the samples of the line (from
  `process()`), through the same interpolator at a step of 0.5 sample.
- The output time of the interpolator moves with the timing loop
  (below).

Why 16000 Hz: the PCM signal fills the band up to 4000 Hz and more
(the codec of netsim is a raised cosine with a roll-off of 0.15, up to
4600 Hz). Sampled at 8000 Hz, the band above 4000 Hz folds onto itself.
Half a symbol away from the instants of the symbols, it cancels near
4000 Hz. Then a T-spaced equaliser decreases from 90 to 21 dB, and
nothing brings it back. At 16000 Hz nothing folds, and a T/2 equaliser
operates correctly with all delays (86 to 88 dB at each phase, offline,
on a raised cosine line).

### Finding Sd

The receiver looks for Sd on alternate samples (8000 Hz), because Sd
repeats each 6 symbols at all phases.

- Over 24 samples, the line is similar to itself 6 samples before, and
  to its negative 3 samples before (the pattern +W +0 +W −W −0 −W).
  This must be true for 96 samples.
- At the reversal of Sd, the lag-6 correlation over 24 samples
  decreases to approximately 0.5 (the 6 samples after it against the 6
  before), at all phases.
- In that dip, the receiver takes the 6 samples whose lag-6 correlation
  is most negative: −1 on the instants of the symbols, −0.25 at a third
  of a symbol away, where the reversal spreads.
- That gives symbol 0 of S-bar d, which is data frame interval 0, at
  sample `b` (16 kHz). TRN1d starts 48 symbols later.

### The equaliser

- 128 taps at 16000 Hz (T/2, over 64 symbols). The sample of symbol k,
  b + 2k, is at the middle tap.
- The signs of TRN1d are known (GPC on ones, from zero). Thus:
  1. its first 96 symbols centre the delay (b moves to the best
     correlation, of the two polarities, because a line can reverse
     it);
  2. the next 1020 symbols give the normal equations, solved by
     Cholesky decomposition;
  3. after that, NLMS (step 0.005) on the signs of TRN1d up to its
     minimum length of 2040 T, and on the decisions after it.

### UINFO's level at each position

The receiver keeps a target level for each position, symbol k mod 6
(`tlev`). From the solution, the targets are the recent mean magnitude
of each position, scaled so that the loudest is equal to the level of
UINFO.

This is necessary for robbed bits. A robbed bit takes the least
significant bit of UINFO in its interval (Ucode 79 comes out as 78, one
step lower). With one level for all six positions, the equaliser was
trained to a target that it could not get one symbol in six: 38 dB
where the clean line gives 77. The spread of UINFO (the noise on which
the design of CP is based) came out at 13 instead of 0.5. An equaliser
that is the same for all six positions cannot give one of them its own
level. Thus the level that it gives there comes from the line, and the
targets follow it. Ri uses the same targets.

### The timing loop

`timing()` runs from the solution of the equaliser.

- **Error signal:** the error of the equaliser times its slope,
  (a − y)(y(+1) − y(−1)). It uses its output one sample on each side,
  divided by the recent power of the targets (the codes of the DIL and
  of the data are up to 64 times the codes of UINFO).
- **Loop:** it moves the output time of the interpolator with a phase
  term (0.02) and a frequency term (2e−5), which follows the clock of
  the codec.
- **Clock fit:** from symbol 2000 to 6000, the phase that it has moved
  is fitted to a straight line. Its slope gives the offset of the clock
  to approximately 0.1 ppm. The frequency term alone has an error of 1
  or 2 ppm. Then the loop runs on the fitted value, with gains of 0.004
  and 2e−7. If not, the jitter of the loop would remove the margin of
  the dense constellations.
- **Second solution:** then the equaliser is solved again, on the
  locked timing. (The first solution is over a drifting clock: 29 dB at
  100 ppm.) Then the spread of UINFO over 512 symbols is used as the
  noise (refer to [Phase 4](#phase-4-stage-d)).
- Only then do we send our S, which ends Jd. (We can wait 5 s for it,
  9.3.2.7/V.90. When Jd is in, the code sends S at the latest 4 s into
  the wait, and retrains if Jd has not come in 4.5 s.) The receiver is
  ready approximately 1.1 s into TRN1d.
- During the DIL, the loop runs on the frequency only: the levels of
  the DIL are the values that it measures, thus it has no reference.
  From Ri, it follows the error again: the signs of Ri, the known
  symbols of TRN2d, then the decisions.

### Jd and Jd'

The sign of each decision is decoded differentially, then goes through
the GPC descrambler (which synchronises itself, thus the bits of TRN1d
before Jd cause no problem). Then it is shifted into a 72-bit window.
The receiver accepts a Jd frame when its sync, start bits, CRC and fill
bits are all correct (`V90Jd`: the rates, the constellations of CP, E
and SCR, and the look-ahead). Jd' is 8 zeros where the sync of the next
frame would be.

### The DIL we ask for

`v90a_start()` asks for this DIL (`DSP_V90A_DIL=0` asks for none: N is
0, and the digital modem goes to phase 4 after Jd'):

- all 128 Ucodes, loudest first;
- 36 symbols each (H = 5: six in each data frame interval);
- each symbol a training symbol (TP = 1, no reference symbols);
- the sign pattern SP is 12 long, and its second half is the opposite
  of its first half. Thus each interval has three of each sign in a
  segment.

A round is 4608 symbols (0.58 s), and the receiver measures two rounds.
The DIL starts 12 symbols after Jd' (5 after its 8th symbol, where the
receiver identifies it), in data frame interval 0.

The equaliser does not change during the DIL, because the levels are
not known yet (a pad changes them). The value of each symbol times its
sign goes into the sums for its interval and Ucode (`dil_add()`).

### What the DIL shows

`V90Rx::dil_analyse()` calculates these values for the constellations
of stage D:

- **Levels and noise:** the level of each Ucode in each interval
  (`dil_level`) and its noise. The noise of the DIL is the median of
  those (approximately 1 on a 16-bit scale on the quiet line of
  netsim). The noise of the data mode is a different measure: the mse
  of the equaliser on random signs.
- **Unusable Ucodes:** a Ucode is unusable where its noise is more than
  4 times the noise of the DIL plus 0.2% of its level. This finds the
  loudest codes, which clip at the codec of the analogue modem (from
  approximately Ucode 112 on the line of netsim). The loudest come first
  in a round. Thus the part of them that spills over the span of the
  equaliser falls on the other codes that clip. (When Ucode 0 came
  first, at the start of the round, it had a noise of 280.)
- **Robbed bits:** the receiver thinks that an interval is robbed when:
  - two adjacent Ucodes (2m and 2m + 1, or 2m + 1 and 2m + 2, as the
    octet of the law sets) come out the same in 7 of 10 of the pairs
    that the noise can separate; or
  - behind a pad (which moves the codes first), the interval separates
    fewer than 3/4 as many levels as the best interval.
- **The pad:** the equaliser, trained on UINFO through the pad, removes
  the loss of the pad. But the codes that were requantised lower show in
  the shape of the levels. Of 0 to 9 dB in 0.5 dB steps, the pad is the
  one whose levels (re-encoded, scaled as the levels of UINFO) are the
  best fit to the measured levels. The fit uses the levels at 10 times
  the noise and more, in the intervals that are not robbed.
- **Distinct levels:** the number of levels that each interval
  separates (3 times the noise apart):

  | Line | Levels told apart (of 128) |
  |---|---|
  | netsim, quiet | 111 to 113 |
  | 3 dB pad | about 94 |
  | robbed interval | about half |
  | netsim noise at 100 (`NET_FLOOR=100`) | about 42 |

### Failures

A failure in phase 3 causes a retrain (stage F, below): no Sd-bar, no
Jd or no Jd' (9.3.2.4, 9.3.2.7/V.90), or no DIL. After the last S-bar,
phase 4 starts.

### Results on netsim

The `NET_PAD` and `NET_RBS` of netsim put pads and robbed bits on the
codes that go to the analogue modem (refer to
[testing](../testing-details.md)). What the DIL found with them:

- 0, 3 and 6 dB pads (µ-law and A-law) found as 0.0, 3.0 and 6.0 dB,
  on a quiet line or with `NET_FLOOR=100` to 300;
- one, two and three robbed intervals, alone and behind a 6 dB pad,
  and with the noise at 100 (where a robbed bit is less important: the
  robbed interval separates 31 levels, and the other intervals 43).

The equaliser on the quiet line of netsim (mse relative to the power of
the two points): 77 dB at TRN1d, at all phases of the samples of the
codec (`NET_PHASE`), and with its clock 50 to 200 ppm away from the
clock of the network (`NET_PPM`).

| Clock offset | Noise before our S (16-bit scale) |
|---|---|
| none | 0.4 |
| 50 ppm | 0.9 |
| 100 ppm | 1.6 |
| 200 ppm | 2.8 |

The noise of the DIL is 0.4 to 1.0 in all of them. Before the T/2
receiver, the value was 75 dB at 8000 Hz on the clock and phase of the
network only (32 to 36 dB with the converter of `dsp_util`, with all
taps).

### The echo canceller

The echo canceller (`V34Echo`, from the V.34 answerer) has a near part
of 256 taps at 9600 Hz and a far part found at the round-trip delay,
adapted by NLMS. It removes our own echo from the line before
`V90Rx::line()`: our S during Jd, S-bar, the CPt, SCR, CP and E of
phase 4, and the upstream data that comes after.

- It trains while S, S-bar, PP, TRN and Ja go out and the digital modem
  is silent, until the Sd of the digital modem (`ec_schedule()`). Then
  it does not change.
- For the PCM receiver, NLMS does not remove sufficient echo in that
  time: approximately 33 dB of an echo 10 dB down. This leaves a noise
  of 12, but a quiet line has 0.5. Thus the canceller keeps the data
  that it trained on. At the end, it solves the near part by least
  squares (`V34Echo::ls_solve()`: the normal equations built from their
  first row, each next row is the row before moved on by a sample,
  solved by Cholesky). That gives 47 to 51 dB. The canceller keeps this
  solution when it is better than NLMS.
- If it removes less than 6 dB (no echo), it stays off.

`DSP_TEST_ECHO` gives a modem its own echo in the tests (refer to
[testing](../testing-details.md)).

### Not done yet

MD, and SCR during the DIL, are not done.

The sign pattern of the DIL repeats with each segment. Thus the
remainder of the adjacent symbols after the equaliser adds in the same
way each time. The levels of some intervals come out up to 0.3% high or
low. The random symbols of TRN2d correct this: the gain of each interval
on them, fitted over the second half of TRN2d, scales the levels used
for MP, Ed and CP.

## Phase 3: the procedure and the receiver, as planned

This section is the original plan for phase 3 (9.3.2/V.90, the
analogue modem). It tells why the receiver has the design that the
previous section gives.

### The procedure

- **Out:** 70 ms of silence, S (128 T), S-bar (16 T), MD if INFO1a
  asked for one (we do not), PP, TRN (a minimum of 512 T, 4 points),
  then Ja (8.3.1, Table 12/V.90: the DIL descriptor) until the Sd of
  the digital modem changes to S-bar d, then silence. The plan was to
  retrain if that had not occurred in 1500 ms. The code waits 3 s from
  the start of Ja (`P3V_JA`). This is the answerer phase 3 of V.34 at
  the upstream symbol rate, with Ja instead of J (`ptx` as `P3A_*` has
  it), and the echo canceller trained on it as in V.34.
- **In:** the Sd, S-bar d, TRN1d (its two points, with signs from the
  scrambled ones; the equaliser trains on its first 2040 T) and Jd
  (Table 13/V.90: the rates that it can send, the constellation sizes)
  of the digital modem. Then S from us (in 5 s), the Jd' of the digital
  modem and the DIL that our Ja asked for, and S-bar from us after Jd'
  (9.3.2.5 to 9.3.2.8/V.90).

### What the PCM receiver needs

The PCM receiver is the new part. The codewords of the digital modem
become voltages at the 8000 Hz clock of the network, and go through the
analogue loop to our codec. Our codec runs at 9600 Hz (the rate of the
converter, or the rate of the codec on an ALSA line: 8000 or 9600 Hz)
on a separate clock. The receiver needs:

- **Timing** locked to the clock of the network. The data pump at
  9600 Hz gives 6/5 samples for each PCM symbol. Thus it needs a
  fractionally spaced equaliser at T/2 or finer, a timing estimate (from
  the drift of the centre of mass of the taps of the equaliser, or from
  a timing error detector on the two points of TRN1d), and an
  interpolator. The clock of an ALSA codec is free-running, thus real
  hardware at 8000 Hz also needs this.
- **An equaliser** trained on TRN1d (known after its first symbols: the
  points are ±the level of UINFO, the signs are the scrambled ones),
  then decision-directed on Jd, which also uses 2 points. Its output is
  the linear value of the sent codeword, before the decision.
- **An echo canceller** for our own upstream signal while the
  downstream comes in (the canceller of V.34, with the reference at
  9600 Hz). In phase 3, the digital modem sends TRN1d while we are
  silent or send TRN, thus the canceller trains on TRN.
- **Detection** of Sd and S-bar d for the frame alignment (8.4.4/V.90:
  the six-symbol pattern +W, +0, +W, −W, −0, −W of Ucodes UINFO + 16
  and 0). The first symbol of Sd is in data frame interval 0.

### What the DIL is for

The DIL (8.4.1/V.90; the descriptor of Ja, Table 12/V.90, tells what it
is) is the codes of the digital modem in a pattern that we ask for:
each Ucode that we name, repeated, in each of the 6 frame positions.
From the output of the equaliser for each code, position by position,
the receiver finds:

- the level of each Ucode as it arrives (the pads of the network change
  it: a 3 or 6 dB digital loss maps Ucodes onto other Ucodes);
- robbed-bit signalling: in some positions the least significant bit is
  lost, thus odd and even codes arrive at the same level, as on T1
  lines;
- the noise at each, which sets the minimum distances for the
  constellations.

## Phase 4 (stage D)

Phase 4 (9.4.2/V.90) uses the `P4V_*` states of `dp_v34.cpp`, the
constellation design of `v90a.cpp` and the phase 4 of the PCM receiver.

### The constellations

The constellations of CPt are designed at the end of phase 3
(`v90a_phase4()`), and the constellations of CP when TRN2d has shown the
noise (`v90a_cp_design()`). The two use the data that the PCM receiver
found:

- **Levels:** the level of each Ucode in each frame interval
  (`V90Rx::levels()`). These are the levels of the DIL, for the usable
  Ucodes. Without a DIL, they are the linear values of the law. For CP,
  they are scaled by the gain that TRN2d showed in each interval.
- **Noise:** the larger of:
  - `V90Rx::noise()`: the spread of UINFO around the mean of each
    interval, over 512 symbols before our S. The signs are random, thus
    this includes the intersymbol interference of the data mode. Each
    interval has its own mean, thus a robbed bit does not count as
    noise. If the noise of the DIL is larger, the receiver uses it.
  - the noise of TRN2d, measured while our SCR and its echo go out.
- **Power limit:** the maximum power that INFO0d permits (Table 15/V.90,
  by its maximum power field). The power is the formula of Table 15:
  the count of each label over the 2^K values of the modulus encoder
  (`V90Con::power()`), on the linear values of the transmitter.
- **Other parameters:** Sr = 0 (no spectral shaping), ld = 0, and all
  upstream rates from 4800 to 33600 bit/s enabled.

**CPt** (`v90a_design_cpt()`): 4 points in each interval, with D = 18
(K = 12, drn = 10, 24000 bit/s). The points are the Ucodes nearest to
L, 3L, 5L and 7L (with the signs, 8 levels with equal spacing). L starts
at 1 dB below the power limit and decreases until the power is in the
limit.

**CP** (`v90a_design_cp()`):

1. In each interval, the first point is the lowest level that is a
   minimum of half the spacing up. Each next point is a minimum of the
   spacing above the last. The spacing is 10 times the noise
   (approximately one symbol error in 10^6).
2. K is the maximum that the points can carry, up to 36 (D = 42,
   56000 bit/s), and only a rate that Jd permits (from 28000 bit/s,
   K = 15, and higher).
3. Then the design removes the largest point of the interval whose
   largest point is loudest, again and again, until the power is in the
   limit and in 3 dB of the power of CPt (8.5.2/V.90).
4. Intervals with the same points share a constellation.

If there is no CP at 28000 bit/s, the modem retrains, to V.34 (stage F,
below).

### What we send

1. **CPt** (`V90Cp::build()`, the inverse of `parse()` of the digital
   modem) immediately after the last S-bar of phase 3, from a cleared
   scrambler and differential encoder. It is sent as MP is, again and
   again (`V34_TX_CP`: a frame that follows the current frame, or E,
   starts at its end).
2. On Ri-bar: **SCR** (ones, sent as CP is, a frame of 2 bits) after
   the CPt frame that goes out, while the known part of TRN2d comes in
   (approximately 300 ms; 4000 ms maximum, 9.4.2.2/V.90).
3. Then **CP**.
4. On MP, when a full CP is out: **CP'** after the CP frame that goes
   out.
5. When a full CP' is out and MP' or Ed is in: **E** (20 ones), then
   **B1** and the upstream of the data mode (`v90a_data_setup()`).

### What we receive

`V90Rx::phase4()`:

- **Ri:** the equaliser follows the decisions on Ri. Ri is 8 frames of
  + + + − − − of UINFO in sequence (frame interval 0 first). Its
  reversal is a frame that is almost the opposite. TRN2d starts 24
  symbols after it.
- **Data frames** from there: each value goes to the nearest level of
  the constellation of its interval (the constellation of CPt). The
  labels give the R of the modulus encoder (interval 0 least
  significant), the signs are decoded differentially, and all D bits go
  through the GPC descrambler (5.4/V.90 in the opposite direction,
  Sr = 0).
- **TRN2d:** its first 340 frames are known. A copy of the encoder of
  the digital modem (`V90Enc` on our CPt, with Ucode + 1 as the linear
  values) gives the symbols. These train the equaliser and count its
  errors. After them, the receiver uses the decisions.
- **MP and MP'** come from the bits (`V90Mp`, Table 16/V.90: 86 bits,
  or 188 with the coefficients of the precoder).
- **Ed** is the 2 frames of zeros after the last frame of an MP'
  sequence.
- **B1d** follows on the constellations of CP, from a cleared
  descrambler and signs: 48 frames of ones. The receiver counts its bit
  errors.

When B1d is in and our E is out (`P4V_E`), the modem reports CONNECT
and goes to the data mode (`V90A_DATA`, below).

### Results on netsim

| Line | Result |
|---|---|
| quiet | the points of CPt are Ucodes 44, 67, 79 and 85 (−13.1 dBm0, and INFO0d gives −12); TRN2d has no incorrect symbol; CP has 89 points in each interval (D = 42, 56000 bit/s, −12.1 dBm0); B1d has no bit error |
| 3 dB pad | 70 points, 56000 bit/s |
| a robbed bit in one interval | 56000 bit/s, µ-law and A-law (that interval has half the points of the other intervals: 48 against 96) |
| 6 dB pad, two robbed intervals and noise at 30 (`NET_FLOOR`) | 33333 bit/s, limited by the noise (57 at the receiver) |

### A fix found on the way

Before this fix, the S-bar of V.34 after an S of odd length started its
alternation again from the start. Thus it was S turned by 90 degrees,
not reversed, and the digital modem did not find it. Our S before Jd'
continues for the time that Jd' takes, thus its length can be odd. Now
S-bar continues the alternation of S (`V34Tx::s_odd`).

### Tests

Run with `ctest -R open_v90a` (label `open`). In each test, the open
modem at 9600 Hz is the analogue modem and the open digital modem
(`open8`) is on the simulated network. Each test is a data call: lines
of text go in the two directions, and the test checks them.
`open_v90a_ulaw` (200000 octets), and `open_v90a_ppm_fast`,
`open_v90a_echo_answers` and `open_v90a_dil_rbs` (100000 octets) also
send bulk data (`BULK`).

| Test | What it covers |
|---|---|
| `open_v90a_ulaw` | the analogue modem calls. INFO0d gives µ-law; INFO1a asks for V.90 at 3429 baud with UINFO 79. The digital modem has our Ja (DIL N = 128); Sd-bar is found; Jd is decoded (all rates); our S ends Jd; Jd' is found; the DIL shows no pad and no robbed bits; our S and S-bar end it after 2 rounds. The digital modem has our CPt (4 points); TRN2d has no incorrect symbol; MP' and Ed are in; B1d has no bit error at 56000 bit/s. The two modems connect (33600 up, 56000 down), and 200000 octets go in each direction with the downstream error less than 1 rms |
| `open_v90a_alaw` | the same on an A-law network |
| `open_v90a_answers` | the digital modem calls and the analogue modem answers (and stays the V.34 answer modem) |
| `open_v90a_nodil` | no DIL (`DSP_V90A_DIL=0`): the constellations on the levels of the law |
| `open_v90a_phase` | the samples of the codec half a network sample away from the symbols (`NET_PHASE=0.5`) |
| `open_v90a_echo` | the own echo of the analogue modem 10 dB down, 1 ms late (`DSP_TEST_ECHO`): the least squares canceller removes 40 dB and more; B1d without an error at or near 56000 bit/s |
| `open_v90a_echo_answers` | the echo 6 dB down, the analogue modem answers on an A-law network, its codec 50 ppm off |
| `open_v90a_ppm_fast`, `open_v90a_ppm_slow` | the clock of the codec 100 ppm fast (phase 0.3); and 100 ppm slow on an A-law network with the analogue modem that answers (phase 0.7). The timing is found to 0.5 ppm or better; B1d has no error |
| `open_v90a_dil_pad` | a 3 dB pad |
| `open_v90a_dil_rbs` | one robbed interval: 56000 bit/s, the noise of UINFO less than 1, 100000 octets in each direction |
| `open_v90a_dil_pad_rbs` | a 6 dB pad with two robbed intervals behind it and some noise; through B1d without a bit error |
| `open_v90a_dil_rbs_alaw` | a robbed interval on an A-law network |
| `open_v90a_stats` | the statistics of the call on the two modems in the middle of the data (`ATI11`, `AT&V1`, `AT#UD`): symbol rates and carriers, the trellis code of the upstream, the 100 ppm codec offset seen by each modem, and the pad and robbed bit as the DIL found them |
| `open_v90a_default` | no settings, the default `AT+MS` (92) on the two modems: V.92 with us as the analogue modem, with PCM upstream |
| `open_v90a_off` | the same with `DSP_V90A=0`: V.34 |

## Data mode (stage E)

The data mode is the `V90A_DATA` state of `dp_v34.cpp` and
`V90Rx::frame()` on `con_data`, the constellations of CP.

### Downstream

Downstream is 5.4/V.90 in the opposite direction, with Sr = 0:

- Each of the six values of a frame goes to the nearest level of the
  constellation of its interval. The labels give the R of the modulus
  decoder (K bits), the signs are decoded differentially, and all D
  bits go through the GPC descrambler. The descrambler continues from
  the state that B1d left.
- The bits wait in the queue of `V90Rx` (`take()`, 1024 bits) for the
  pump, which gives them to the core (`CoreBits`) one sample at a time.
- The equaliser and the timing loop follow the decisions, as in
  phase 4. The level of each point also follows its own decisions
  (`LEV_MU`, 0.01), because the levels of the DIL, also when scaled by
  the gain of TRN2d, are slightly incorrect for some codes.
- The rate is the rate of CP: (drn + 20) × 8000 / 6 bit/s.

### Upstream

`v90a_data_setup()` starts the data mode of V.34 at the symbol rate of
INFO1a, from E. The rate is the highest that the rate mask of our CP
and the MP of the digital modem both permit (its maximum, `rate_max`).
It uses the trellis code of MP (16, 32 or 64 states), the coefficients
of its precoder, and its expanded constellation and nonlinear encoder
bits.

### CONNECT

CONNECT comes when B1d is in and our E is out. The log shows
"V.90 connect <up>/<down> (analogue modem)". The statistics of the call
are kept as for V.90, then V.42 and V.44 run as with V.34.

### Carrier loss and logging

If the power of the downstream stays below a thousandth of the power of
UINFO for the carrier loss time of the pump, the call ends. Each
second, the log gives the error of the downstream (rms, 16-bit scale),
its signal-to-error ratio, the timing offset and the phase. Retrains
and renegotiations are stage F, below.

### No DC removal

Usually, the core removes the DC of the line (`dcr_process()` in
`Modem::process()`). It does not do this when the pump reports that it
takes exact PCM codes (`dp_runtime_pcm()`), as V.91 and the PCM
upstream of V.92 do. The PCM receiver reports this from phase 3.

The reason: the DC of the codes is part of their signal. The estimate
of the DC remover is a whole number that changes each block. It came
through as an offset of up to 10 on each value, and no equaliser could
remove it. In data mode, this made the error move between 0.5 and
9 rms over some seconds, and V.42 frames failed. Without it, on the
quiet line of netsim, the error stays at 0.5 rms (78 dB) through a
200000-octet transfer.

### On by default

V.90 as the analogue modem is the default of the 9600 Hz modem (`v90a`
in `V34Pump::start()`; refer to [When it is used](#when-it-is-used)).
The core keeps V.90 and V.92 on an analogue line. With a modem that is
not a V.90 digital modem, V.8 agrees on V.34 as before.

## 8000 Hz lines

On an analogue line at 8000 Hz (dmodemd `-a -r 8000`), the core reports
the line as analogue with `dp_runtime_set_analog()`. If not, the pump
would think that an 8000 Hz line is the network of the digital modem.

- The PCM receiver takes the samples of the line from `process()` as
  they are, through `V90Interp` at 0.5 sample for each output. (The
  8000 to 9600 Hz converter of the pump cuts below 4000 Hz, which would
  remove part of the PCM signal.)
- The echo canceller runs on those samples at 8000 Hz, on the signal
  that we send at the same time. `DSP_TEST_ECHO` also operates there.

The samples of the codec have the band above 4000 Hz folded onto the
band below it:

- **On the symbols of the network (phase 0)**, the samples are the
  codes.
- **A quarter of a sample off**, an equaliser removes the folding:
  66 dB at TRN1d, where 9600 Hz gives 77; 56000 bit/s, with the points
  of CP 17 apart, where they are 8 at 9600 Hz.
- **Half a sample off**, the folded part cancels the signal where it
  falls, near 4000 Hz, and no equaliser brings it back (21 dB maximum,
  offline with 128 taps): no constellation.

A codec whose clock is not the clock of the network drifts through all
phases, and through the bad phase each 1.25 s at 100 ppm. Thus V.90 at
8000 Hz is stable only with the codec on the clock of the network, and
the modem uses it only with `DSP_V90A=1`.

In the tests, `NET_ANALOG=a` (or `b`) of netsim with `NET=8000:8000`
makes modem A (or B) such a modem, and `MODEM_ANALOG=<name>` tells
`half_open` that its line is analogue.

| Test | What it covers |
|---|---|
| `open_v90a_8k` | phase 0.25, 100000 octets in each direction |
| `open_v90a_8k_answers` | phase 0.1, answering, A-law |
| `open_v90a_8k_off` | without `DSP_V90A=1`: V.34 |

## Retrains and rate renegotiations (stage F)

This is 9.5.2 and 9.6.2/V.90, tested with the side of our digital modem
(9.5.1, 9.6.1).

### The digital modem's renegotiation

Figure 8/V.90:

1. In data mode, `V90Rx::rd_frame()` looks for Rd: + + + − − − (of the
   two polarities) of the loudest point of each interval (label 0 or 1),
   8 frames in sequence. From then on, no bits go to the core (circuit
   104 is clamped).
2. On Rd-bar, 24 symbols later, the frames of phase 4 start again
   (`reneg_frames()`): the constellations of CPt, a cleared descrambler
   and signs.
3. TRN2d is optional in a renegotiation, thus the receiver does not
   think that it is known. The equaliser trains on the decisions. Their
   error over frames 170 to 339 is the noise for the design of the new
   CP (the measure of phase 3 is not used).
4. The pump (`v90a_reneg()`) sends S for 128 T and S-bar for 16 T
   (`V90A_RN_S`, `V90A_RN_SBAR`), then the SCR, CP (designed again),
   CP', E and B1 of phase 4, and the data at the new rates. The log
   shows "V.90 renegotiated <up>/<down> (analogue modem)". There is no
   new CONNECT.

### Ours

Figure 9/V.90: we send S and S-bar while the data continues to come
in. The digital modem answers our S-bar with Rd, and the remaining
steps are as above.

We start a renegotiation (`v90a_reneg_ours()`) when, after the first
2 s of data:

- the error of the downstream is 2.5 times the noise for which CP was
  designed (8 dB worse) for one second; or
- it is less than half of that noise for 20 s, while a faster CP is
  still possible (D less than 42).

`DSP_V90A_RENEG=<ms>[a|c]` starts one in a test, and the DTE can ask
for one with `ATO3`. If no Ed arrives in 5000 ms plus 2 round trips of
our S-bar, we retrain.

Not used: CPs (a silent period, to train the echo canceller again) and
cleardown (9.7, a CP with drn = 0). The canceller does not track, and a
hang-up ends the call without a cleardown.

### Retrains

Retrains are 9.5.2/V.90.

- **The retrain of the digital modem:** its tone B, which `rt_tone()`
  finds on the line of the PCM receiver in phases 3 and 4, in data mode
  and in renegotiations. We send 70 ms of silence, then our tone A, and
  run phase 2 again as the V.34 answerer. `retrain()` first stops the
  PCM receiver, and gives the DC removal back to the core.
- **Ours** (`v90a_fail()`): each failure of phases 3 and 4 (no Sd-bar,
  Jd, Jd', DIL, Ri-bar, TRN2d, MP, MP' or B1d) and a renegotiation that
  times out. `ATO1` (the DTE) and `DSP_V34_RETRAIN=<ms>[a|c]` (a test,
  on the two modems) also start one.
- **Falling back to V.34:** if there are no constellations or upstream
  rate to use, or from the third retrain, the INFO1a of phase 2 asks
  for V.34 (the log shows "V.34 this time"). Thus a line that PCM
  cannot use connects at V.34. After four retrains, the call ends.
- When the modem is in data again after a retrain, the log shows
  "V.90 retrained". There is no new CONNECT.

### Tests

| Test | What it covers |
|---|---|
| `open_v90a_reneg` | the renegotiation of the digital modem, upstream to 24000 bit/s, then its own back up to 26400; 100000 octets in each direction across the two |
| `open_v90a_reneg_ours` | ours (`DSP_V90A_RENEG`) |
| `open_v90a_reneg_noise` | noise of 25 rms on the line to us 16 s into the call (approximately 3 s into the data mode): ours on errors, CP spaced 231 apart, 45333 bit/s |
| `open_v90a_retrain` | the retrain of the digital modem, in data mode: tone B, phases 2 to 4, data at 56000 again |
| `open_v90a_retrain_ours` | ours, with tone A |
| `open_v90a_v34` | noise of 400 rms: no CP, a retrain, V.34 at 12000 |
| `open_v90a_dte_retrain`, `open_v90a_dte_reneg` | the `ATO1` and `ATO3` of the DTE on the analogue modem |

## V.92 (stage G)

With `+MS=92` (the default; `DSP_V92A=0` gives V.90 only), our INFO0a
shows V.92 (bit 26, Table 16/V.92; no short phase 2), and the INFO0d of
the digital modem also shows it (bit 27). The two V.92 bits of INFO0d
are in the opposite sequence to INFO0a, and `Info0::parse_d()` takes
this into account. The log shows "INFO0d V.92: V.92 as the analogue
modem" (`v92a_on`).

The call is the call of V.90 (PCM down, V.34 up: our INFO1a is the
INFO1a of V.90), plus the modem-on-hold of V.92 (9.10/V.92; the side of
the digital modem is in [V.92](v92-details.md#v92-modem-on-hold)).

### Our modem-on-hold request

`AT+PMHR=<s>` in online command mode, or `DSP_V92_MOH_REQ=<ms>[:<s>]`
in a test, starts it:

1. We stop data mode as for a retrain, send our tone A, then MHreq.
2. On its MHack (with its T1), we are silent for s seconds (in T1).
3. During this time it sends ANSam, and we take the call back: its
   ANSam for 200 ms, V.8 as the call modem (CM), phases 2 to 4. The log
   shows "V.90 back from hold" and the data continues (no new CONNECT).
4. If it sends MHnack: we send MHfrr, it sends ANSam, and the remaining
   steps are the same.

### Its modem-on-hold request

MHreq after its tone B:

1. On a V.92 call, the tone A of our retrain holds back its reversal
   for 1 s (the reversal of a retrain would come before MHreq,
   approximately 600 ms after the tone).
2. We answer MHack (with our T1, `AT+PMHT`) or MHnack.
3. We go on hold on its silence or tone.
4. We send ANSam (with the V.21 receiver on the channel of CM, because
   we were the caller of the call), and answer its CM: phase 1 as the
   answer modem, then phases 2 to 4.

Now the digital modem also uses the return procedure of the requester:
its CM to the ANSam of the held modem. Before, it sent ANSam itself
after the hold, against 9.10.2.1/V.92. (The modems behind the stand-in
of netsim and the original library hang up during the hold, thus this
problem did not show.)

Tests: `open_v92a_moh` (ours, 5 s on hold, with a 50 s stream of data
across it) and `open_v92a_moh_theirs` (the request of the digital
modem).

## V.92 PCM upstream, phase 3 (stage G2)

When an INFO1d offers PCM upstream (its bit 70, Table 17/V.92: the
3429 baud low carrier bit), our INFO1a is the INFO1a of Table 18
(`make_info1a_v92()`). It has a precoder and prefilter of p1 and z2
with the fewest coefficients (192 in total, 128 each), no MD, UINFO,
and 6 for the two symbol rates. Then phase 3 follows 9.5.2/V.92.

### The PAM transmitter

`V92ATx` (`dsp/v92a.cpp`) sends PAM symbols (pulse amplitude
modulation) at 8000 each second, one for each sample of the codec of
the network.

- Each symbol is a sinc pulse cut at 4000 Hz (Blackman-windowed, 16
  symbols on each side) on our 9600 Hz line. Thus where the codec
  samples one symbol, the other symbols are at their zeros.
- Its clock is the clock of the network, as the timing loop of the PCM
  receiver measures it (`V90Rx::ppm()`, when that runs on TRN1d). Until
  then (Ru, the first TRN1u and Ja), it is free-running. At the end of
  the second TRN1u, it is in one symbol at 100 ppm.
- LU is approximately the power of our V.34 signal (`V92A_LU`, 3000 on
  the line).
- The symbols are made 16 in front of the line, thus the pump waits
  for the last pulse of E1u to go out (`e1u_sent()`).

### The sequence

Where the lengths are fixed, the transmitter goes to the next step
automatically:

1. 70 ms of silence.
2. Ru for 384 T, Ru-bar for 24 T.
3. TRN1u for 2040 T (GPA on ones, from zero).
4. Ja: 24 ones, then the DIL descriptor of Table 20 (the descriptor of
   V.90, plus the rate mask with all rates from 24000 to 48000 bit/s),
   scrambled and differentially encoded. It runs until the Sd-bar of
   the PCM receiver, and ends at a 12-bit boundary.
5. Silence while TRN1d trains the receiver.
6. Su for 144 T when Jd is in, then Su-bar for 24.5 T (half a symbol
   later from then on).
7. Su until Jp, which ends at 12 symbols. (`V90Rx` reads bit 47 of Jd
   for Jp, and the fraction of Jp in bits 18:33.)
8. Su-bar for 24 T plus the fraction of Jp (the 0.5 of the digital
   modem makes the half symbol a full symbol).
9. The second TRN1u (the data frames count from its first symbol)
   while the receiver measures the DIL.
10. CPt: 24 ones, then the CPt constellations of V.90 in the form of
    Table 23 (`v92_cpt_build()`), until Ri. It ends at the end of a
    CPt.
11. E1u (12 zeros).

### The receiver and failures

- **The receiver** is the receiver of V.90: Sd, TRN1d, Jd; then Jp' (12
  zeros, as Jd') after the Jp; the DIL; Ri.
- **Failures** (no Sd-bar, Jd, Jp, Jp', DIL or Ri in time, and the
  failures in phase 4 below) cause a retrain, with V.90 for the
  remainder of the call (`v92a_noup`). When PCM upstream has failed
  before data, the digital modem also removes bit 70 from its INFO1d
  after that.

### Fixes this needed

- **The E1u detector of our digital modem:** the fill of CPt to the end
  of the data frame can be all zeros together with the last bit of the
  CRC (when that is 0). The detector thought that this was E1u. Now E1u
  is only a frame that starts after the end of the last CPt.
- **The Sd-bar detector of the PCM receiver:** after a hold, an echo of
  ours over Sd makes its lag-6 correlation decrease slightly before the
  reversal (to 0.49, where a reversal goes to −1.4). Now the detector
  does not accept a dip that never goes below −0.2 as Sd-bar, and
  accepts the next one. V.90 had the same dip, but there Sd comes
  later, after it.

## V.92 PCM upstream, phase 4 and data mode (stage G3)

This follows 9.6.2/V.92, after E1u.

### What we send

- **TRN2u** on 4 points (Table 28: the sign bit scrambled and
  differentially encoded, then the magnitude bit, 1/√5 or 3/√5 of LU),
  from a cleared scrambler. It runs until SUVd is in (or for 12000 T),
  when TRN2d has shown the noise that the design of CP needs. It ends
  at a 12-symbol boundary.
- **SUVu** (Table 27, `v92_suvu_build()`), again and again, each the
  next sequence of the pump (`v92a_next_seq()`).
  - One **CPu** (Table 23 with type 1: the CP constellations of V.90,
    `v92_cpu_build()`) when SUVd is in. The pump sends it again if no
    acknowledgement comes in 100 ms plus a round trip.
  - All sent sequences are acknowledged when CPd is in.
  - Each sequence is filled to 12 symbols (24 bits).
- **E2u** after the current sequence, when a full sequence of ours with
  the acknowledgement has gone out and SUVd', CPd' or Ed is in (and the
  CPd is one that we can use: its moduli are sufficient for its bits,
  its classes are in its points). E2u is one symbol longer if bit 29 of
  CPd specifies this. Then **B1u** and the data.

### What we receive

The receiver reads the sequences of V.92 instead of the MP of V.90:

- SUVd (51 bits to the end of its CRC), or CPd (Table 30: its length
  from its parts, `v92_cpd_length()`, `V92Cpd::parse()`);
- their acknowledge bits, for Ed (2 frames of zeros after the
  acknowledged one), as MP' had them;
- B1d on the constellations of CPu.

### The upstream data mode

The data mode is 6.4/V.92, in `V92ATx::data_symbol()`:

1. The K = 2 (drn + 17) bits of each data frame are scrambled (GPA,
   from zero at B1u).
2. **Modulus encoder** (6.4.1): the sign of R above (M − 1)/2 is coded
   differentially, and R is inverted when the last one was 1. Then K0
   to K11 come from R by the 12 moduli. R is held in 4 words (the
   product of the moduli is up to 2^96).
3. **Point choice:** of the class of each Ki, the point (the linear
   values of CPd, the negative ones mirrored) that keeps the output of
   the precoder smallest. On the 4th symbol of a trellis frame, the
   parity of the point makes the sum of the frame agree with the Y0 of
   V.34 (the low bit of the encoder state). The trellis codes are the
   16, 32 or 64 state codes of V.34, one step for each trellis frame of
   4 symbols.
4. **Precoder:** x = u + z1 * u + p1 * x (Q0.15 and Q1.14).
5. **Prefilter:** v = z2 * x + p2 * v.
6. **Gain:** LU × G × v on the line (8.8.3: G scales v to a mean square
   of 1, which is the power that LU represents).

### CONNECT

CONNECT comes when B1d is in and B1u has gone out. The log shows
"V.92 connect <up>/<down> (analogue modem)". The downstream is the data
mode of V.90, with its retrains and modem-on-hold. Both now come back
with PCM upstream. The renegotiation and fast parameter exchange of
PCM upstream are stage G5, below.

### Results on netsim

With our digital modem on the line of netsim:

| Setting | Upstream | Downstream |
|---|---|---|
| its prefilter design (64 taps, a precoder of 32, G.711 levels up to 32124) | 38666 | 56000 |
| its plain precoder (`DSP_V92_PREFILTER=0`) | 37333 | 56000 |
| the codec 100 ppm off, the samples of the network 0.3 away from ours | 33333 | |

A 1.5 MB transfer took 5 minutes, with no slip in the design (our clock
is the clock of the network) and no renegotiation.

The E2u detector of our digital modem had the same weakness as its E1u
detector: the fill of an acknowledged SUVu, with the last 3 bits of its
CRC at zero, is a frame of zeros. Now the detector accepts E2u only
after the end of the last sequence.

### Tests

| Test | What it covers |
|---|---|
| `open_v92a_up` | µ-law, all steps of phases 3 and 4 on the two sides, 100 kB in each direction |
| `open_v92a_up_precoder` | the plain precoder of the digital modem |
| `open_v92a_up_ppm` | the codec 100 ppm fast: the mse of the digital modem at E1u stays at 0.0003 (0.05 to 0.2 if it does not follow the clock) |
| `open_v92a_up_answers` | A-law, with us as the answerer |
| `open_v92a_up_echo` | the echo of our hybrid at −10 dB while the DIL comes in under our TRN1u |
| `open_v92a_moh`, `open_v92a_moh_theirs` | modem-on-hold, which comes back with PCM upstream |
| `open_v90a_default` | V.92 with PCM upstream as the default |

## V.92 quick connect (stage G4)

### Short phase 1

Calling (9.2.1.1/V.92), when ANSam has continued for 1 s:

1. **QC1a** on V.21(L) (Table 2: two times ten ones, 0101010101 and the
   frame 0 0 0 P W 0 X Y Z 1). P is 0, because our V.42 always runs its
   detection phase. UQTS is 79 (WXYZ = 1010). Then CM.
2. The **QCA1d** of the digital modem (on V.21(H), where the JM
   receiver listens; the QC frame parser accepts the two types, and bits
   21 and 22 tell which) ends CM immediately.
3. Silence until its **ANSpcm**. (The modem does not look for QTS and
   QTS-bar before it: our PCM receiver finds Sd in phase 3 as before.)
   The 2100 Hz ANSam detector of V.8 is used, for 50 ms.
4. **TONEq** (980 Hz, the mark of V.21(L)) until ANSpcm ends (a minimum
   of 50 ms), then 75 ms of silence, and phase 2 as after V.8 for V.90,
   with us as the analogue modem.

If no ANSpcm arrives in 2 s, we send CM again and run V.8. A digital
modem that does not answer QC1a sends JM to our CM as usual.

QC1a is sent on each call (with a fixed UQTS), not only after a
previous call. It is not sent on a return from hold or in a retrain.

### Short phase 2

Short phase 2 is 9.4.2/V.92. Bit 27 of our INFO0a asks for it when we
try for PCM upstream (not in a retrain, and not after a failed PCM
upstream). If the INFO0d of the digital modem also has bit 26:

1. its tone B reversal comes first (when it receives our tone A);
2. our reversal comes 40 ms after it, then 10 ms more of tone A;
3. then the INFO1a of PCM upstream (Table 18). No INFO1d comes in short
   phase 2, thus its bit 70 cannot offer PCM upstream. The request of
   the digital modem for short phase 2 counts as the offer.

If there is no tone B reversal in 2.5 s, we retrain (with a full
phase 2).

### Results

With our digital modem, CONNECT comes:

| Start-up | Time after dialling |
|---|---|
| quick connect and short phase 2 | 9.7 s |
| V.8 and short phase 2 | 10.7 s |
| neither | 12.2 s |

Then the upstream is 37333 bit/s: the samples of the network fall at a
different phase against the samples of our line, and its prefilter
design comes out one step lower than the 38666 after V.8.

### Fixes

Two fixes on the side of the digital modem:

- Its INFO0d asked for short phase 2 in a retrain, where it actually
  does the full phase 2 (and the return from hold failed because of
  this).
- Now our own flag for it is cleared in a retrain (the INFO0 sequences
  are not sent again there).

### Not done

The answer to a QC1d of a digital modem (9.2.3; ours never sends it),
and the QC2a / CRe of V.8 bis.

### Tests

| Test | What it covers |
|---|---|
| `open_v92a_qc` | QC1a, QCA1d, ANSpcm, TONEq, short phase 2, PCM upstream, 100 kB in each direction |
| `open_v92a_qc_off` | `DSP_V92A_QC=0`: V.8, then short phase 2 |
| `open_v92a_qc_unanswered` | the digital modem with `DSP_V92_QC=0`: V.8 after our QC1a |

## PCM upstream's renegotiation and fast parameter exchange (stage G5)

### Rate renegotiation

Rate renegotiation is 9.8.2/V.92 (`v92a_reneg()`). It starts:

- from the digital modem: its Rd and Rd-bar arrive at the PCM receiver
  (the Rd of V.90: + + + − − − of the loudest points); or
- from us: on the error of the downstream, as for V.90
  (`v90a_reneg_ours()`; for margin we use a fast parameter exchange
  instead, below), on the `ATO3` of the DTE, or with `DSP_V90A_RENEG`
  in a test.

Then:

1. Our Ru at the start of the next data frame (`V92ATx::ru_req`; the
   digital modem continues to count its frames), then Ru-bar.
2. TRN2u. Its differential encoder starts from the sign of Ru-bar.
3. The sequences of phase 4 again, on the renegotiation frames of the
   receiver (TRN2d, SUVd, CPd, Ed, B1d), and a new CPu, E2u and B1u on
   the new CPd. The log shows "V.92 renegotiated <up>/<down> (analogue
   modem)".

If no B1d arrives in 25 s (the limit of the digital modem), we retrain.
The two sides do not ask for the silent period.

### The digital modem's fast parameter exchange

This is 9.9.2.2/V.92.

1. **Its Rf:** + + − − + + and − − + + − − alternately on the loudest
   point of each interval (`V90Rx::rf_frame()`). Then Rf-bar (the
   pattern one frame later, 4 frames).
2. **Its SUVd and CPd** in the modulation of the data mode (from a
   cleared descrambler and signs) until Ed, then B1d.
3. **Ours, from the next data frame:**
   - the 32 frames of RM and the 2 frames of RM' (Tables 25 and 26:
     their Ki go directly into the precoder, trellis coded);
   - then SUVu frames in the data mode (the scrambler and the sign of
     the modulus encoder from zero; with a prefilter, also the precoder
     and prefilter, as the prefilter receiver of the digital modem
     expects, as in the original library);
   - a CPus after SUVd (Table 24: the downstream as it is, thus the
     constellations of the PCM receiver stay), acknowledged when CPd is
     in;
   - E2u (a frame of zeros);
   - the 48 frames of ones of FB1u on the old parameters;
   - then B1u and the data on the new CPd, from zero. The log shows
     "V.92 fast parameter exchange done".

The digital modem counts errors in FB1u and B1u:

- with its prefilter, approximately 100 in the 2688 bits of FB1u,
  approximately as many as with the original library (154);
- with its plain precoder, none in the two, because its receiver
  accepts the restart at B1u in the way that this modem makes it (refer
  to [V.92 PCM upstream](v92-details.md#v92-pcm-upstream): the trellis
  paths run to their end and then start again from state 0, the
  descrambler from zero, and the previous outputs of the precoder stay
  in the feedback). Before that fix, the last frames of FB1u and the
  first two or three frames of B1u frequently had tens of errors.

### Our own fast parameter exchange

This is 9.9.2.1/V.92 (`v92a_fpe(2)`). We start one for margin (the
error of the downstream less than half the noise for which CP was
designed, for 20 s, with a faster CP possible; no training, different
from the renegotiation of V.90 for the same reason), or with
`DSP_V92A_FPE=<ms>` in a test.

1. RM, RM' and SUVu go out as above, without a wait.
2. The frames of the PCM receiver stay data until the Rf of the digital
   modem (its answer to our RM'). Thus the SUVd and CPd events of the
   start-up are cleared first (`V90Rx::fpe_wait()`).
3. After its SUVd, we send a CPu (Table 23, not CPus) designed again on
   the error of the data mode (`v90a_cp_design(2)`). The receiver keeps
   it separately (`V90Rx::data_cp_next()`) and uses it at the Ed of the
   exchange: B1d and the data come on it, and the frames before Ed on
   the old constellations.
4. If its Rd arrives before then, its renegotiation starts instead
   (9.9.2.1.2).

The errors of the downstream still cause a renegotiation, which has
TRN2d to train again on.

Before this was done, a renegotiation or fast parameter exchange of a
digital modem caused us to retrain to V.90. (When the digital modem
received no RM a second after its Rf, it did a renegotiation instead.
The renegotiation alone already completed correctly.)

### Tests

All tests send 150 kB in each direction.

| Test | What it covers |
|---|---|
| `open_v92a_reneg` | the renegotiation of the digital modem (`DSP_V92_RENEG`) |
| `open_v92a_reneg_ours` | ours (`DSP_V90A_RENEG`) |
| `open_v92a_fpe` | its fast parameter exchange with the prefilter (`DSP_V92_FPE`) |
| `open_v92a_fpe_precoder` | the same with its plain precoder |
| `open_v92a_fpe_ours`, `open_v92a_fpe_ours_precoder` | ours (`DSP_V92A_FPE`): a new CPu, 95 points against the 89 of the start-up, B1d clean on it |

In `open_v92a_dte_reneg`, the `ATO3` of the DTE starts ours, on a 25 s
stream of data.

## Testing

The only available digital modem is our own (`open8`): the closed
modems in the interop tests are all analogue modems. There are two
checks against this:

- The phase 3 and 4 signals of the analogue modems, from their calls to
  `open8` (`smartlink_v90_*`, `agere_v90_*`, `sm56_v90_*`, ...), are a
  reference for ours: you can compare S, PP, TRN, Ja and CP side by
  side.
- The receiver of the digital modem already reads their signals. Thus
  if it reads ours in the same way, this is a check of what we send.

The equaliser of the receiver is tested against the analogue side of
netsim (its noise, loss and echo). Its timing is tested with the clock
offset and phase of netsim between the network and the codec
(`NET_PPM`, `NET_PHASE`; refer to `open_v90a_ppm_fast` and
`open_v90a_phase`). Real lines through dmodemd `-a` have not had a test
yet.
