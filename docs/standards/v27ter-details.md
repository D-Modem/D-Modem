# V.27 ter: technical details

This page gives the technical details for [V.27 ter](v27ter.md). The
clause numbers are the clause numbers of V.27 ter (11/1988). This page
is our own description, not a copy of the Recommendation.

## The signal (2)

| Item | 4800 bit/s | 2400 bit/s |
|---|---|---|
| Carrier | 1800 Hz ± 1 Hz, no pilot tone | The same |
| Modulation rate | 1600 baud ± 0.01 % | 1200 baud ± 0.01 % |
| Bits a symbol | 3 (a tribit) | 2 (a dibit) |
| Spectrum | 50 % raised cosine, divided equally between the transmitter and the receiver. At 1000 Hz and 2600 Hz, 3 dB ± 2 dB less than the maximum. | 50 % raised cosine or more. At 1200 Hz and 2400 Hz, 3 dB ± 2 dB less than the maximum. |
| Linear phase (11) | 1100 Hz to 2500 Hz | 1300 Hz to 2300 Hz |

The level is as specified in V.2. With the backward channel, divide the
power equally between the two channels. The receiver must operate with
a frequency error of at least ± 7 Hz.

### Line code at 4800 bit/s (2.3)

Each tribit is a phase change from the previous symbol. The first bit of
the tribit is the first bit after the scrambler.

| Tribit | Phase change |
|---|---|
| 001 | 0° |
| 000 | 45° |
| 010 | 90° |
| 011 | 135° |
| 111 | 180° |
| 110 | 225° |
| 100 | 270° |
| 101 | 315° |

### Line code at 2400 bit/s (2.4)

| Dibit | Phase change |
|---|---|
| 00 | 0° |
| 01 | 90° |
| 11 | 180° |
| 10 | 270° |

## The turn-on sequence (2.5.1)

The modem sends the turn-on sequence from circuit 105 ON to circuit 106
ON. It has five segments.

| Segment | Content | With talker echo protection | Without protection |
|---|---|---|---|
| 1 | Unmodulated carrier | 185 ms to 200 ms | None |
| 2 | Silence | 20 ms to 25 ms | None |
| 3 | 180° phase changes | a) 14 symbols, b) 50 symbols | The same |
| 4 | Equalizer pattern: 0° or 180° | a) 58 symbols, b) 1074 symbols | The same |
| 5 | Scrambled ONE bits | 8 symbols | The same |

Sequence b) (long) is for the first transmission after circuit 107 goes
ON. Sequence a) (short) is for each turn-around after that.

| Total time | 4800 bit/s | 2400 bit/s |
|---|---|---|
| a) with protection | 265 ms | 281 ms |
| b) with protection | 923 ms | 1158 ms |
| a) without protection | 50 ms | 66 ms |
| b) without protection | 708 ms | 943 ms |

Segment 4 comes from the scrambler of clause 9 (1 + x<sup>-6</sup> +
x<sup>-7</sup>) with ONE bits at its input. The modem uses each third bit
of the scrambler output: ZERO gives 0°, ONE gives 180°. The scrambler
starts with 0011110 in its first seven stages. Thus, segment 4 starts
with 0°, 180°, 180°, 180°, 180°, 180°, 0°. Table 4 gives the start and
the end of segments 4 and 5 for the two rates. After segment 5, circuit
106 goes ON and the data starts.

## The turn-off sequence (2.5.2)

After circuit 105 goes OFF:

| Segment | Content | Time |
|---|---|---|
| A | The rest of the data, then scrambled ONE bits | 5 ms to 10 ms |
| B | Silence | 20 ms |
| Total | | 25 ms to 30 ms |

If circuit 105 goes ON again during the turn-off, the modem waits until
the end of the turn-off. As an option, the modem can start a turn-on
sequence less than 20 ms after it receives segment A from the far modem.

## Backward channel (4)

The backward channel is the backward channel of [V.23](v23-details.md):
75 baud, 390 Hz for ONE and 450 Hz for ZERO. A faster backward channel
on the same frequencies is permitted.

## Interchange circuits (5)

