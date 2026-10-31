# V.90 analogue modem: technical details

This is the technical companion to [V.90 and V.92: the analogue modem](v90a.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

The open DSP has V.90 as the **digital modem** (see [V.90](v90.md)):
the end of the call on the 8000 Hz G.711 network. This page is about
the other end, the **analogue modem**: a softmodem on a telephone line
(dmodemd `-a`, see [hardware](../hardware.md)) calling an ISP's V.90
server.

## Terms

- **PCM**: the digital modem's G.711 codes, which reach the analogue
  modem as voltages on its line.
- **Ucode**: a PCM code number from 0 to 127.
- **UINFO**: the Ucode of TRN1d's two points, which the analogue modem
  chooses in INFO1a.
- **DIL** (digital impairment learning): a sequence of codes, chosen by
  the analogue modem, that shows it what the network does to each code.
- **Data frame interval**: one of the 6 symbol positions of a data
  frame. Robbed bits and some pads affect only some intervals.
- **T**: one symbol period. **T/2**: half of one.
- **mse**: mean square error. **rms**: root mean square, here on a
  16-bit scale.
- **NLMS**: normalised least mean squares, an adaptive filter update.

## Stages

The work went in stages, each with its tests:

| Stage | What | State |
|---|---|---|
| A | Phases 1 and 2: V.8, INFO0a, INFO0d, INFO1d, INFO1a for V.90 | done |
| B | Phase 3: S, PP, TRN, Ja out; the PCM receiver (timing, equaliser) on TRN1d, Jd, Jd' | done |
| C | Phase 3: the DIL and what it shows (levels, robbed bits, pads) | done |
| D | Phase 4: Ri, the CPt and CP constellations, TRN2d, MP, E, B1 | done |
| E | Data mode: the PCM decoder, V.34 upstream; CONNECT; on by default on analogue lines | done (V.90 at 9600 Hz on by default; `DSP_V90A=0` leaves it out) |
| F | Retrains and rate renegotiation as the analogue modem (9.5.2, 9.6.2/V.90) | done (no CPs or cleardown) |
| G | V.92 as the analogue modem: V.92 with V.34 upstream and modem-on-hold | done |
| G2 | V.92's PCM upstream, phase 3: INFO1a (Table 18), the PAM transmitter, Ru to E1u | done |
| G3 | V.92's PCM upstream, phase 4 and data mode | done (on by default; `DSP_V92A_UP=0` leaves it out) |
| G4 | V.92's quick connect as the analogue modem: short phase 1 calling, short phase 2 | done (calling; `DSP_V92A_QC=0` leaves QC1a out, `DSP_V92_SHORT2=0` short phase 2) |
| G5 | PCM upstream's rate renegotiation and fast parameter exchange, both ways | done |

## When it is used

The analogue modem's mode is chosen in `V34Pump::start()`
(`dp_v34.cpp`):

- **At 9600 Hz**, with `+MS` set to V.90 or V.92 (V.92 is the default),
  the modem offers V.90 as the analogue modem. `DSP_V90A=0` leaves PCM
  out (the call is then V.34).
- **With V.92**, it also asks for PCM upstream wherever the digital
  modem offers it. `DSP_V92A_UP=0` leaves out PCM upstream alone.
- **At 8000 Hz** (`-r 8000`), V.90 as the analogue modem needs
  `DSP_V90A=1`; otherwise such a line runs V.34. See
  [8000 Hz lines](#8000-hz-lines).
- It is not offered when V.8 is used only to reach a low-speed
  modulation.
- **V.91** is between two digital modems, so on an analogue line it is
  always V.34: the core turns `+MS=V91` into V.34 there.

A dmodemd `-a` line is an analogue line at either rate. The line tells
the core so through `Host::analog_line()`, and the core tells the pump
through `dp_runtime_set_analog()`. A 9600 Hz line is always taken as
analogue.

## Phases 1 and 2 (stage A)

In `dp_v34.cpp`, with `AT+MS=90` or `92` on an analogue line (see
above):

### V.8

- CM and JM carry PCM in the modulation octet, and the PCM modem
  availability octet with the analogue modem's bit (`V8_PCM_ANALOGUE`).
- A JM keeps that bit only when the CM had the digital modem's bit,
  just as the digital modem's JM keeps its own bit only for an analogue
  modem.
- The PSTN access octet's "digital connection" bit belongs to the
  digital modem alone.
- V.90 is agreed (`v90a_on`) when the other end's CM or JM has the
  digital modem's bit (`v8_v90a()`).

### The roles

From phase 2 on, the analogue modem is the V.34 *answer* modem,
whichever side called (9.2.2/V.90).

- When it answers, it is that already.
- When it calls, as an analogue modem calls an ISP, it becomes the
  answer modem at the end of V.8, 75 ms after CJ. From CJ on, its
  control channel sends at 2400 Hz and listens at 1200 Hz (the digital
  modem's INFO0d may come at once). The tone detectors, including the
  retrain's, are mirrored, and the answerer's phase 2 (`P2A_*`) begins
  with INFO0a.

This is the digital modem's swap the other way round: the digital
modem becomes the call modem.

### INFO0a and INFO0d

- **INFO0a** is V.34's INFO0 (Table 8/V.90): our symbol rates and
  carriers, with bits 26 and 27 clear.
- **INFO0d** (Table 7/V.90) is 13 bits longer than INFO0: 58 bits to
  the end of its CRC, which is at bit 42. It carries the digital
  modem's phase 2 power, its maximum power and where it measures it,
  the network's law (bit 39) and whether it takes 3429 baud upstream
  (bit 40).
- The receiver looks for frames of INFO0d's length:
  - while it waits for INFO0 (`info0_rx_len()`);
  - for the INFO0d repeats of phase 2's recovery;
  - when it tells INFO1d from the INFO0d before it.
- The law in INFO0d becomes ours (`alaw`): it is what the PCM receiver
  will decode.

### Probing and INFO1d

Probing is V.34's: L1 and L2 both ways. INFO1d (Table 9/V.90) is
INFO1c: for each symbol rate, the carrier, the pre-emphasis and the
most the digital modem can receive from us.

### INFO1a

INFO1a (Table 10/V.90, `make_info1a_v90()`) asks for V.90 with 6 in
bits 37:39 (the digital modem's 8000 symbols a second).

- **Bits 34:36** hold the upstream symbol rate that `v90a_choose()`
  picks. Of 3000, 3200 and 3429 baud (3429 only if INFO0d allows it),
  it takes the one for which INFO1d gives the most, on the carrier
  INFO1d asks for if our INFO0a has it.
- **If INFO1d offers none of them**, we send V.34's INFO1a instead
  (`choose_bauds()`), and the call goes on as V.34 with us as its
  answer modem (9.2.2.1.9/V.90).
- **UINFO** (bits 25:31) is the loudest Ucode within INFO0d's maximum
  power, and at least 67. 0 dBm0 is 3.17 dB below the sine at µ-law's
  overload point (3.14 dB for A-law). So INFO0d's −12 dBm0 (what our
  digital modem sends) gives UINFO 79 (−12.3 dBm0; −12.0 for A-law). The
  analogue modems in the interop tests ask for 77 to 79 with the same
  INFO0d.

Then phase 3 follows.

## Phase 3 (stages B and C)

This is `dp_v34.cpp`'s `P3V_*` states and `v90a.cpp`'s PCM receiver
(`V90Rx`).

### What we send

`v90a_start()` sets up V.34's transmitter at the upstream symbol rate,
on the carrier INFO1d asked for, with the answer modem's scrambler
(GPA). The sequence is:

1. 70 ms of silence.
2. S for 128 T, then S-bar for 16 T.
3. PP, then TRN for 2048 T. (At least 512 T are needed: the digital
   modem trains on PP and TRN.)
4. Ja, again and again (`V34_TX_JA`). Ja is the DIL descriptor
   (`V90Dil::build()`, the inverse of the digital modem's `parse()`),
   sent as MP is: differentially on 4 points, scrambled.
5. Ja stops at the digital modem's Sd-bar, then silence. If Sd-bar has
   not come within 3 s of Ja's start, we retrain.
6. S once Jd is in, until Jd'; then S-bar for 16 T.
7. Silence while the DIL comes in.
8. S for 128 T and S-bar for 16 T, which end the DIL (9.3.2.10/V.90:
   within 5 s).

### The receiver's input

The receiver works on the line at 16000 Hz, two samples per PCM symbol.

- `V90Rx::line()` takes the pump's 9600 Hz samples through
  `V90Interp`: a Blackman-windowed sinc over 128 samples, cut at
  4800 Hz (the line's Nyquist frequency), tabulated at 256 phases
  between two samples and interpolated linearly between those.
- On an 8000 Hz line it takes the line's own samples (from
  `process()`), through the same interpolator at a step of 0.5 sample.
- The interpolator's output time moves with the timing loop (below).

Why 16000 Hz: the PCM signal fills the band up to 4000 Hz and beyond it
(netsim's codec is a raised cosine with a roll-off of 0.15, up to
4600 Hz). Sampled at 8000 Hz, the band above 4000 Hz folds onto itself,
and half a symbol away from the symbols' instants it cancels near
4000 Hz. A T-spaced equaliser then falls from 90 to 21 dB, and nothing
brings it back. At 16000 Hz nothing folds, and a T/2 equaliser handles
any delay (86 to 88 dB at every phase, offline, on a raised cosine
line).

### Finding Sd

Sd is looked for on every other sample (8000 Hz), since Sd repeats
every 6 symbols at any phase.

- Over 24 samples, the line resembles itself 6 samples earlier and its
  negative 3 samples earlier (the pattern +W +0 +W −W −0 −W). This must
  hold for 96 samples.
- At Sd's reversal, the lag-6 correlation over 24 samples dips to about
  0.5 (the 6 samples after it against the 6 before), at any phase.
- Within that dip, the receiver takes the 6 samples whose lag-6
  correlation is most negative: −1 on the symbols' instants, −0.25 a
  third of a symbol off, where the reversal spreads.
- That gives symbol 0 of S-bar d, which is data frame interval 0, at
  sample `b` (16 kHz). TRN1d starts 48 symbols later.

### The equaliser

- 128 taps at 16000 Hz (T/2, spanning 64 symbols). Symbol k's sample,
  b + 2k, sits at the middle tap.
- TRN1d's signs are known (GPC on ones, from zero). So:
  1. its first 96 symbols centre the delay (b moves to the best
     correlation, either polarity, since a line may reverse it);
  2. the next 1020 symbols give the normal equations, solved by
     Cholesky decomposition;
  3. after that, NLMS (step 0.005) on TRN1d's signs up to its minimum
     length of 2040 T, and on the decisions after it.

### UINFO's level at each position

The receiver keeps a target level for each position, symbol k mod 6
(`tlev`). From the solution on, the targets are each position's recent
mean magnitude, scaled so that the loudest equals UINFO's level.

This is needed for robbed bits. A robbed bit takes UINFO's least
significant bit in its interval (Ucode 79 comes out as 78, a step
lower). With one level for all six positions, the equaliser was trained
to a target it could not reach one symbol in six: 38 dB where the clean
line gives 77, and UINFO's spread (the noise CP is designed on) came out
at 13 instead of 0.5. An equaliser that is the same for all six
positions cannot give one of them a level of its own, so whatever it
gives there is the line's, and the targets follow it. Ri uses the same
targets.

### The timing loop

`timing()` runs from the equaliser's solution on.

- **Error signal:** the equaliser's error times its slope,
  (a − y)(y(+1) − y(−1)), using its output one sample either side,
  divided by the recent power of the targets (the DIL's codes and the
  data's are up to 64 times UINFO's).
- **Loop:** it moves the interpolator's output time with a phase term
  (0.02) and a frequency term (2e−5), which follows the codec's clock.
- **Clock fit:** from symbol 2000 to 6000, the phase it has moved is
  fitted to a straight line. Its slope gives the clock's offset to
  about 0.1 ppm, where the frequency term alone is off by 1 or 2 ppm.
  The loop then runs on the fitted value, with gains of 0.004 and 2e−7:
  the loop's jitter would otherwise cost the dense constellations their
  margin.
- **Second solution:** then the equaliser is solved again, on the
  locked timing. (The first solution spans a drifting clock: 29 dB at
  100 ppm.) UINFO's spread over 512 symbols is then taken as the noise
  (see [Phase 4](#phase-4-stage-d)).
- Only then do we send our S, ending Jd. (We may wait 5 s for it,
  9.3.2.7/V.90; once Jd is in, the code sends S at the latest 4 s into
  the wait, and retrains if Jd has not come in 4.5 s.) The receiver is
  ready about 1.1 s into TRN1d.
- Through the DIL, the loop runs on the frequency alone: the DIL's
  levels are what it is measuring, so there is nothing to steer by.
  From Ri on it follows the error again: Ri's signs, TRN2d's known
  symbols, then the decisions.

### Jd and Jd'

Each decision's sign is decoded differentially, passed through the GPC
descrambler (which synchronises itself, so TRN1d's bits before Jd do no
harm), and shifted into a 72-bit window. A Jd frame is taken when its
sync, start bits, CRC and fill bits are all right (`V90Jd`: the rates,
the constellations of CP, E and SCR, and the look-ahead). Jd' is 8 zeros
where the next frame's sync would be.

### The DIL we ask for

`v90a_start()` asks for this DIL (`DSP_V90A_DIL=0` asks for none: N is
0, and the digital modem goes to phase 4 after Jd'):

- all 128 Ucodes, loudest first;
- 36 symbols each (H = 5: six in each data frame interval);
- every symbol a training symbol (TP = 1, no reference symbols);
- the sign pattern SP is 12 long, its second half the opposite of its
  first, so each interval has three of each sign in a segment.

A round is 4608 symbols (0.58 s), and two rounds are measured. The DIL
starts 12 symbols after Jd' (5 after its 8th symbol, where the receiver
recognises it), in data frame interval 0.

The equaliser is held still during the DIL, because the levels are not
known yet (a pad changes them). Each symbol's value times its sign goes
into its interval's and Ucode's sums (`dil_add()`).

### What the DIL shows

`V90Rx::dil_analyse()` works out, for stage D's constellations:

- **Levels and noise:** each Ucode's level in each interval
  (`dil_level`) and its noise. The DIL's noise is the median of those
  (about 1 on a 16-bit scale on netsim's quiet line). The data mode's
  noise is a different measure: the equaliser's mse on random signs.
- **Unusable Ucodes:** a Ucode is unusable where its noise is more than
  4 times the DIL's noise plus 0.2% of its level. This catches the
  loudest codes, which clip at the analogue modem's codec (from about
  Ucode 112 on netsim's line). The loudest come first in a round, so
  that what they spill over the equaliser's span falls on the others
  that clip. (When Ucode 0 came first, as the round started, it had a
  noise of 280.)
- **Robbed bits:** an interval is taken as robbed when either:
  - two neighbouring Ucodes (2m and 2m + 1, or 2m + 1 and 2m + 2,
    depending on the law's octet) come out alike in 7 out of 10 of the
    pairs that the noise can tell apart; or
  - behind a pad (which moves the codes first), the interval tells fewer
    than 3/4 as many levels apart as the best interval.
- **The pad:** the equaliser, trained on UINFO through the pad, takes
  the pad's loss away; but the codes that were requantised lower show in
  the shape of the levels. Of 0 to 9 dB in 0.5 dB steps, the pad is the
  one whose levels (re-encoded, scaled as UINFO's) best fit the measured
  ones. The fit uses the levels at 10 times the noise and above, in the
  intervals that are not robbed.
- **Distinct levels:** the number of levels each interval tells apart
  (3 times the noise apart):

  | Line | Levels told apart (of 128) |
  |---|---|
  | netsim, quiet | 111 to 113 |
  | 3 dB pad | about 94 |
  | robbed interval | about half |
  | netsim noise at 100 (`NET_FLOOR=100`) | about 42 |

### Failures

A failure in phase 3 retrains (stage F, below): no Sd-bar, no Jd or no
Jd' (9.3.2.4, 9.3.2.7/V.90), or no DIL. After the last S-bar, phase 4
follows.

### Results on netsim

netsim's `NET_PAD` and `NET_RBS` put pads and robbed bits on the codes
going to the analogue modem (see [testing](../testing-details.md)). What
the DIL found with them:

- 0, 3 and 6 dB pads (µ-law and A-law) found as 0.0, 3.0 and 6.0 dB,
  on a quiet line or with `NET_FLOOR=100` to 300;
- one, two and three robbed intervals, alone and behind a 6 dB pad,
  and with the noise at 100 (where a robbed bit matters less: the
  robbed interval tells 31 levels apart against the others' 43).

The equaliser on netsim's quiet line (mse relative to the two points'
power): 77 dB at TRN1d, at any phase of the codec's samples
(`NET_PHASE`) and with its clock 50 to 200 ppm off the network's
(`NET_PPM`).

| Clock offset | Noise before our S (16-bit scale) |
|---|---|
| none | 0.4 |
| 50 ppm | 0.9 |
| 100 ppm | 1.6 |
| 200 ppm | 2.8 |

The DIL's noise is 0.4 to 1.0 in all of them. Before the T/2 receiver,
the figure was 75 dB at 8000 Hz on the network's clock and phase only
(32 to 36 dB with `dsp_util`'s converter, whatever the taps).

### The echo canceller

The echo canceller (`V34Echo`, the V.34 answerer's) has a near part of
256 taps at 9600 Hz and a far part found at the round-trip delay,
adapted by NLMS. It takes our own echo off the line before
`V90Rx::line()`: our S during Jd, S-bar, phase 4's CPt, SCR, CP and E,
and the upstream data to come.

- It trains while S, S-bar, PP, TRN and Ja go out and the digital modem
  is silent, until the digital modem's Sd (`ec_schedule()`). Then it
  stays as it is.
- For the PCM receiver, NLMS takes away too little in that time: about
  33 dB of an echo 10 dB down, which leaves a noise of 12 against the
  0.5 of a quiet line. So what it trained on is stored, and at the end
  the near part is solved by least squares (`V34Echo::ls_solve()`: the
  normal equations built from their first row, each next row the one
  before moved on by a sample, solved by Cholesky). That gives 47 to
  51 dB, and is kept when it does better than NLMS.
- If it takes away less than 6 dB (no echo), it stays off.

`DSP_TEST_ECHO` gives a modem its own echo in the tests (see
[testing](../testing-details.md)).

### Not done yet

MD, and SCR during the DIL, are not done.

The DIL's sign pattern repeats with each segment, so what the equaliser
leaves of the neighbouring symbols adds the same way each time. Some
intervals' levels come out up to 0.3% high or low. TRN2d's random
symbols put that right: each interval's gain on them, fitted over
TRN2d's second half, scales the levels used for MP, Ed and CP.

## Phase 3: the procedure and the receiver, as planned

This section is the original plan for phase 3 (9.3.2/V.90, the analogue
modem). It explains why the receiver is built the way the previous
section describes.

### The procedure

- **Out:** 70 ms of silence, S (128 T), S-bar (16 T), MD if INFO1a
  asked for one (we do not), PP, TRN (at least 512 T, 4 points), then
  Ja (8.3.1, Table 12/V.90: the DIL descriptor) until the digital
  modem's Sd turns to S-bar d, then silence. The plan was to retrain
  if that had not happened within 1500 ms; the code waits 3 s from the
  start of Ja (`P3V_JA`). This is V.34's answerer phase 3 at the
  upstream symbol rate, with Ja in place of J (`ptx` as `P3A_*` has
  it), and the echo canceller trained on it as in V.34.
- **In:** the digital modem's Sd, S-bar d, TRN1d (its two points, with
  signs from the scrambled ones; the equaliser trains on its first
  2040 T), and Jd (Table 13/V.90: the rates it can send, the
  constellation sizes). Then S from us (within 5 s), the digital
  modem's Jd' and the DIL our Ja asked for, and S-bar from us after Jd'
  (9.3.2.5 to 9.3.2.8/V.90).

### What the PCM receiver needs

The PCM receiver is the new part. The digital modem's codewords become
voltages at the network's 8000 Hz clock, and travel through the analogue
loop to our codec. Our codec runs at 9600 Hz (the converter's rate, or
the codec's own on an ALSA line: 8000 or 9600 Hz) on a clock of its own.
The receiver needs:

- **Timing** locked to the network's clock. The data pump at 9600 Hz
  gives 6/5 samples per PCM symbol. That calls for a fractionally spaced
  equaliser at T/2 or finer, a timing estimate (from the drift of the
  centre of mass of the equaliser's taps, or from a timing error
  detector on TRN1d's two points), and an interpolator. An ALSA codec's
  clock runs free, so this is needed on real hardware at 8000 Hz too.
- **An equaliser** trained on TRN1d (known after its first symbols: the
  points are ±UINFO's level, the signs the scrambled ones), then
  decision-directed on Jd, which also uses 2 points. Its output is the
  linear value of the codeword sent, before the decision.
- **An echo canceller** for our own upstream signal while the
  downstream comes in (V.34's, with the reference at 9600 Hz). In
  phase 3 the digital modem sends TRN1d while we are silent or sending
  TRN, so the canceller trains on TRN.
- **Detection** of Sd and S-bar d for the frame alignment (8.4.4/V.90:
  the six-symbol pattern +W, +0, +W, −W, −0, −W of Ucodes UINFO + 16
  and 0). Sd's first symbol is in data frame interval 0.

### What the DIL is for

The DIL (8.4.1/V.90; Ja's descriptor, Table 12/V.90, says what it is)
is the digital modem's codes in a pattern we ask for: each Ucode we
name, repeated, in each of the 6 frame positions. From the equaliser's
output for each code, position by position, the receiver finds:

- the level of each Ucode as it arrives (the network's pads change it:
  a 3 or 6 dB digital loss maps Ucodes onto others);
- robbed-bit signalling: in some positions the least significant bit is
  lost, so odd and even codes arrive at the same level, as on T1 lines;
- the noise at each, which sets the distances the constellations can
  afford.

## Phase 4 (stage D)

Phase 4 (9.4.2/V.90) uses `dp_v34.cpp`'s `P4V_*` states, `v90a.cpp`'s
constellation design and the PCM receiver's phase 4.

### The constellations

CPt's constellations are designed at the end of phase 3
(`v90a_phase4()`), and CP's once TRN2d has shown the noise
(`v90a_cp_design()`). Both use what the PCM receiver found:

- **Levels:** each Ucode's level in each frame interval
  (`V90Rx::levels()`). These are the DIL's levels, for the usable
  Ucodes; without a DIL, the law's linear values. For CP they are
  scaled by the gain TRN2d showed in each interval.
- **Noise:** the larger of:
  - `V90Rx::noise()`: UINFO's spread about each interval's own mean,
    over 512 symbols before our S. The signs are random, so this
    includes the data mode's intersymbol interference; and because
    each interval has its own mean, a robbed bit does not count as
    noise. If the DIL's noise is larger, that is used instead.
  - TRN2d's noise, measured with our SCR and its echo going out.
- **Power limit:** the most power INFO0d allows (Table 15/V.90, by its
  maximum power field). The power is Table 15's formula, each label's
  count over the 2^K values of the modulus encoder
  (`V90Con::power()`), on the transmitter's linear values.
- **Other parameters:** Sr = 0 (no spectral shaping), ld = 0, and every
  upstream rate from 4800 to 33600 bit/s enabled.

**CPt** (`v90a_design_cpt()`): 4 points in each interval, with D = 18
(K = 12, drn = 10, 24000 bit/s). The points are the Ucodes nearest L,
3L, 5L and 7L (with the signs, 8 levels evenly apart). L starts at 1 dB
below the power limit and is lowered until the power is within it.

**CP** (`v90a_design_cp()`):

1. In each interval, the first point is the lowest level at least half
   the spacing up; each next point is at least the spacing above the
   last. The spacing is 10 times the noise (about one symbol error in
   10^6).
2. K is the most the points can carry, up to 36 (D = 42, 56000 bit/s),
   and only a rate that Jd allows (from 28000 bit/s, K = 15, up).
3. Then the largest point of the interval whose largest point is
   loudest is dropped, again and again, until the power is within the
   limit and within 3 dB of CPt's (8.5.2/V.90).
4. Intervals with the same points share a constellation.

If there is no CP even at 28000 bit/s, the modem retrains, to V.34
(stage F, below).

### What we send

1. **CPt** (`V90Cp::build()`, the inverse of the digital modem's
   `parse()`) right after phase 3's last S-bar, from a cleared scrambler
   and differential encoder. It is sent as MP is, again and again
   (`V34_TX_CP`: a frame to follow the current one, or E, takes over at
   its end).
2. On Ri-bar: **SCR** (ones, sent as CP is, a frame of 2 bits) after
   the CPt frame going out, while TRN2d's known part comes in (about
   300 ms; 4000 ms at most, 9.4.2.2/V.90).
3. Then **CP**.
4. On MP, once a whole CP is out: **CP'** after the CP frame going out.
5. Once a whole CP' is out and MP' or Ed is in: **E** (20 ones), then
   **B1** and the data mode's upstream (`v90a_data_setup()`).

### What we receive

`V90Rx::phase4()`:

- **Ri:** the equaliser follows the decisions on Ri. Ri is 8 frames of
  + + + − − − of UINFO in a row (frame interval 0 first); its reversal
  is a frame mostly the opposite. TRN2d starts 24 symbols after it.
- **Data frames** from there: each value goes to the nearest level of
  its interval's constellation (CPt's). The labels give the modulus
  encoder's R (interval 0 least significant), the signs are decoded
  differentially, and all D bits go through the GPC descrambler
  (5.4/V.90 the other way, Sr = 0).
- **TRN2d:** its first 340 frames are known. A copy of the digital
  modem's encoder (`V90Enc` on our CPt, with Ucode + 1 as the linear
  values) gives the symbols, which train the equaliser and count its
  errors. After them, the decisions are used.
- **MP and MP'** come from the bits (`V90Mp`, Table 16/V.90: 86 bits,
  or 188 with the precoder's coefficients).
- **Ed** is the 2 frames of zeros after the last frame of an MP'
  sequence.
- **B1d** follows on CP's constellations, from a cleared descrambler and
  signs: 48 frames of ones, its bit errors counted.

With B1d in and our E out (`P4V_E`), the modem reports CONNECT and goes
to the data mode (`V90A_DATA`, below).

### Results on netsim

| Line | Result |
|---|---|
| quiet | CPt's points are Ucodes 44, 67, 79 and 85 (−13.1 dBm0 against INFO0d's −12); TRN2d has no symbol wrong; CP has 89 points in each interval (D = 42, 56000 bit/s, −12.1 dBm0); B1d has no bit error |
| 3 dB pad | 70 points, 56000 bit/s |
| a robbed bit in one interval | 56000 bit/s, µ-law and A-law (that interval has half the others' points: 48 against 96) |
| 6 dB pad, two robbed intervals and noise at 30 (`NET_FLOOR`) | 33333 bit/s, limited by the noise (57 at the receiver) |

### A fix found on the way

V.34's S-bar after an S of odd length used to restart its alternation
from the beginning, so it was S turned by 90 degrees rather than
reversed, and the digital modem never saw it. Our S before Jd' lasts as
long as Jd' takes, so its length can be odd. S-bar now carries on S's
alternation (`V34Tx::s_odd`).

### Tests

Run with `ctest -R open_v90a` (label `open`). In each, the open modem at
9600 Hz is the analogue modem and the open digital modem (`open8`) is on
the simulated network. Each is a data call: lines of text go both ways
and are checked. `open_v90a_ulaw` (200000 octets), and
`open_v90a_ppm_fast`, `open_v90a_echo_answers` and `open_v90a_dil_rbs`
(100000 octets) also send bulk data (`BULK`).

| Test | What it covers |
|---|---|
| `open_v90a_ulaw` | the analogue modem calls. INFO0d says µ-law; INFO1a asks for V.90 at 3429 baud with UINFO 79. The digital modem has our Ja (DIL N = 128); Sd-bar is found; Jd is decoded (all rates); our S ends Jd; Jd' is found; the DIL shows no pad and no robbed bits; our S and S-bar end it after 2 rounds. The digital modem has our CPt (4 points); TRN2d has no symbol wrong; MP' and Ed are in; B1d has no bit error at 56000 bit/s. Both connect (33600 up, 56000 down), and 200000 octets go each way with the downstream's error under 1 rms |
| `open_v90a_alaw` | the same on an A-law network |
| `open_v90a_answers` | the digital modem calls and the analogue modem answers (and stays the V.34 answer modem) |
| `open_v90a_nodil` | no DIL (`DSP_V90A_DIL=0`): the constellations on the law's levels |
| `open_v90a_phase` | the codec's samples half a network sample off the symbols (`NET_PHASE=0.5`) |
| `open_v90a_echo` | the analogue modem's own echo 10 dB down, 1 ms late (`DSP_TEST_ECHO`): the least squares canceller takes 40 dB and more away; B1d without an error at or close to 56000 bit/s |
| `open_v90a_echo_answers` | the echo 6 dB down, the analogue modem answering on an A-law network, its codec 50 ppm off |
| `open_v90a_ppm_fast`, `open_v90a_ppm_slow` | the codec's clock 100 ppm fast (phase 0.3); and 100 ppm slow on an A-law network with the analogue modem answering (phase 0.7). The timing is found to within 0.5 ppm; B1d has no error |
| `open_v90a_dil_pad` | a 3 dB pad |
| `open_v90a_dil_rbs` | one robbed interval: 56000 bit/s, UINFO's noise under 1, 100000 octets each way |
| `open_v90a_dil_pad_rbs` | a 6 dB pad with two robbed intervals behind it and some noise; through B1d without a bit error |
| `open_v90a_dil_rbs_alaw` | a robbed interval on an A-law network |
| `open_v90a_stats` | the call's statistics on both modems in the middle of the data (`ATI11`, `AT&V1`, `AT#UD`): symbol rates and carriers, the upstream's trellis code, the 100 ppm codec offset seen by each, and the pad and robbed bit as the DIL found them |
| `open_v90a_default` | no settings, the default `AT+MS` (92) on both modems: V.92 with us as the analogue modem, with PCM upstream |
| `open_v90a_off` | the same with `DSP_V90A=0`: V.34 |

## Data mode (stage E)

The data mode is `dp_v34.cpp`'s `V90A_DATA` state and `V90Rx::frame()`
on `con_data`, CP's constellations.

### Downstream

Downstream is 5.4/V.90 the other way round, with Sr = 0:

- Each of a frame's six values goes to the nearest level of its
  interval's constellation. The labels give the modulus decoder's R
  (K bits), the signs are decoded differentially, and all D bits go
  through the GPC descrambler, continuing from where B1d left it.
- The bits wait in `V90Rx`'s queue (`take()`, 1024 bits) for the pump,
  which hands them to the core (`CoreBits`) a sample at a time.
- The equaliser and the timing loop follow the decisions, as in
  phase 4. Each point's level also follows its own decisions (`LEV_MU`,
  0.01), because the DIL's levels, even scaled by TRN2d's gain, are a
  little off for some codes.
- The rate is CP's: (drn + 20) × 8000 / 6 bit/s.

### Upstream

`v90a_data_setup()` starts V.34's data mode at INFO1a's symbol rate,
from E on. The rate is the highest that both our CP's rate mask and the
digital modem's MP allow (its maximum, `rate_max`). It uses MP's trellis
code (16, 32 or 64 states), its precoder's coefficients, and its
expanded constellation and nonlinear encoder bits.

### CONNECT

CONNECT comes once B1d is in and our E is out. The log says
"V.90 connect <up>/<down> (analogue modem)". The call's statistics are
kept as for V.90, then V.42 and V.44 run as with V.34.

### Carrier loss and logging

If the downstream's power stays below a thousandth of UINFO's for the
pump's carrier loss time, the call ends. Each second, the log gives the
downstream's error (rms, 16-bit scale), its signal-to-error ratio, the
timing offset and the phase. Retrains and renegotiations are stage F,
below.

### No DC removal

The core normally takes the line's DC out (`dcr_process()` in
`Modem::process()`). It does not when the pump reports that it takes
exact PCM codes (`dp_runtime_pcm()`), as V.91 and V.92's PCM upstream
do. The PCM receiver reports so from phase 3 on.

The reason: the codes' DC is part of their signal. The DC remover's
estimate is a whole number that steps every block, and it came through
as an offset of up to 10 on every value, which no equaliser could undo.
In data mode that made the error wander from 0.5 to 9 rms over seconds,
and V.42 frames failed. Without it, on netsim's quiet line, the error
stays at 0.5 rms (78 dB) through a 200000-octet transfer.

### On by default

V.90 as the analogue modem is the 9600 Hz modem's default (`v90a` in
`V34Pump::start()`; see [When it is used](#when-it-is-used)). The core
keeps V.90 and V.92 on an analogue line. Against a modem that is not a
V.90 digital modem, V.8 agrees on V.34 as before.

## 8000 Hz lines

On an analogue line at 8000 Hz (dmodemd `-a -r 8000`), the core reports
the line as analogue with `dp_runtime_set_analog()`; otherwise an
8000 Hz line would be taken for the digital modem's network.

- The PCM receiver takes the line's samples from `process()` as they
  are, through `V90Interp` at 0.5 sample per output. (The pump's 8000 to
  9600 Hz converter cuts below 4000 Hz, which would remove part of the
  PCM signal.)
- The echo canceller runs on those samples at 8000 Hz, on what we send
  at the same time. `DSP_TEST_ECHO` works there too.

The codec's samples have the band above 4000 Hz folded onto the band
below it:

- **On the network's symbols (phase 0)**, the samples are the codes
  themselves.
- **A quarter of a sample off**, an equaliser undoes the folding: 66 dB
  at TRN1d where 9600 Hz gives 77; 56000 bit/s, with CP's points 17
  apart where they are 8 at 9600 Hz.
- **Half a sample off**, the folded part cancels what it lands on, near
  4000 Hz, and no equaliser brings it back (21 dB at best, offline with
  128 taps): no constellation.

A codec whose clock is not the network's drifts through every phase,
and through the bad one every 1.25 s at 100 ppm. So V.90 at 8000 Hz
only holds with the codec on the network's clock, and it is only used
with `DSP_V90A=1`.

In the tests, netsim's `NET_ANALOG=a` (or `b`) with `NET=8000:8000`
makes modem A (or B) such a modem, and `MODEM_ANALOG=<name>` tells
`half_open` that its line is analogue.

| Test | What it covers |
|---|---|
| `open_v90a_8k` | phase 0.25, 100000 octets each way |
| `open_v90a_8k_answers` | phase 0.1, answering, A-law |
| `open_v90a_8k_off` | without `DSP_V90A=1`: V.34 |

## Retrains and rate renegotiations (stage F)

This is 9.5.2 and 9.6.2/V.90, tested against our digital modem's side
of them (9.5.1, 9.6.1).

### The digital modem's renegotiation

Figure 8/V.90:

1. In data mode, `V90Rx::rd_frame()` looks for Rd: + + + − − − (either
   polarity) of each interval's loudest point (label 0 or 1), 8 frames
   in a row. From then on no bits go to the core (circuit 104 is
   clamped).
2. On Rd-bar, 24 symbols on, phase 4's frames start again
   (`reneg_frames()`): CPt's constellations, a cleared descrambler and
   signs.
3. TRN2d is optional in a renegotiation, so it is not taken as known.
   The equaliser is trained on the decisions, and their error over
   frames 170 to 339 is the noise the new CP is designed on (phase 3's
   measure no longer counts).
4. The pump (`v90a_reneg()`) sends S for 128 T and S-bar for 16 T
   (`V90A_RN_S`, `V90A_RN_SBAR`), then phase 4's SCR, CP (designed
   again), CP', E and B1, and the data at the new rates. The log says
   "V.90 renegotiated <up>/<down> (analogue modem)"; there is no new
   CONNECT.

### Ours

Figure 9/V.90: we send S and S-bar while the data still comes in; the
digital modem answers our S-bar with Rd, and the rest is as above.

We start a renegotiation (`v90a_reneg_ours()`) when, after the first
2 s of data:

- the downstream's error is 2.5 times the noise CP was designed for
  (8 dB worse) for a second; or
- it is under half of that noise for 20 s, while a faster CP is still
  possible (D below 42).

`DSP_V90A_RENEG=<ms>[a|c]` starts one in a test, and the DTE can ask
for one with `ATO3`. If no Ed arrives within 5000 ms plus 2 round trips
of our S-bar, we retrain.

Not used: CPs (a silent period, to train the echo canceller again) and
cleardown (9.7, a CP with drn = 0). The canceller does not track, and a
hang-up ends the call without a cleardown.

### Retrains

Retrains are 9.5.2/V.90.

- **The digital modem's:** its tone B, detected by `rt_tone()` on the
  PCM receiver's line in phases 3 and 4, in data mode and in
  renegotiations. We send 70 ms of silence, then our tone A, and run
  phase 2 again as the V.34 answerer. `retrain()` takes the PCM receiver
  off first, and gives the core its DC removal back.
- **Ours** (`v90a_fail()`): every failure of phases 3 and 4 (no Sd-bar,
  Jd, Jd', DIL, Ri-bar, TRN2d, MP, MP' or B1d) and a renegotiation that
  times out. `ATO1` (the DTE) and `DSP_V34_RETRAIN=<ms>[a|c]` (a test,
  on either modem) also start one.
- **Falling back to V.34:** with no constellations or upstream rate to
  use, or from the third retrain on, phase 2's INFO1a asks for V.34
  (the log says "V.34 this time"). So a line that PCM cannot use
  connects at V.34. After four retrains the call ends.
- Back in data after a retrain, the log says "V.90 retrained"; there is
  no new CONNECT.

### Tests

| Test | What it covers |
|---|---|
| `open_v90a_reneg` | the digital modem's renegotiation, upstream to 24000 bit/s, then its own back up to 26400; 100000 octets each way across both |
| `open_v90a_reneg_ours` | ours (`DSP_V90A_RENEG`) |
| `open_v90a_reneg_noise` | noise of 25 rms on the line to us 16 s into the call (about 3 s into the data mode): ours on errors, CP spaced 231 apart, 45333 bit/s |
| `open_v90a_retrain` | the digital modem's, in data mode: tone B, phases 2 to 4, data at 56000 again |
| `open_v90a_retrain_ours` | ours, with tone A |
| `open_v90a_v34` | noise of 400 rms: no CP, a retrain, V.34 at 12000 |
| `open_v90a_dte_retrain`, `open_v90a_dte_reneg` | the DTE's `ATO1` and `ATO3` on the analogue modem |

## V.92 (stage G)

With `+MS=92` (the default; `DSP_V92A=0` gives V.90 only), our INFO0a
says V.92 (bit 26, Table 16/V.92; no short phase 2), and the digital
modem's INFO0d says so too (bit 27). INFO0d has its two V.92 bits the
other way round from INFO0a, which `Info0::parse_d()` takes into
account. The log says "INFO0d V.92: V.92 as the analogue modem"
(`v92a_on`).

The call is V.90's (PCM down, V.34 up: our INFO1a is V.90's), plus
V.92's modem-on-hold (9.10/V.92; the digital modem's side is in
[V.92](v92-details.md#v92-modem-on-hold)).

### Our modem-on-hold request

Started by `AT+PMHR=<s>` in online command mode, or by
`DSP_V92_MOH_REQ=<ms>[:<s>]` in a test:

1. We leave data mode as for a retrain, send our tone A, then MHreq.
2. On its MHack (with its T1), we go silent for s seconds (within T1).
3. Meanwhile it sends ANSam, and we take the call back: its ANSam for
   200 ms, V.8 as the call modem (CM), phases 2 to 4. The log says
   "V.90 back from hold" and the data goes on (no new CONNECT).
4. On its MHnack instead: we send MHfrr, it sends ANSam, and the rest is
   the same.

### Its modem-on-hold request

MHreq after its tone B:

1. Our retrain's tone A holds back its reversal for 1 s on a V.92 call
   (a retrain's reversal would come before MHreq, about 600 ms after the
   tone).
2. We answer MHack (with our T1, `AT+PMHT`) or MHnack.
3. We go on hold on its silence or tone.
4. We send ANSam (with the V.21 receiver on CM's channel, since we were
   the call's caller), and answer its CM: phase 1 as the answer modem,
   then phases 2 to 4.

The requester's way back, its CM to the held modem's ANSam, is now the
digital modem's as well. Before, it sent ANSam itself after the hold,
against 9.10.2.1/V.92. (The modems behind netsim's stand-in and the
original library hang up during the hold, so this never showed.)

Tests: `open_v92a_moh` (ours, 5 s on hold, with a 50 s stream of data
across it) and `open_v92a_moh_theirs` (the digital modem's).

## V.92 PCM upstream, phase 3 (stage G2)

Where an INFO1d offers PCM upstream (its bit 70, Table 17/V.92: the
3429 baud low carrier bit), our INFO1a is Table 18's
(`make_info1a_v92()`): a precoder and prefilter of p1 and z2 with the
fewest coefficients (192 in all, 128 each), no MD, UINFO, and 6 for both
symbol rates. Phase 3 then follows 9.5.2/V.92.

### The PAM transmitter

`V92ATx` (`dsp/v92a.cpp`) sends PAM symbols (pulse amplitude modulation)
at 8000 a second, one per sample of the network's codec.

- Each symbol is a sinc pulse cut at 4000 Hz (Blackman-windowed, 16
  symbols either side) on our 9600 Hz line, so that where the codec
  samples one symbol the others are at their zeros.
- Its clock is the network's, as the PCM receiver's timing loop
  measures it (`V90Rx::ppm()`, once that runs on TRN1d). Until then
  (Ru, the first TRN1u and Ja) it runs free; by the end of the second
  TRN1u it is within one symbol at 100 ppm.
- LU is about our V.34 signal's power (`V92A_LU`, 3000 on the line).
- The symbols are made 16 ahead of the line, so the pump waits for
  E1u's last pulse to go out (`e1u_sent()`).

### The sequence

Where the lengths are fixed, the transmitter moves itself on:

1. 70 ms of silence.
2. Ru for 384 T, Ru-bar for 24 T.
3. TRN1u for 2040 T (GPA on ones, from zero).
4. Ja: 24 ones, then the DIL descriptor of Table 20 (V.90's, plus the
   rate mask with every rate from 24000 to 48000 bit/s), scrambled and
   differentially encoded. It runs until the PCM receiver's Sd-bar, and
   ends at a 12-bit boundary.
5. Silence while TRN1d trains the receiver.
6. Su for 144 T once Jd is in, then Su-bar for 24.5 T (half a symbol
   later from then on).
7. Su until Jp, ending at 12 symbols. (`V90Rx` reads Jd's bit 47 for
   Jp, and Jp's fraction in bits 18:33.)
8. Su-bar for 24 T plus Jp's fraction (the digital modem's 0.5 makes the
   half symbol a whole one).
9. The second TRN1u (the data frames count from its first symbol) while
   the DIL is measured.
10. CPt: 24 ones, then V.90's CPt constellations in Table 23's form
    (`v92_cpt_build()`), until Ri, ending at the end of a CPt.
11. E1u (12 zeros).

### The receiver and failures

- **The receiver** is V.90's: Sd, TRN1d, Jd; then Jp' (12 zeros, as
  Jd') after the Jp; the DIL; Ri.
- **Failures** (no Sd-bar, Jd, Jp, Jp', DIL or Ri in time, and those in
  phase 4 below) retrain, with V.90 for the rest of the call
  (`v92a_noup`). The digital modem, once PCM upstream has failed before
  data, also leaves bit 70 out of its INFO1d after that.

### Fixes this needed

- **Our digital modem's E1u detector:** CPt's fill to the end of the
  data frame may be all zeros together with the CRC's last bit (when
  that is 0), which it took for E1u. E1u is now only a frame that starts
  after the last CPt's end.
- **The PCM receiver's Sd-bar detector:** after a hold, an echo of ours
  over Sd makes its lag-6 correlation dip a little before the reversal
  (to 0.49, where a reversal goes to −1.4). A dip that never goes below
  −0.2 is now not taken as Sd-bar, and the next one is taken. V.90 had
  the same dip, but there Sd comes later, past it.

## V.92 PCM upstream, phase 4 and data mode (stage G3)

This follows 9.6.2/V.92, after E1u.

### What we send

- **TRN2u** on 4 points (Table 28: the sign bit scrambled and
  differentially encoded, then the magnitude bit, 1/√5 or 3/√5 of LU),
  from a cleared scrambler. It runs until SUVd is in (or for 12000 T),
  once TRN2d has shown the noise that CP's design needs, and ends at a
  12-symbol boundary.
- **SUVu** (Table 27, `v92_suvu_build()`), again and again, each the
  pump's next sequence (`v92a_next_seq()`).
  - One **CPu** (Table 23 with type 1: V.90's CP constellations,
    `v92_cpu_build()`) once SUVd is in, sent again if no
    acknowledgement comes within 100 ms plus a round trip.
  - All sent acknowledged once CPd is in.
  - Each sequence is filled to 12 symbols (24 bits).
- **E2u** after the current sequence, once one of ours with the
  acknowledgement has gone out whole and SUVd', CPd' or Ed is in (and
  the CPd is one we can take: its moduli enough for its bits, its
  classes within its points). E2u is a symbol longer if CPd's bit 29
  says so. Then **B1u** and the data.

### What we receive

The receiver reads V.92's sequences in the place of V.90's MP:

- SUVd (51 bits to the end of its CRC), or CPd (Table 30: its length
  from its parts, `v92_cpd_length()`, `V92Cpd::parse()`);
- their acknowledge bits, for Ed (2 frames of zeros after the
  acknowledged one), as MP' had them;
- B1d on CPu's constellations.

### The upstream data mode

The data mode is 6.4/V.92, in `V92ATx::data_symbol()`:

1. Each data frame's K = 2 (drn + 17) bits are scrambled (GPA, from zero
   at B1u).
2. **Modulus encoder** (6.4.1): the sign of R above (M − 1)/2 is coded
   differentially, and R is turned over when the last one was 1; then
   K0 to K11 come from R by the 12 moduli. R is held in 4 words (the
   moduli's product is up to 2^96).
3. **Point choice:** of each Ki's class, the point (CPd's linear values,
   the negative ones mirrored) that keeps the precoder's output
   smallest. On a trellis frame's 4th symbol, the point's parity makes
   the frame's sum match V.34's Y0 (the encoder state's low bit). The
   trellis codes are V.34's 16, 32 or 64 state codes, one step per
   trellis frame of 4 symbols.
4. **Precoder:** x = u + z1 * u + p1 * x (Q0.15 and Q1.14).
5. **Prefilter:** v = z2 * x + p2 * v.
6. **Gain:** LU × G × v on the line (8.8.3: G scales v to a mean square
   of 1, which is the power LU stands for).

### CONNECT

CONNECT comes once B1d is in and B1u has gone out. The log says
"V.92 connect <up>/<down> (analogue modem)". The downstream is V.90's
data mode, with its retrains and modem-on-hold, both of which now come
back with PCM upstream. PCM upstream's renegotiation and fast parameter
exchange are stage G5, below.

### Results on netsim

Against our digital modem on netsim's line:

| Setting | Upstream | Downstream |
|---|---|---|
| its prefilter design (64 taps, a precoder of 32, G.711 levels up to 32124) | 38666 | 56000 |
| its plain precoder (`DSP_V92_PREFILTER=0`) | 37333 | 56000 |
| the codec 100 ppm off, the network's samples 0.3 off ours | 33333 | |

A 1.5 MB transfer took 5 minutes, with no slip to design for (our clock
is the network's) and no renegotiation.

Our digital modem's E2u detector had the same weakness as its E1u one:
an acknowledged SUVu's fill, with its CRC's last 3 bits zero, is a frame
of zeros. E2u is now only taken after the last sequence's end.

### Tests

| Test | What it covers |
|---|---|
| `open_v92a_up` | µ-law, every step of phases 3 and 4 on both sides, 100 kB each way |
| `open_v92a_up_precoder` | the digital modem's plain precoder |
| `open_v92a_up_ppm` | the codec 100 ppm fast: the digital modem's mse at E1u stays at 0.0003 (0.05 to 0.2 without following the clock) |
| `open_v92a_up_answers` | A-law, with us answering |
| `open_v92a_up_echo` | our hybrid's echo at −10 dB while the DIL comes in under our TRN1u |
| `open_v92a_moh`, `open_v92a_moh_theirs` | modem-on-hold, coming back with PCM upstream |
| `open_v90a_default` | V.92 with PCM upstream as the default |

## V.92 quick connect (stage G4)

### Short phase 1

Calling (9.2.1.1/V.92), once ANSam has lasted 1 s:

1. **QC1a** on V.21(L) (Table 2: twice ten ones, 0101010101 and the
   frame 0 0 0 P W 0 X Y Z 1). P is 0, because our V.42 always runs its
   detection phase; UQTS is 79 (WXYZ = 1010). Then CM.
2. The digital modem's **QCA1d** (on V.21(H), where the JM receiver
   listens; the QC frame parser takes either kind, with bits 21 and 22
   saying which) ends CM at once.
3. Silence until its **ANSpcm**. (QTS and QTS-bar before it are not
   looked for: our PCM receiver finds Sd in phase 3 as before.) V.8's
   ANSam 2100 Hz detector is used, for 50 ms.
4. **TONEq** (980 Hz, V.21(L)'s mark) until ANSpcm ends (at least
   50 ms), then 75 ms of silence, and phase 2 as after V.8 for V.90,
   with us as the analogue modem.

If no ANSpcm arrives within 2 s, we send CM again and run V.8. A
digital modem that does not answer QC1a sends JM to our CM as usual.

QC1a is sent on every call (with a fixed UQTS), not only after an
earlier call; it is left out on a return from hold and in a retrain.

### Short phase 2

Short phase 2 is 9.4.2/V.92. Our INFO0a's bit 27 asks for it when we go
for PCM upstream (not in a retrain, and not after a failed PCM
upstream). If the digital modem's INFO0d has bit 26 too:

1. its tone B reversal comes first (once it hears our tone A);
2. ours follows 40 ms after it, then 10 ms more of tone A;
3. then PCM upstream's INFO1a (Table 18). No INFO1d comes in short
   phase 2, so its bit 70 cannot offer PCM upstream; the digital modem
   asking for short phase 2 is taken as the offer.

If there is no tone B reversal within 2.5 s, we retrain (with a full
phase 2).

### Results

Against our digital modem, CONNECT comes:

| Start-up | Time after dialling |
|---|---|
| quick connect and short phase 2 | 9.7 s |
| V.8 and short phase 2 | 10.7 s |
| neither | 12.2 s |

The upstream is then 37333 bit/s: the network's samples land at another
phase against our line's, and its prefilter design comes out one step
lower than the 38666 after V.8.

### Fixes

Two fixes on the digital modem's side:

- Its INFO0d asked for short phase 2 in a retrain, where it actually
  does the full one (and the return from hold went wrong on that).
- Our own flag for it is now cleared in a retrain (the INFO0 sequences
  are not sent again there).

### Not done

Answering a digital modem's QC1d (9.2.3; ours never sends it), and
V.8 bis' QC2a / CRe.

### Tests

| Test | What it covers |
|---|---|
| `open_v92a_qc` | QC1a, QCA1d, ANSpcm, TONEq, short phase 2, PCM upstream, 100 kB each way |
| `open_v92a_qc_off` | `DSP_V92A_QC=0`: V.8, then short phase 2 |
| `open_v92a_qc_unanswered` | the digital modem with `DSP_V92_QC=0`: V.8 after our QC1a |

## PCM upstream's renegotiation and fast parameter exchange (stage G5)

### Rate renegotiation

Rate renegotiation is 9.8.2/V.92 (`v92a_reneg()`). It starts either:

- from the digital modem: its Rd and Rd-bar arrive at the PCM receiver
  (V.90's Rd: + + + − − − of the loudest points); or
- from us: on the downstream's error, as for V.90
  (`v90a_reneg_ours()`; for margin we use a fast parameter exchange
  instead, below), on the DTE's `ATO3`, or with `DSP_V90A_RENEG` in a
  test.

Then:

1. Our Ru at the start of the next data frame (`V92ATx::ru_req`; the
   digital modem keeps counting its frames), then Ru-bar.
2. TRN2u, its differential encoder starting from Ru-bar's sign.
3. Phase 4's sequences again, on the receiver's renegotiation frames
   (TRN2d, SUVd, CPd, Ed, B1d), and a new CPu, E2u and B1u on the new
   CPd. The log says "V.92 renegotiated <up>/<down> (analogue modem)".

If no B1d arrives within 25 s (the digital modem's own limit), we
retrain. Neither side asks for the silent period.

### The digital modem's fast parameter exchange

This is 9.9.2.2/V.92.

1. **Its Rf:** + + − − + + and − − + + − − in turn on each interval's
   loudest point (`V90Rx::rf_frame()`). Then Rf-bar (the pattern one
   frame on, 4 frames).
2. **Its SUVd and CPd** in the data mode's modulation (from a cleared
   descrambler and signs) until Ed, then B1d.
3. **Ours, from the next data frame on:**
   - RM's 32 frames and RM''s 2 (Tables 25 and 26: their Ki go straight
     into the precoder, trellis coded);
   - then SUVu frames in the data mode (the scrambler and the modulus
     encoder's sign from zero; with a prefilter, the precoder and
     prefilter too, as the digital modem's prefilter receiver expects,
     following the original library);
   - a CPus after SUVd (Table 24: the downstream as it is, so the PCM
     receiver's constellations stay), acknowledged once CPd is in;
   - E2u (a frame of zeros);
   - FB1u's 48 frames of ones on the old parameters;
   - then B1u and the data on the new CPd, from zero. The log says
     "V.92 fast parameter exchange done".

The digital modem counts errors in FB1u and B1u:

- with its prefilter, about 100 in FB1u's 2688 bits, about as many as
  with the original library (154);
- with its plain precoder, none in either, since its receiver takes the
  restart at B1u the way this modem makes it (see
  [V.92 PCM upstream](v92-details.md#v92-pcm-upstream): the trellis
  paths run to their end and then restart from state 0, the descrambler
  from zero, and the precoder's earlier outputs stay in the feedback).
  Before that fix, FB1u's last frames and B1u's first two or three
  often had tens of errors.

### Our own fast parameter exchange

This is 9.9.2.1/V.92 (`v92a_fpe(2)`). We start one for margin (the
downstream's error under half the noise CP was designed on for 20 s,
with a faster CP possible; no training, unlike V.90's renegotiation for
the same reason), or with `DSP_V92A_FPE=<ms>` in a test.

1. RM, RM' and SUVu go out as above, without waiting.
2. The PCM receiver's frames stay data until the digital modem's Rf (its
   answer to our RM'), so the start-up's SUVd and CPd events are cleared
   first (`V90Rx::fpe_wait()`).
3. After its SUVd we send a CPu (Table 23, not CPus) designed anew on
   the data mode's error (`v90a_cp_design(2)`). The receiver keeps it
   aside (`V90Rx::data_cp_next()`) and takes it up at the exchange's
   Ed: B1d and the data come on it, and the frames before Ed on the old
   constellations.
4. If its Rd arrives before then, its renegotiation takes over
   (9.9.2.1.2).

The downstream's errors still lead to a renegotiation, which has TRN2d
to retrain on.

Before this was done, a digital modem's renegotiation or fast parameter
exchange made us retrain to V.90. (The digital modem, with no RM a
second after its Rf, renegotiated instead; the renegotiation alone
already got through that.)

### Tests

All send 150 kB each way.

| Test | What it covers |
|---|---|
| `open_v92a_reneg` | the digital modem's renegotiation (`DSP_V92_RENEG`) |
| `open_v92a_reneg_ours` | ours (`DSP_V90A_RENEG`) |
| `open_v92a_fpe` | its fast parameter exchange with the prefilter (`DSP_V92_FPE`) |
| `open_v92a_fpe_precoder` | the same with its plain precoder |
| `open_v92a_fpe_ours`, `open_v92a_fpe_ours_precoder` | ours (`DSP_V92A_FPE`): a new CPu, 95 points against the start-up's 89, B1d clean on it |

`open_v92a_dte_reneg` has the DTE's `ATO3` start ours, on a 25 s
stream of data.

## Testing

The only digital modem available is our own (`open8`): the closed modems
in the interop tests are all analogue ones. There are two checks against
that:

- The analogue modems' own phase 3 and 4 signals, from their calls to
  `open8` (`smartlink_v90_*`, `agere_v90_*`, `sm56_v90_*`, ...), are a
  reference for ours: S, PP, TRN, Ja and CP can be compared side by
  side.
- The digital modem's receiver already reads theirs, so ours going
  through it the same way is a check of what we send.

netsim's analogue side (its noise, loss and echo) is what the
receiver's equaliser is tested against. Its timing is tested with
netsim's clock offset and phase between the network and the codec
(`NET_PPM`, `NET_PHASE`; see `open_v90a_ppm_fast` and
`open_v90a_phase`). Real lines through dmodemd `-a` have not been tried
yet.
