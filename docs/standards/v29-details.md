# V.29: technical details

This page gives the technical details for [V.29](v29.md). The clause
numbers are the clause numbers of V.29 (11/1988). This page is our own
description, not a copy of the Recommendation.

## The signal (2, 3, 4)

| Item | Value |
|---|---|
| Carrier | 1700 Hz ± 1 Hz, no pilot tone |
| Modulation rate | 2400 baud ± 0.01 % |
| Rates | 9600, 7200 and 4800 bit/s ± 0.01 % |
| Level | As specified in V.2 |
| Frequency offset | The receiver must operate with an error of at least ± 7 Hz. |
| Spectrum (11) | Linear phase from 700 Hz to 2700 Hz. At 500 Hz and 2900 Hz, 4.5 dB ± 2.5 dB less than the maximum. |

### Line code at 9600 bit/s (2.2.1)

The scrambled data goes in groups of four bits (quadbits) Q1 Q2 Q3 Q4.
Q2, Q3 and Q4 give the phase change from the previous symbol, as in
V.27:

| Q2 Q3 Q4 | Phase change |
|---|---|
| 001 | 0° |
| 000 | 45° |
| 010 | 90° |
| 011 | 135° |
| 111 | 180° |
| 110 | 225° |
| 100 | 270° |
| 101 | 315° |

Q1 and the new absolute phase give the amplitude:

| Absolute phase | Q1 = 0 | Q1 = 1 |
|---|---|---|
| 0°, 90°, 180°, 270° | 3 | 5 |
| 45°, 135°, 225°, 315° | √2 | 3√2 |

### Fallback rates (2.2.2, 2.2.3)

- **7200 bit/s.** Three data bits give Q2, Q3 and Q4. Q1 is always ZERO.
- **4800 bit/s.** Two data bits give Q2 and Q3. Q1 is ZERO, and Q4 is
  the inverse of Q2 + Q3 (modulo 2). The phase changes are 0°, 90°, 180°
  and 270° (V.26 alternative A), at the amplitude 3.

## Interchange circuits (5)

### Circuit 109 (5.2)

| Item | Value |
|---|---|
| ON | Above -26 dBm |
| OFF | Below -31 dBm |
| Hysteresis | At least 2 dB |
| ON to OFF | 30 ms ± 9 ms |
| OFF to ON, first equalization | Before data goes to circuit 104 |
| During a new equalization in data | Stays ON. Circuit 104 can be held at ONE. |
| OFF to ON after an interruption, no new equalization | 15 ms ± 10 ms |
| OFF to ON after an interruption, new equalization | Before data goes to circuit 104 |

These levels are for leased circuits of special quality (M.1020,
M.1025). Choose the OFF time so that all valid data bits go to circuit
104.

### Circuit 106 (5.3)

| Circuit 105 | Circuit 105 ON to circuit 106 ON |
|---|---|
| Does not control the carrier | 15 ms ± 5 ms |
| Starts the carrier and a synchronizing signal | 253.5 ms ± 0.5 ms |

Circuit 106 goes OFF after the last valid symbol is sent.

### Other circuits

Circuit 111 selects one of two rates. A manual selector gives the pair:
9600/7200, 9600/4800 or 7200/4800. The modem gives the clocks on
circuits 114 and 115. It can also take the transmit clock from circuit
113. Or it can send on the receive clock (loopback timing). The
electrical characteristics are the characteristics of V.28.

## The synchronizing signal (8)

| Segment | Content | Symbols | Time |
|---|---|---|---|
| 1 | Silence | 48 | About 20 ms |
| 2 | Alternations ABAB… | 128 | About 53 ms |
| 3 | Equalizer pattern of C and D | 384 | About 160 ms |
| 4 | Scrambled ONE bits | 48 | About 20 ms |
| Total | | 608 | About 253 ms |

The symbol counts are exact. The times are approximate.

