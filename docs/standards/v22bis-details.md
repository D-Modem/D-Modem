# V.22 bis: technical details

This page gives the technical details for [V.22 bis](v22bis.md). The
clause numbers are the clause numbers of V.22 bis (11/1988). This page is
our own description, not a copy of the Recommendation.

## The signal (2)

The carriers, the modulation rate, the shaping, the guard tones and the
frequency tolerance are the same as [V.22](v22-details.md). The 550 Hz
guard tone is 3 dB below the data. The compromise equalizer is in the
transmitter.

### The constellation (2.5.2)

At 2400 bit/s, the data is divided into quadbits. The first two bits
give the change of quadrant:

| First two bits | Quadrant change |
|---|---|
| 00 | +90° (1 to 2, 2 to 3, 3 to 4, 4 to 1) |
| 01 | 0° |
| 11 | +270° |
| 10 | +180° |

The last two bits select one of the four points in the quadrant. In each
quadrant, the four points are on a square grid at ±1 and ±3. The
constellation is turned 90° from one quadrant to the next. Thus, the
same last two bits always give the same point, relative to the quadrant.

At 1200 bit/s, the modem uses only the point "01" of each quadrant.
This is the signal of V.22.

## Circuit 109 (3.2, 3.3)

| Item | Value |
|---|---|
| ON | Above -43 dBm |
| OFF | Below -48 dBm |
| Hysteresis | 2 dB at least |
| ON to OFF | 40 ms to 65 ms after the level falls below the threshold. At 1200 bit/s, the time can be 10 ms to 24 ms, as in V.22. |
| OFF to ON after a dropout | 40 ms to 205 ms after the level rises above the threshold |
| Circuit 106 | Follows circuit 105 in 3.5 ms |

The thresholds apply to a signal of scrambled ONE bits. Circuit 109 must
not respond to the guard tones or to the answer tone.

## The scrambler (5)

The scrambler is the same as V.22: 1 + x<sup>-14</sup> + x<sup>-17</sup>.
The inversion after 64 ONE bits operates all the time, also during the
handshake and the retrain. The counter starts again after each
inversion.

## The handshake at 2400 bit/s (6.3.1.1)

S1 is unscrambled dibits 00 and 11, one after the other, at 1200 bit/s,
for 100 ms ± 3 ms.

### Call modem

1. Receive in the high channel at 1200 bit/s. Stay silent.
2. Detect unscrambled ONE bits for 155 ms ± 10 ms.
3. Wait 456 ms ± 10 ms. Send S1. Then send scrambled ONE bits at 1200
   bit/s.
4. If S1 comes from the answer modem, set circuit 112 ON at the end of
   that S1.
5. 600 ms ± 10 ms after circuit 112 comes ON, send scrambled ONE bits at
   2400 bit/s. The receiver can use 16 points from 450 ms ± 10 ms.
6. After 200 ms ± 10 ms of scrambled ONE bits at 2400 bit/s, let circuit
   106 follow circuit 105.
7. After 32 bits of scrambled ONE bits at 2400 bit/s from the answer
   modem, set circuit 109 ON.

If the call modem receives scrambled ONE bits at 1200 bit/s for 270 ms
and no S1, the answer modem is a V.22 modem. Then the handshake of V.22
continues.

### Answer modem

1. After the answer tone, send unscrambled ONE bits at 1200 bit/s.
2. If S1 comes from the call modem, set circuit 112 ON at the end of that
   S1. Then send S1 and scrambled ONE bits at 1200 bit/s.
3. Do steps 5 to 7 of the call modem, with the same times.

If the answer modem receives scrambled ONE bits for 270 ms and no S1,
the call modem is a V.22 modem. Then the handshake of V.22 continues.

## Retrain (6.4)

1. A modem detects loss of equalization. Or it detects the end of S1
   from the far modem.
2. The modem sets circuit 106 OFF. It sends S1 for 100 ms, then scrambled
   ONE bits at 1200 bit/s.
3. 600 ms after the end of the S1 from the far modem, it sends scrambled
   ONE bits at 2400 bit/s.
4. After 200 ms, circuit 106 follows circuit 105.
5. After 32 bits of scrambled ONE bits at 2400 bit/s, the receiver is
   ready.

If no S1 comes back in 1.2 s, the modem starts the retrain again.
Circuits 107 and 109 stay ON during the retrain.

## After a loss of the line signal (6.5)

1. Set circuit 109 OFF. Hold circuit 104 at ONE.
2. When the signal comes back, set circuit 109 ON, but hold circuit 104
   for 100 ms more.
3. If a retrain starts in those 100 ms, do the retrain. If not, release
   circuit 104.

## Rate change (6.6, optional)

The modem that starts the change sends S1 for 100 ms, then scrambled
dibits R1 at 1200 bit/s. R1 is `11` for 2400 bit/s, and `01` or `10`
for 1200 bit/s. The far modem replies with S1 and R2. Then each modem
uses the rate of R2. The times are 450 ms and 600 ms, as in the
handshake.

## Tests (7)

The remote loop 2 has the same signals as V.22, at the current rate. The
far modem detects the end of the signal in 40 ms to 65 ms.

## V.22 bis in D-Modem

| Item | D-Modem (`dsp/dp_v22.cpp`) |
|---|---|
| S1 detection | 40 ms of S1 (`run_s1`). The end of S1 is the last symbol of a run of 10 ms or more. Scrambled data cannot make a run of this length. |
| 2400 bit/s | 600 ms after the end of the far S1, then 200 ms of scrambled ONE bits |
| 16-point receiver | After 40 ms of scrambled ONE bits at 2400 bit/s |
| Circuit 109 | `Circuit109`: ON to OFF in 45 ms at 2400 bit/s (46 ms to 55 ms in the tests). After a dropout, ON after 110 ms. |
| Fast connect | A D-Modem extension (`fast`), not part of V.22 bis. It keeps its own short times. |
| Remote loop 2 (7.1) | As [V.22](v22-details.md#v22-in-d-modem), at the data rate (2400 bit/s or 1200 bit/s). Modem B finds the gap after 45 ms. |
| Not implemented | The rate change of 6.6 |

### Corrections (2026-11)

| Error | Correction |
|---|---|
| The answer modem sent its S1 when it detected the S1 of the call modem. V.22 bis requires the S1 at the end of the received S1 (6.3.1.1.2 b). | The answer modem waits for the end of the S1 of the call modem. |
| The two modems timed the 600 ms from the detection of the far S1 (40 ms into it). V.22 bis times it from the end of the far S1. Thus, the change to 2400 bit/s came about 60 ms early. | The two modems time the 600 ms from the end of the far S1. |
| The carrier detector had the errors of [V.22](v22-details.md#corrections-2026-11). It took up to about 180 ms to turn OFF. V.22 bis requires 40 ms to 65 ms. | The same `Circuit109` as V.22, with 45 ms at 2400 bit/s. |
| Remote loop 2 used the patterns of V.54 clause 6. V.22 bis 7.1 says that its signals are not compatible with V.54. | The signals of 7.1, as in [V.22](v22-details.md#corrections-2026-11). |

The test `V22.Circuit109` (`dsp/test/pump_test.cpp`) measures the levels
and the times.
