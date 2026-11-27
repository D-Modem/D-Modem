# V.32: technical details

This page gives the technical details for [V.32](v32.md). The clause
numbers are the clause numbers of V.32 (03/93). This page is our own
description, not a copy of the Recommendation.

## The signal (2)

| Item | Value |
|---|---|
| Carrier | 1800 Hz ± 1 Hz, no pilot tone |
| Frequency offset | The receiver must operate with an offset of up to ± 7 Hz. |
| Modulation rate | 2400 baud ± 0.01 % |
| Rates | 9600 bit/s (optional), 4800 bit/s (mandatory). 2400 bit/s is for further study. |
| Level | As specified in V.2 |
| Spectrum | With ONE bits into the scrambler: 4.5 dB ± 2.5 dB less at 600 Hz and 3000 Hz than the maximum between them |

The transmit rate and the receive rate of a modem are the same.

### Differential code (2.4.1.1, 2.4.2, Table 1)

The first two bits of each symbol, Q1 and Q2, give a change of quadrant.
The bits Y1 Y2 then give the quadrant. The quadrants have the names of
the training points A (Y1 Y2 = 00), B (01), C (11) and D (10).

| Q1 Q2 | Phase change |
|---|---|
| 00 | +90° |
| 01 | 0° |
| 10 | +180° |
| 11 | +270° |

At 4800 bit/s, the modem sends only the points A, B, C and D. At 9600
bit/s without the trellis code, Q3 and Q4 select one of the four points
in the quadrant (Figure 1). Each quadrant is the quadrant of A, turned
by the phase of its training point. In the quadrant of A:

| Q3 Q4 | Point (Re, Im) |
|---|---|
| 00 | (-1, -1) |
| 01 | (-3, -1): A |
| 10 | (-1, -3) |
| 11 | (-3, -3) |

### Trellis code (2.4.1.2, Table 2, Figures 2 and 3)

At 9600 bit/s with the trellis code, the differential code of Table 2
changes Q1 Q2 into Y1 Y2. A systematic convolutional encoder with eight
states makes a redundant bit Y0 from Y1 and Y2. Y0 Y1 Y2 Q3 Q4 select one
of 32 points (Figure 3). The trellis code makes the signal immune to a
phase error of 90°, 180° or 270°.

## Interchange circuits (3)

| Circuit | Rule |
|---|---|
| 106 | After the start-up and a retrain, it follows circuit 105 in 2 ms. |
| 109 | It changes only at the steps of the procedures of clause 5. A level detector cannot tell the far signal from an echo of the near signal. Thus V.32 gives no levels or times. |
| 111, 112 | They select the higher rate (ON) or the lower rate (OFF). |
| 113, 114, 115 | The clocks. The transmit clock can come from the DTE, or follow the receive clock. |

## Scrambler (4)

The scrambler is self-synchronizing. Each direction uses a different
polynomial:

| Modem | Scrambler | Descrambler |
|---|---|---|
| Caller (GPC) | 1 + x<sup>-18</sup> + x<sup>-23</sup> | 1 + x<sup>-5</sup> + x<sup>-23</sup> |
| Answerer (GPA) | 1 + x<sup>-5</sup> + x<sup>-23</sup> | 1 + x<sup>-18</sup> + x<sup>-23</sup> |

On a leased circuit, the users select the caller and the answerer.

## The signals of the start-up (5)

| Signal | Content |
|---|---|
| AA | Point A again and again: a tone at 1800 Hz |
| CC | Point C again and again: AA with its phase turned by 180° |
| AC, CA | A and C in turn: tones at 600 Hz and 3000 Hz. CA is AC with its phase reversed. An even number of symbols each time. |
| S | A and B in turn, for 256 symbols |
| S-bar | C and D in turn, for 16 symbols. The change from S to S-bar gives a time reference. |
| TRN | Scrambled ONE bits at 4800 bit/s, without the differential code. At least 1280 symbols, at most 8192. |
| R1, R2, R3 | A 16-bit rate word again and again, scrambled, at 4800 bit/s with the differential code |
| E | One 16-bit word after the last complete rate word. It gives the rate of the data. |
| B1 | Scrambled ONE bits at the rate of the data, for 128 symbols |

