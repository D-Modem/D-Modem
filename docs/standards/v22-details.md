# V.22: technical details

This page gives the technical details for [V.22](v22.md). The clause
numbers are the clause numbers of V.22 (11/1988). This page is our own
description, not a copy of the Recommendation.

## The signal (2)

| Item | Value |
|---|---|
| Carrier, low channel (call modem) | 1200 Hz ± 0.5 Hz |
| Carrier, high channel (answer modem) | 2400 Hz ± 1 Hz |
| Modulation rate | 600 baud ± 0.01% |
| Bit rates | 1200 bit/s (2 bits a symbol) or 600 bit/s (1 bit a symbol) |
| Shaping | Square root of a raised cosine, 75% roll-off |
| Group delay | ± 150 µs in 900 Hz to 1500 Hz and in 2100 Hz to 2700 Hz |
| Receiver frequency tolerance | ± 7 Hz at least |
| Guard tone | 1800 Hz ± 20 Hz, 6 dB below the data, only from the high channel. A country can disable it or use 550 Hz. |
| Equalizer | Fixed compromise equalizer, half in the transmitter and half in the receiver |

### The phase changes (2.5.2)

At 1200 bit/s, the scrambled data is divided into dibits. The first bit
of the dibit is the first bit from the scrambler.

| Dibit | 600 bit/s bit | Phase change (modes i to iv) | Phase change (mode v) |
|---|---|---|---|
| 00 | 0 | +90° | +270° |
| 01 | | 0° | +180° |
| 11 | 1 | +270° | +90° |
| 10 | | +180° | 0° |

## The scrambler (5)

- The scrambler and the descrambler are self-synchronizing, with the
  polynomial 1 + x<sup>-14</sup> + x<sup>-17</sup>.
- If the output of the scrambler has 64 ONE bits in a sequence, the
  transmitter inverts the next input bit. This prevents a lock-up that
  can start the remote loop 2 by accident. This function is off during
  the handshake.
- The descrambler can do the same in reverse.

## Data modes (4)

| Alternative | Modes |
|---|---|
| A | i) 1200 bit/s synchronous. iii) 600 bit/s synchronous (optional). |
| B | As A, and ii) 1200 bit/s start-stop and iv) 600 bit/s start-stop (optional). |
| C | As B, and v): start-stop at 1200 bit/s, or any data at up to 300 bit/s. |

Start-stop characters have 8, 9, 10 or 11 bits. V.14 converts them to
synchronous bits. In mode v), the line rate is 1205 bit/s (or 1223 bit/s
as an option). The transmitter samples the input, so data at up to
300 bit/s goes through with no buffer.

## Circuits 106 and 109 (3.2, 3.3)

| Circuit | Constant carrier | Controlled carrier |
|---|---|---|
| 106, OFF to ON | 2 ms or less | 210 ms to 275 ms |
| 106, ON to OFF | 2 ms or less | 2 ms or less |
| 109, OFF to ON | 105 ms to 205 ms | 105 ms to 205 ms |
| 109, ON to OFF | 10 ms to 24 ms | 10 ms to 24 ms |

The carrier detector (circuit 109) turns ON above -43 dBm and OFF below
-48 dBm. It has at least 2 dB of hysteresis. It must not respond to the
guard tone or to the answer tone.

## The handshake (6.3)

On the switched network, the call modem sends in the low channel and the
answer modem sends in the high channel.

### Call modem (6.3.1.1)

1. Connect to the line and receive in the high channel. Stay silent.
2. Detect unscrambled ONE bits for 155 ms ± 50 ms.
3. Wait 456 ms ± 10 ms. Then send scrambled ONE bits in the low channel.
4. Detect scrambled ONE bits in the high channel for 270 ms ± 40 ms. Then
   set circuit 109 ON.
5. Wait 765 ms ± 10 ms more. Then let circuit 106 follow circuit 105.

### Answer modem (6.3.1.2)

1. After the answer tone of V.25, receive in the low channel.
2. Send unscrambled ONE bits.
3. Detect scrambled ONE or ZERO bits in the low channel for 270 ms ± 40
   ms. Then send scrambled ONE bits.
4. Wait 765 ms ± 10 ms. Then set circuit 109 ON and let circuit 106
   follow circuit 105.

