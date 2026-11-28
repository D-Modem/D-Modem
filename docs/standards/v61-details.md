# V.61: technical details

This page gives the line signals, the frames, the start-up, the retrain
and the rate renegotiation of V.61 (08/96). For a summary, refer to
[V.61](v61.md).

## The line signals (5.1, 5.2)

| Item | Value |
|---|---|
| Symbol rate | 3000 symbols/s ± 0.01 %, or 2800 symbols/s ± 0.01 % |
| Carrier | 1800 Hz ± 1 Hz at 3000 symbols/s, 1680 Hz ± 1 Hz at 2800 symbols/s |
| Frequency offset | The receiver operates with an offset of ± 7 Hz |
| Band edges | 4.5 dB ± 2.5 dB below the maximum: 300 Hz and 3300 Hz (3000 symbols/s), 280 Hz and 3080 Hz (2800 symbols/s) |
| Power | As in V.2 |

## The data-only coding (5.4)

The first two bits of each symbol (Q1, Q2) go through the differential
quadrant code of Table 1:

| Q1 Q2 | Change of quadrant |
|---|---|
| 0 0 | +90° |
| 0 1 | 0° |
| 1 0 | +180° |
| 1 1 | +270° |

The quadrants A, B, C and D are Y1 Y2 = 00, 01, 11 and 10. The two
coded bits (Y1, Y2) and the other bits (Q3 ...) select a point. The
codes are not trellis codes.

| Rate (bit/s) | Bits per symbol | Points | Figure |
|---|---|---|---|
| 14400 | 6 | 64 | 1 |
| 12000 | 5 | 32 | 2 |
| 9600 | 4 | 16 | 3 |
| 7200 | 3 | 8 | 4 |
| 4800 | 2 | 4 | 5 |

Each constellation is the one before it with more points around it.
A turn of 90° changes only Y1 Y2.

## The audio plus data coding (5.5)

At 4800 bit/s with speech, the data point is one of the 4 points of
Figure 6. The power of these points is 2.5 dB less than the power of
the data-only points. The transmitter makes the audio signal element
from the speech in these steps:

1. A pre-emphasis filter (Tables 2 and 3). The index of its coefficients
   goes in each control segment.
2. A move down by the audio carrier: 1600 Hz at 3000 symbols/s, 1493.3
   Hz at 2800 symbols/s. Then a low pass filter, and one complex sample
   at each symbol.
3. An adaptive gain for each frame (5.5.2.3): 32 steps of the fourth
   root of 2 (about 1.5 dB each). The inphase and quadrature gains are
   in 3 steps of each other.
4. An inversion of every second symbol (5.5.2.4).
5. An asymmetric scale (5.5.2.5): the negative half goes to 0.7 of the
   distance to the quadrant boundary. The positive half has a scale two
   times larger.
6. A turn by the angle of the data point (Table 5: 243.43°, 333.43°,
   63.43° and 153.43° for 00, 01, 11 and 10).

The modem adds the audio signal element to the data point. The
receiver finds the quadrant, then does the steps in the opposite
order.

## The frames (5.3)

A frame has 70 symbols. At 3000 symbols/s, symbols 1 to 56 have user
data and symbols 57 to 70 are the control segment. At 2800 symbols/s,
symbols 1 to 60 have user data and symbols 61 to 70 are the control
segment.

- A frame with speech is sent at 4800 bit/s plus audio.
- A data-only frame is sent at the data-only rate of the last start-up,
  retrain or rate renegotiation.
- Variable rate (5.3.2, an option): a modem sends data-only frames
  while its speech is silent. Rate word 3 of the far modem permits it.
- User control (5.3.3, an option): the user can ask for data-only
  frames.
- The receiver accepts the two types of frame at all times.

## The control segment (5.6)

The control segment uses the first two bits of each symbol: 28 bits at
3000 symbols/s, 20 bits at 2800 symbols/s. Bit 2 is first in time, then
bit 1. A frame can have more than two bits per symbol. Then the third
bit of each control symbol is a copy of D, and the other bits are
zeros.

| Bits | Frame with speech | Data-only frame |
|---|---|---|
| Synchronization | 1101110 (bits 1 to 7, 3000 symbols/s only) | 1101110 1110 (bits 1 to 11, 3000 symbols/s), 1110 (bits 1 to 4, 2800 symbols/s) |
| Next frame | D: 1 data only, 0 with speech | D, and DB: 0 silence, 1 user control |
| Speech | P5 to P0 (pre-emphasis), L (lock), G4 to G0, D1 D0, GF (gain) | AC, C7 to C0 (auxiliary channel) |
| Check | The inverse of D, L, and G4 G3 G2 (G4 G3 at 2800 symbols/s) | The inverse of D, and C7 to C4 (C7 to C5 at 2800 symbols/s) |