S, S-bar and TRN are together the receiver conditioning signal (5.2).

### TRN (5.2.3)

The scrambler starts at zero, and its input is ONE. For the first 256
symbols, the first bit of each dibit selects A (ZERO) or C (ONE). The
second bit is not sent. After that, each dibit selects a point directly
(Table 5):

| Dibit | Point |
|---|---|
| 00 | A |
| 01 | B |
| 11 | C |
| 10 | D |

The first points of TRN are known. The caller sends CCCCCCCCCAAACCC.
The answerer sends CCCAACCCAACCACC.

### The rate word (5.3, Table 6)

The bits are numbered B0 to B15 in the order of transmission.

| Bits | Meaning |
|---|---|
| B0 to B3 | 0000. In E: 1111. |
| B7, B11, B15 | 1: the synchronization bits |
| B4 | 2400 bit/s (for further study). B4 = 1 with B8 = 1 tells that the modem has V.32 bis. |
| B5 | 4800 bit/s |
| B6 | 9600 bit/s |
| B8 | The trellis code at the fastest rate of B4 to B6 |
| B9 to B14 | 001000: no special modes |
| B4 to B6 = 000 | A request to clear the call (a GSTN cleardown) |

A receiver detects a rate signal when it receives two identical words in
a row. The bits B0 to B3, B7, B11 and B15 must be correct. The differential
code of the rate signal starts from the last point of TRN.

### The start-up sequence (5.4, Figure 4)

| Step | Caller | Answerer |
|---|---|---|
| 1 | Waits for the answer tone (at least 1 s), then sends AA | Sends the V.25 answer tone, then AC |
| 2 | | After at least 128 symbols of AC, and 64 symbols of the 1800 Hz tone, sends CA |
| 3 | 64 ± 2 symbols after the AC to CA reversal arrives, sends CC. Starts a timer. | Starts a timer |
| 4 | | Stops the timer when the AA to CC reversal arrives: the round-trip delay MT. 64 ± 2 symbols later, sends AC again (after an A). |
| 5 | Stops the timer at the second reversal: the round-trip delay NT. Stops. | When the 1800 Hz tone stops, waits 16 symbols |
| 6 | | Sends S, S-bar and TRN, then R1 (its rates) |
| 7 | After R1, sends S for NT, then S, S-bar, TRN and R2 (the rates of R1 that it can receive) | Stops when S arrives. Trains its receiver. |
| 8 | | After R2, sends S, S-bar, TRN and R3 (the rate for the two directions) |
| 9 | After R3, sends E, then B1 and data | After E, sends E, then B1 and data |

The caller turns circuit 109 ON and releases circuit 104 128 symbols
after E arrives. The answerer does this after its 128 symbols of B1. If
R2 or R3 asks for a cleardown, the modems clear the call.

A network echo canceller (G.165) needs 650 ms to learn. The TRN
can be longer for it. The answerer must not disconnect for 3 s after it
starts AC, for a manual caller.

## Retrain (5.5)

Either modem can start a retrain when its reception is bad:

- The caller sends AA, and continues from step 3 of the start-up.
- The answerer sends AC for at least 128 symbols, and continues from
  step 2.

A caller that receives AC (600 Hz or 3000 Hz) for more than 128 symbols
does a retrain. An answerer that receives AA (1800 Hz) for more than 128
symbols does a retrain. Circuit 107 stays ON. Circuit 109 stays ON,
unless AA or the first AC continues for more than 45 s.

## Asynchronous modes (7)

The modem can convert start-stop characters with V.14. The rates are
9600 bit/s and 4800 bit/s, with 8 to 11 bits for each character. The
signalling rate of the characters can be 1 % (basic) or 2.3 % (extended)
faster than the nominal rate.