In Alternative C, the call modem sends scrambled ZERO bits in step 3 to
ask for mode v). The answer modem then replies in mode v).

A loss of the line signal after the handshake must not start a new
handshake.

### Controlled carrier (6.3.2)

The modem sends a ONE pattern when circuit 105 comes ON. Circuit 106
comes ON 210 ms to 275 ms later. The far modem sets circuit 109 ON after
105 ms to 205 ms. Do not use controlled carrier on lines with echo
suppressors.

## Tests (7)

V.22 has the test loops 2 and 3 of V.54, but with its own signals for the
remote loop 2:

1. Modem A sends unscrambled ONE bits.
2. Modem B detects them for 154 ms to 231 ms. It sends scrambled
   reversals (alternate ONE and ZERO).
3. Modem A detects the reversals for 231 ms to 308 ms. It sends
   scrambled ONE bits.
4. Modem B closes loop 2.
5. To stop the loop, modem A stops its signal for 77 ms ± 10 ms.

## V.22 in D-Modem

| Item | D-Modem (`dsp/dp_v22.cpp`) |
|---|---|
| Modes | V.22 (1200 bit/s), V.22 bis (2400 bit/s) and Bell 212A, synchronous at the pump, start-stop through the data link layer |
| Transmitter | `QamTx` with a root raised cosine of roll-off 0.75 |
| Receiver | Band-pass, down conversion, matched filter, AGC, Gardner timing loop, T/2 LMS equalizer and a carrier PLL |
| Guard tone | None (a national option of 2.1) |
| Scrambler | `V22Scrambler` (`dsp/dsp_util.cpp`), with the inversion after 64 ONE bits |
| Handshake | 155 ms of unscrambled ONE bits, 456 ms, 270 ms, 765 ms |
| Circuit 109 | `Circuit109` (`dsp/dsp.h`): ON above about -43.4 dBm0, OFF below about -47 dBm0. ON to OFF in 11 ms (12 ms to 21 ms in the tests). After a dropout in data, ON after 110 ms. |
| Remote loop 2 (7.1) | The pump sends and identifies the signals of 7.1 in data (`loop2_cmd()`, `loop2_symbol()`, `loop2_level()`). Modem B identifies 190 ms of unscrambled ONE bits, and closes the loop 20 ms after they stop. Modem A identifies 270 ms of reversals, then 270 ms of its scrambled ONE bits. It stops after 3 s with no answer. To end the loop, modem A sends no signal for 77 ms. Modem B finds the gap after 17 ms at the output of the matched filter (20 dB below the line signal), and opens the loop 110 ms after the signal comes back. Bell 212A does not have this loop. |
| Not implemented | 600 bit/s, mode v), controlled carrier |

### Corrections (2026-11)

A check against 3.2 and 3.3 found three errors in the carrier detector:

| Error | Correction |
|---|---|
| The detector turned ON at about -41.9 dBm0. V.22 requires ON above -43 dBm. | ON at about -43.4 dBm0, OFF at about -47 dBm0. |
| After the far carrier stopped, the detector stayed ON for up to about 180 ms. V.22 requires 10 ms to 24 ms. | A fast power meter (1.2 ms) finds the end of the carrier. The detector turns OFF when this meter stays below the OFF level for 11 ms. |
| After a dropout in data, the detector turned ON again immediately. V.22 requires 105 ms to 205 ms. | The detector waits 110 ms. |

The test `V22.Circuit109` (`dsp/test/pump_test.cpp`) measures the levels
and the times.

A check against 7.1 found one more error:

| Error | Correction |
|---|---|
| Remote loop 2 used the patterns of V.54 clause 6 (scrambled 0s, scrambled 1s, and 8192 bits of TERM). V.22 7.1 says that its signals are not compatible with V.54. | The pump sends and identifies the signals of 7.1: unscrambled ONE bits, scrambled reversals, scrambled ONE bits, and 77 ms without a line signal at the end. The core asks the pump for the loop (`dp_runtime_loop2()`). |

The test `Leased/V22Loop2.Signals` (`dsp/test/leased_call_test.cpp`)
finds the unscrambled ONE bits and the 77 ms gap in the signal of
modem A. Then it sends data both ways.