The control segment applies to the next frame. Table 6 marks some bits
with "*": the synchronization bits and the last bit of a data-only
segment. These states are only suggestions for the transmitter. The
receiver must not rely on them (Note 4). The check bits give the frame
synchronization check. A receiver can use an OR of a bit and the
inverse of its check bit. This goes towards data only, towards lock,
and towards more attenuation of the speech.

## The scramblers (7)

The caller scrambles with 1 + x^-18 + x^-23 and descrambles with
1 + x^-5 + x^-23. The answerer uses the opposite pair. The control
segments go through the scrambler with the data.

## The start-up signals (8)

| Signal | Description |
|---|---|
| S or AA | The symbol rate signal (8.3), always at 3000 symbols/s, for 256 symbols. S (A B A B ...) asks for 3000 symbols/s, AA (A A ...) asks for 2800 symbols/s. |
| DCBA | The channel holding signal of the caller (8.4), at 3000 symbols/s |
| F, G | The round trip delay tones (8.5): F 2100 Hz (or 1960 Hz) from the answerer, G 1500 Hz (or 1400 Hz) from the caller. The bar signals are the same tones with a reversal of 180°. |
| S, S-bar, TRN | The receiver conditioning signal (8.6): S (A B) for 256 symbols, S-bar (C D) for 16 symbols, then TRN for 1280 to 8192 symbols |
| TRN | Scrambled ones from a scrambler of zeros, without the differential code. The first 256 symbols are A or C (the first bit of each dibit). Then Table 9: 00 A, 01 B, 11 C, 10 D. |
| R1 to R5 | Rate signals (8.7): rate words 1, 2 and 3 one after the other, repeated, scrambled, with the differential code |
| E | One 16-bit word at the end of a rate signal (8.7.2). It gives the one rate of the B1 that follows. |
| B1 | Scrambled ones at the data rate, after E |

## The rate words (Tables 10 to 13)

B0 is first in time. B0 to B3, B7, B11 and B15 are for synchronization.
B4 and B8 identify the word.

| Word | B4 B8 | Content |
|---|---|---|
| 1 | 1 1 | The data-only rates: B5 4800, B6 9600, B9 7200, B10 12000, B12 14400 bit/s. No rate calls for a GSTN cleardown. B13 and B14 are 0, and the receiver ignores them. |
| 2 | 1 0 | B5: voice plus data at 4800 bit/s |
| 3 | 0 1 | B14: the far transmitter can use silence detection (variable rate) |
| E | 1 1 | B0 to B3 = 1111. One rate, as in word 1. |

A receiver detects a rate signal when it receives the same word 1 two
times, 48 bits apart.

## The start-up (9)

The answerer sends ANS (or the V.8 bis signals). The caller:

1. Receives the answer tone for 1 s or more, then sends S (or AA) for
   256 symbols, then DCBA.
2. Receives the S or AA of the answerer, and selects the symbol rate.
   Sends G.
3. Receives F and its reversal. Sends a reversal of G, 64 ± 2 symbols
   after the reversal of F, and starts a timer.
4. Receives the second reversal of F. Stops the timer (NT) and goes
   silent.
5. Receives S, trains, and receives R1. Sends S for NT, then S (256
   symbols), S-bar and TRN.
6. Sends R2: the rates of R1 that it can receive. Receives R3, sends E,
   then B1.
7. Starts the data 50 symbols after E with the first control segment.
   Turns on circuit 109 128 symbols after the received E.

The answerer:

1. Receives S or AA, stops ANS. Sends its S or AA for 256 symbols when
   the DCBA of the caller starts.
2. Receives G (1500 Hz or 1400 Hz), which gives the symbol rate. Sends
   F.
3. After 128 symbols of F and 64 symbols of G, sends a reversal of F
   and starts a timer (MT).
4. Receives the reversal of G. Sends a second reversal of F 64 ± 2
   symbols after it.
5. When G stops, waits 16 symbols, then sends S, S-bar, TRN and R1.
6. Receives S, goes silent, trains and receives R2. Sends S, S-bar, TRN
   and R3: the rates to use, from the rates of R2.
7. Receives E, sends E, then B1, then the data.

If R2 calls for a cleardown, or the answerer can use no rate of R2, R3
calls for a cleardown. The answerer sends this R3 for 64 symbols or
more, then clears the call. The caller clears the call when R3 calls
for a cleardown.

## The retrain (10)

A modem retrains when its receiver is not satisfactory. It also
retrains when it receives the tone of the far modem (G or F) for more
than 128 symbols.
The caller sends G, and the answerer sends F, at the symbol rate of the
start-up. Then the start-up continues from the step with G. Circuit 107
stays on.

## The rate renegotiation (11)