| Circuit | OFF to ON | ON to OFF |
|---|---|---|
| 106 at 4800 bit/s | a) 50 ms, b) 708 ms, plus 215 ms ± 10 ms with protection | 2 ms or less |
| 106 at 2400 bit/s | a) 67 ms, b) 944 ms, plus 215 ms ± 10 ms with protection | 2 ms or less |
| 109 | After the synchronization, before data goes to circuit 104. Not during the unmodulated carrier, with protection. | 5 ms to 15 ms |
| 121 | 80 ms to 160 ms | 2 ms or less |
| 122 | Less than 80 ms | 15 ms to 80 ms |

The detectors of circuits 109 and 122 are ON above -43 dBm and OFF below
-48 dBm, with at least 2 dB of hysteresis. On a line with known
conditions, the operator can make them less sensitive (for example, -33
dBm and -38 dBm).

In half duplex, the modem holds circuit 104 at ONE and circuit 109 OFF
while circuit 105 is ON. It can continue for 150 ms ± 25 ms after
circuit 105 goes OFF.

The modem gives the clocks on circuits 114 and 115. The DTE can give the
transmit clock on circuit 113.

## Scrambler (9)

The scrambler is self-synchronizing, with the polynomial 1 +
x<sup>-6</sup> + x<sup>-7</sup>. It has guards against repeated
patterns of 1, 2, 3, 4, 6, 8, 9 and 12 bits. This is the scrambler of
V.27, with one more guard for 8-bit patterns. Appendix I gives an
example circuit.

## Other items (6 to 11)

- The receiver has an automatic adaptive equalizer.
- The electrical characteristics are the characteristics of V.28.
- If the echo suppressors must be disabled, use the procedure of
  [V.25](v25.md).
- The operator must not adjust the send level or the receive
  sensitivity.

## V.27 ter in D-Modem

| Item | D-Modem (`dsp/fax_modems.cpp`) |
|---|---|
| Use | The fax pages (T.30, Class 1), and the 4-wire pump (`dsp/dp_4wire.cpp`) |
| Turn-on sequence | Sequence b) with talker echo protection, for each transmission |
| Segments 1 and 2 on the line | 196 ms and 22 ms at 4800 bit/s, 198 ms and 20.5 ms at 2400 bit/s |
| Segments 3, 4 and 5 | 50, 1074 and 8 symbols. The phase changes agree with Table 4 at the two rates. |
| After segment 5 | Scrambled ONE bits until 100 ms, then the data. On the line, this is the same as a DTE that sends ONE bits. |
| Turn-off on the line | Segment A 7.9 ms and segment B 21.4 ms at 4800 bit/s; 10.0 ms and 19.0 ms at 2400 bit/s |
| Receiver | It finds segment 4 by correlation with a local copy. This gives the start of the data, the carrier phase and known symbols for the equalizer. |
| Circuit 109 | ON at -43.4 dBm0. OFF at -47 dBm0, or 12 dB below the level of the signal. OFF in 5 ms to 14.5 ms. |
| Not implemented | The short sequence a), the backward channel, the turn-on without protection |

The OFF level of 12 dB below the signal is a D-Modem rule. It finds the
end of a signal when its echo or the noise of the line is still above
-48 dBm. A signal that drops by 12 dB at once, but stays above -43 dBm,
turns the detector OFF. V.27 ter does not permit this. On a telephone
line, the level does not change this quickly while a signal continues.

### Corrections (2026-11)

| Error | Correction |
|---|---|
| The segment lengths were counts of symbols for 1600 baud. At 2400 bit/s (1200 baud), segment 1 was about 267 ms (185 ms to 200 ms required) and segment 2 about 27 ms (20 ms to 25 ms). | The lengths are times, at the rate in use. |
| The pulse shaping adds about 6 ms to 8 ms to the end of each signal. Thus, segment 1 was about 206 ms on the line at 4800 bit/s, and segment 2 only about 15 ms. | The nominal times include the pulse shaping. The times on the line agree with Table 3. |
| In the turn-off, segment A was about 14 ms to 16 ms (5 ms to 10 ms required), and segment B about 13 ms to 15 ms (20 ms). | Segment A has 2 ms of ONE bits after the data. Segment B has 27 ms of silence, about 20 ms on the line. |

The test `V27ter.SegmentTimes` (`dsp/test/fax_test.cpp`) measures the
times on the line at the two rates.