| Point | 9600 bit/s | 7200 bit/s | 4800 bit/s |
|---|---|---|---|
| A (segment 2) | Amplitude 3 at 180°: it sets the absolute phase | The same | The same |
| B (segment 2) | (3, -3) | (1, -1) | (0, -3) |
| C (segment 3) | Amplitude 3 at 0° | The same | The same |
| D (segment 3) | (-3, 3) | (-1, 1) | (0, 3) |

Segment 3 sends C for a ZERO and D for a ONE of the sequence of 1 +
x<sup>-6</sup> + x<sup>-7</sup>. The generator starts at 0101010 and
runs at the symbol rate. Thus, segment 3 starts with CDCDCDC. After
segment 4, circuit 106 goes ON and the data starts.

## Scrambler (9)

The scrambler is self-synchronizing, with the polynomial 1 +
x<sup>-18</sup> + x<sup>-23</sup>. Its register gets ZERO bits during
segments 1, 2 and 3. Thus, it starts segment 4 from a known state.

## Equalizer and resynchronization (10)

- The receiver has an automatic adaptive equalizer.
- When the receiver loses equalization, the modem sends a synchronizing
  signal, and expects one back.
- When the receiver detects a synchronizing signal from the far modem,
  the modem sends one back. It can do this at any time in the received
  signal, whatever the state of circuit 105.
- If no synchronizing signal comes back in 1.2 s, the modem sends
  another. On double satellite connections, 1.8 s to 2.6 s is better.
- If the modem cannot synchronize on the received signal, it sends
  another synchronizing signal.
- A modem that receives a synchronizing signal that it did not ask for,
  and synchronizes correctly, sends back only one.

## Multiplexer (12)

As an option, the modem combines data channels of 7200, 4800 and 2400
bit/s into one stream. The position in the quadbit identifies each
channel. Each port has a buffer for small differences of phase and
frequency.

## V.29 in D-Modem

| Item | D-Modem (`dsp/fax_modems.cpp`, `dsp/dp_4wire.cpp`) |
|---|---|
| Use | The fax pages (T.30, Class 1), and the 4-wire pump |
| Rates | 9600, 7200 and 4800 bit/s. The 4-wire receiver tries all three on each synchronizing signal. |
| Line code and points | As in Tables 1, 2 and 3. The points A, B, C and D agree with Figure 4. |
| Synchronizing signal | 48, 128, 384 and 48 symbols. Segment 3 from 0101010. The scrambler starts segment 4 at ZERO. |
| Receiver | It finds segment 3 by correlation with a local copy. |
| Circuit 109, 4-wire | ON at -27 dBm0. OFF at -30 dBm0, or 12 dB below the signal. OFF in 24 ms to 34 ms. |
| Circuit 109, fax | The fax detector: ON at -43.4 dBm0, OFF in 5 ms to 14.5 ms |
| Resynchronization, 4-wire | After 0.3 s of a high equalizer error, or on the far synchronizing signal. Again after 1.2 s without an answer. |
| After an interruption | Always a new synchronization. Circuit 104 is held at ONE until the data. |
| Not implemented | The multiplexer, controlled carrier on the 4-wire pump, loopback timing |

The fax detector has a lower ON level than V.29. A fax call goes over
the switched network, where the received level can be much lower than
on a leased circuit. V.29 does not give levels for the switched network.

### Corrections (2026-11)

| Error | Correction |
|---|---|
| The 4-wire pump used the detector of the fax receiver. It came ON at -43.4 dBm0 (it must be OFF below -31 dBm), and it went OFF in 5 ms to 14.5 ms (30 ms ± 9 ms required). | `FaxRx::set_detector` sets the levels and an OFF time. The 4-wire V.29 receiver is ON at -27 dBm0 and OFF at -30 dBm0, with 20 ms more before OFF. |

The test `V29.Circuit109` (`dsp/test/fax_test.cpp`) measures the levels
and the OFF time of the 4-wire detector.