A modem changes the rate without a retrain. The procedure is mandatory.

| Step | Initiator | Responder |
|---|---|---|
| 1 | Sends the preamble, then R4 (the rates it wants, and all the lower rates it can use) | Detects the preamble in the data, and receives R4. Continues to send data. |
| 2 | Detects the preamble of the responder, and receives R5 | Sends its preamble, then R5 (its rates, independent of R4) for 64 symbols |
| 3 | After 64 symbols of R4 or more, sends E with the highest common rate | Sends E with the highest common rate |
| 4 | Sends B1 for 24 symbols, then data. The first control segment is 50 symbols after E. | The same |

- The preamble of the caller is AA for 56 symbols, then CC for 8
  symbols. The preamble of the answerer is AC for 56 symbols, then CA
  for 8 symbols.
- R4 and R5 start from a scrambler of zeros. The differential code
  starts from the last symbol of the preamble.
- R4 or R5 can call for a cleardown, or the two can have no common
  rate. Then the modem sends E for 64 symbols or more, and clears the
  call.

## The test loops (12)

The modem must have the test loops 2 and 3 of V.54, in the data-only
mode only.

## V.61 in D-Modem

| Item | D-Modem (`dsp/dp_v61.cpp`) |
|---|---|
| Symbol rates | 3000 symbols/s. 2800 symbols/s when the far modem sends AA, or with `$DSP_V61_BAUD=2800`. |
| Rates | 14400, 12000, 9600, 7200 and 4800 bit/s data only, from the error of the receiver on TRN (`v61_min_snr`). 4800 bit/s with speech when the two modems have a voice path. |
| Speech | `dsp/v61_audio.cpp`: the speech at 8000 Hz, moved down and filtered at the symbol instants (an equivalent method, as the note to 5.5.1.2 permits). Fixed flat pre-emphasis, index 20. |
| Variable rate | Data-only frames after three silent frames, when the rate word 3 of the far modem permits it. D-Modem's rate word 3 permits it. |
| Control segment | As Table 6. The receiver checks the inverted copies only. D: majority of D, the inverse of its copy and the third bits, a tie gives data only. L and G4 to G2: OR with the inverse of the copy. |
| TRN | 2048 symbols (683 ms) |
| Retrain | On the far tone for more than 128 symbols, on a lost equalization (2 s), on bad control segments for 1 s, or on `ATO1` |
| Rate renegotiation | As clause 11, on `ATO3`. A preamble needs 24 symbols in a row on one point (AA), or on two opposite points in turn (AC), at any of the four phases. A retrain if it is not complete in 3 s. |
| Cleardown | R3 (start-up) or E (renegotiation) for 192 symbols, then the call ends |
| Test loops | Loops 2 and 3 of V.54 at the bit level (`core/v54.h`) |
| Not implemented | The auxiliary control channel (AC = 0, C7 to C0 = 01110111), user controlled data-only frames (5.3.3), and synchronous interfaces |

### Deviations

These items do not agree with V.61. They are not corrected.

| Item | D-Modem |
|---|---|
| De-emphasis in the receiver (5.5.2.1.1) | The receiver ignores the pre-emphasis index of the far modem. It operates as if the far modem uses the flat set (index 20). The speech of a far modem with a different set has a wrong frequency response. The data is not changed. |
| Test loops in data-only mode (12) | A loop does not stop the frames with speech. |
| The wait of MT (9.2) | The answerer trains its receiver on the S of the caller at once. It does not wait for the time MT. |

## Corrections (2026-11)

| Error | Correction |
|---|---|
| The receiver checked the synchronization bits of the control segment (the bits marked with "*" in Table 6). A far modem with other states in these bits caused a retrain after 1 s. | The receiver checks only the inverted copies: D, L and G4 to G2 (with speech), D and C7 to C4 (data only). |
| The receiver did not accept a rate word 1 with B13 or B14 set. | The receiver ignores B13 and B14 (Note 1 of Table 10). |
| The caller accepted only an R3 with one rate. | The caller accepts an R3 with more rates, and uses the fastest. |
| When R2 had no common rate, the answerer stopped at once. It sent no R3. The caller did not detect a cleardown R3. | The answerer trains again and sends a cleardown R3, then clears the call. The caller clears the call on a cleardown R3. |
| There was no rate renegotiation (clause 11). | The rate renegotiation of clause 11, from the DTE (`ATO3`) or from the far modem. The DTE can also start a retrain (`ATO1`). |

The tests `V61.ControlDefaultsNotChecked`, `V61.ReservedBitsIgnored`,
`V61.R3WithSeveralRates`, `V61.Cleardown` and `V61.Renegotiation`
(`dsp/test/pump_test.cpp`), and `Core.RetrainOnRequestV61`
(`core/test/core_test.cpp`) check these corrections.
