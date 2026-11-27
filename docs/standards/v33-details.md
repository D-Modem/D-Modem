# V.33: technical details

This page gives the technical details for [V.33](v33.md). The clause
numbers are the clause numbers of V.33 (11/88). This page is our own
description, not a copy of the Recommendation.

## The signal (2, 3, 4)

| Item | Value |
|---|---|
| Carrier | 1800 Hz ± 1 Hz |
| Frequency offset | The receiver must operate with an offset of up to ± 7 Hz. |
| Rates | 14400 bit/s and 12000 bit/s ± 0.01 % |
| Modulation rate | 2400 baud ± 0.01 % |
| Level | As specified in V.2 |

At 14400 bit/s, each symbol carries six bits. Q1 and Q2 go through the
differential code of Table 1A. The convolutional encoder of Figure 1
makes Y0 from Y1 and Y2. Y0, Y1, Y2 and Q3 to Q6 select one of 128
points (Figure 2). At 12000 bit/s, Q6 is removed and the modem uses 64
points (Figure 3).

## Interchange circuits (5)

| Circuit | Rule |
|---|---|
| 109, threshold (5.2) | ON above -26 dBm. OFF below -33 dBm. The ON level is at least 2 dB above the OFF level. |
| 109, time (5.2) | OFF to ON: 15 ms ± 5 ms when no new equalization is necessary. ON to OFF: 40 ms ± 10 ms. During a new equalization in data, circuit 109 stays ON. |
| 106 | OFF to ON: 15 ms ± 5 ms |

## Scrambler (7, Annex)

The scrambler is self-synchronizing, with the polynomial 1 +
x<sup>-18</sup> + x<sup>-23</sup>. It runs at 4800 bit/s during segments
2 and 3, and at the rate of the data during segment 4.

## The synchronizing signal (8, Table 3)

| Segment | Content | Symbols |
|---|---|---|
| 1 | A and B in turn | 256 |
| 2 | The equalizer conditioning pattern | 2976 |
| 3 | The rate signal | 64 |
| 4 | Scrambled ONE bits at the rate of the data | 48 |

The total is 3344 symbols (1393 ms).

- Segment 2: scrambled ONE bits at 4800 bit/s, without the differential
  code. 00 is C, 01 is D, 11 is A, 10 is B. The first points are
  CDCDCDCDCDCDBDBD.
- Segment 3: a 16-bit rate word, scrambled, sent eight times at 4800
  bit/s. The differential code of Table 1B starts from the last symbol
  of segment 2.
- Segment 4: the differential code starts from the first symbol of
  segment 3. The convolutional encoder starts at zero. At the end,
  circuit 106 goes ON and the data starts.

### The rate word (8.3, Table 4A)

| Bits | Meaning |
|---|---|
| B0 to B3 | 0000 |
| B7, B11, B15 | 1: the synchronization bits |
| B14 | 0: the word gives only rates (Table 4A). 1: the word gives the multiplexer configuration (Table 4B). |
| B8 | 1: the modem can send and receive 12000 bit/s |
| B9 | 1: the modem can send and receive 14400 bit/s |
| B8 B9 | 11 or 01: segment 4 is at 14400 bit/s. 10: segment 4 is at 12000 bit/s. |

A receiver detects the rate signal when it receives two identical words
in a row. In each word, the bits B0 to B3, B7, B11 and B15 must be
correct.

## Training and retraining (9)

- The receiver has an automatic adaptive equalizer.
- When the receiver loses equalization, or when the rate changes, the
  modem sends a synchronizing signal and expects one back.
- When the receiver detects a far synchronizing signal, the modem sends
  one back. It can start at any time in the received signal.
- If no synchronizing signal comes back in 1.2 s, the modem sends
  another.
- The modem sends a new synchronizing signal at the fastest common rate
  in these conditions: it cannot synchronize, it cannot detect the rate
  signal, it cannot supply the rate, or it sends faster than the far
  rate. It does not do this if it sent one in the last 1.2 s.
- A modem that receives a synchronizing signal that it did not ask for,
  and synchronizes correctly, sends back only one.

## Multiplexer (10)

As an option, the modem combines data channels of 12000, 9600, 7200,
4800 and 2400 bit/s into one stream. The rate word (Table 4B) selects
the configuration.

## V.33 in D-Modem

| Item | D-Modem (`dsp/dp_4wire.cpp`) |
|---|---|
| Use | The 4-wire pump (`AT&L2`, `AT+MS=V33`), duplex with a continuous carrier |
| Signal | The V.17 transmitter and receiver of `dsp/fax_modems.cpp`, with the long train. Segment 3 sends the rate word in place of the bridge word of V.17. |
| Rate words | 0x8B80 (14400 bit/s: B8 B9 = 11) and 0x8980 (12000 bit/s: B8 B9 = 10), B0 first. B14 = 0. |
| Rate | The transmitter starts at the fastest rate of `+MS`. The receiver gets the rate from segment 3. A modem that sends faster than the far modem comes down to the far rate. |
| Circuit 109 | ON at -27 dBm0. OFF at -32 dBm0, or 12 dB below the signal. OFF 30 ms to 50 ms after the signal stops. |
| Resynchronization | After a high equalizer error, or on the far synchronizing signal. Again after 1.2 s without an answer. Circuit 104 is held at ONE until the data. |
| Not implemented | The multiplexer and Table 4B |

### Corrections (2026-11)

| Error | Correction |
|---|---|
| The 4-wire V.33 receiver used the detector of the fax receiver. It came ON at -43.4 dBm0 (it must be OFF below -33 dBm), and it went OFF in 5 ms to 14 ms (40 ms ± 10 ms required). | `V33_ON_AMP`, `V33_OFF_AMP` and `V33_OFF_MS` set the detector: ON at -27 dBm0, OFF at -32 dBm0, with 30 ms more before OFF. |

The test `V33.Circuit109` (`dsp/test/fax_test.cpp`) measures the levels
and the OFF time. The tests of `dsp/test/leased_call_test.cpp` make V.33
calls on a 4-wire line.