## Automode (Annex A)

An automode modem can also operate with V.22 bis and V.22 modems:

- The answerer sends the answer tone. If AA comes during the tone, it
  continues with V.32. If not, it sends USB1 of V.22 bis for 1500 ms ±
  50 ms. If S1 or SB1 comes, it continues with V.22 bis. If not, it
  continues with V.32 (AC).
- The caller sends AA after 1 s of answer tone. If AC comes, it
  continues with V.32. USB1 can come for 155 ms ± 10 ms after more than
  800 ms of answer tone. Then the caller stops AA, waits 456 ms, and
  continues with V.22 bis.

## V.32 in D-Modem

| Item | D-Modem (`dsp/dp_v32.cpp`) |
|---|---|
| Rates | 4800 bit/s, 9600 bit/s with the trellis code, 9600 bit/s without it |
| Pulse shape | Root raised cosine, roll-off 0.25 |
| Echo canceller | 128 taps. It learns during our own TRN, while the far modem is silent. |
| TRN | 2048 symbols (853 ms) |
| Turn-round (step 3, step 4) | 63.5 to 65 symbols on the line (`V32.TurnRoundDelay`) |
| AC | Starts with A. CA starts after an even number of symbols (after a C). AC starts again after an A. |
| AA before CA | At least 64 symbols of the 1800 Hz tone |
| Rate words | V.32: B4 = 0, and B8 for the trellis code. The words of V.32 bis are on [the V.32 bis page](v32bis-details.md). |
| Rate selection | From the error of the receiver on TRN. Without B8 from the far modem, only 4800 bit/s and 9600 bit/s without the trellis code. |
| Cleardown | An R2 that asks for a cleardown gets an R3 that asks for one, for 64 symbols. Then the call ends. An R3 that asks for one ends the call at the caller. |
| B1 | 128 symbols. The receiver finds the state of the far trellis encoder in the first 8 symbols of B1. |
| Retrain | After 2 s with the equalizer error above the limit of the rate, or when the far modem retrains (AA or AC for more than 128 symbols). Circuit 104 is held at ONE. |
| Automode, caller | V.22 bis after 100 ms of USB1. V.21 or V.23 after 300 ms of their mark. |
| Automode, answerer | V.22 bis after 3 s of AC without AA |
| Not implemented | Leased circuits, the test loops of V.54, the special echo canceller sequence, the answerer's USB1 of Annex A |

The answerer completes the current word of R1 before it stops for the S
of the caller (at most 7 symbols). It does not wait for MT, because its
receiver trains on S whenever S comes.

### Corrections (2026-11)

| Error | Correction |
|---|---|
| The caller sent CC, and the answerer sent AC again, 53 to 54 symbols after the reversal arrived (64 ± 2 required). The code expected 26 symbols of delay in its detector. The delay is about 12 symbols. | `V32_TURN_SYMS` (49 symbols after the detector) puts the transition on the line 63.5 to 65 symbols after the reversal. |
| AC could start with C. CA could start after an odd number of symbols, and AC again after a C. The answerer sent CA after 48 symbols of AA (64 required). | AC starts with A. The reversals come after C (to CA) and after A (to AC). CA waits for 64 symbols of the tone. |
| There was no 9600 bit/s without the trellis code. A modem without the trellis code got 4800 bit/s. | The 16 points of Figure 1, with the differential code of Table 1. A far word without B8 selects them. |
| A rate word without a rate (a cleardown) was not detected. The modems waited until the time limit. | The cleardown is detected. The answerer sends a cleardown R3. |
| In V.32 (`AT+MS=32`), the rate words had B4 = 1. With B8 = 1, this told the far modem that we have V.32 bis. | B4 = 0 in V.32. |

The tests `V32.TurnRoundDelay`, `V32.UncodedRate`, `V32.Cleardown` and
`V32.RateWords` (`dsp/test/pump_test.cpp`) check these corrections.
