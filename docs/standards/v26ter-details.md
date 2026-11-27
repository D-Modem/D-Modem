# V.26 ter: technical details

This page gives the technical details for [V.26 ter](v26ter.md). The
clause numbers are the clause numbers of V.26 ter (11/1988). This page is
our own description, not a copy of the Recommendation.

## The signal (2)

| Item | Value |
|---|---|
| Carrier | 1800 Hz ± 1 Hz. There is no pilot tone. |
| Modulation rate | 1200 baud ± 0.01 % |
| Spectrum | 100 % raised cosine, divided equally between the transmitter and the receiver. At 1200 Hz and 2400 Hz, the energy is 3 dB ± 2 dB less than the maximum. |
| Group delay of the transmitter | ± 100 µs from 1200 Hz to 2400 Hz |
| Frequency offset | The receiver must operate with an offset of up to ± 7 Hz. |
| Equalizer | A fixed equalizer in the receiver, or an adaptive equalizer that converges on data at 2400 bit/s without a training sequence. |
| Level | As specified in V.2. |

### Line code (2.5.2)

At 2400 bit/s, each dibit is a phase change from the previous symbol.
The first bit of the dibit is the first bit after the scrambler.

| Dibit | Phase change |
|---|---|
| 00 | 0° |
| 01 | 90° |
| 11 | 180° |
| 10 | 270° |

At 1200 bit/s, each bit is a phase change: 0° for ZERO and 180° for ONE.

### Synchronizing signals (2.7)

| Segment | Content |
|---|---|
| Segment 1 | 32 symbols of 180° phase changes |
| Segment 2 | 64 bits of scrambled ONE bits: 32 symbols at 2400 bit/s, 64 symbols at 1200 bit/s. Table 3 gives the phase changes for each scrambler. |

## Scrambler (5)

Each direction has its own self-synchronizing scrambler:

- **GPC** = 1 + x<sup>-18</sup> + x<sup>-23</sup>. The call modem sends
  with GPC.
- **GPA** = 1 + x<sup>-5</sup> + x<sup>-23</sup>. The answer modem sends
  with GPA.

Each modem receives with the scrambler of the other modem. Appendix I
gives the process and the initial contents of the registers for segment
2.

## Interchange circuits (3)

| Item | Value |
|---|---|
| Circuit 106 | OFF to ON and ON to OFF in 2 ms or less, from circuit 105 (constant carrier) |
| Circuit 109 ON | Above -43 dBm |
| Circuit 109 OFF | Below -48 dBm |
| Hysteresis | At least 2 dB |
| Special quality leased circuits (M.1020) | ON above -26 dBm, OFF below -31 dBm |
| Circuit 109 ON in the start-up | After the synchronization, before data goes to circuit 104 |
| Circuit 109 ON to OFF | 5 ms to 15 ms |
| Circuit 109 ON after a dropout | 40 ms to 50 ms after the level is above the threshold |

The modem gives the transmit clock (circuit 114) and the receive clock
(circuit 115). The DTE can give the transmit clock on circuit 113.

## Modes (4)

| Mode | Rate |
|---|---|
| i) | 2400 bit/s synchronous |
| ii) | 2400 bit/s start-stop, 8 to 11 bits a character (optional) |
| iii) | 1200 bit/s synchronous |
| iv) | 1200 bit/s start-stop, 8 to 11 bits a character (optional) |

For the start-stop modes, the modem changes the characters to a
synchronous stream as specified in V.14. The received characters can be
a little faster than the nominal rate:

| Rate | Basic range | Extended range |
|---|---|---|
| 2400 bit/s | 2400 bit/s to 2424 bit/s | 2400 bit/s to 2455 bit/s |
| 1200 bit/s | 1200 bit/s to 1212 bit/s | 1200 bit/s to 1227 bit/s |

## Rate patterns (6.1.3)

A rate pattern is one octet, sent 32 times (256 bits), scrambled, at
1200 bit/s. The modem sends the least significant bit first. A rotation
of an octet has the same meaning as the octet.

| Octet | Rates |
|---|---|
| 01 | 1200 bit/s |
| 03 | 2400 bit/s |
| 07 | 1200 bit/s and 2400 bit/s |
| 05 | 4800 bit/s (provisional) |
| 09 | 2400 bit/s and 4800 bit/s (provisional) |

The Recommendation lists 34 octets that can be rate patterns. Table 7
gives a meaning to five of them.

## The start-up (6.3)

### Sequence A

The answer modem is silent for 2.15 s ± 0.35 s. It sends 2100 Hz
± 15 Hz for 3.3 s ± 0.7 s, then it is silent for 75 ms ± 20 ms (V.25).

### Sequence B (half duplex, 1200 bit/s)

1. The answer modem sends the synchronizing signals and 256 bits of its
   rate pattern. Then it is silent.
2. The call modem waits for 4 octets of the rate pattern in a row, with
   no errors. It selects the highest rate that the two modems can do.
3. The call modem is silent for 250 ms ± 5 ms. Then it sends the
   synchronizing signals and 256 bits of the rate pattern of the
   selected rate.
4. The answer modem waits for 4 octets of a rate pattern in a row. If no
   pattern comes in 2 s, it starts sequence B again. If the pattern
   gives a rate that it cannot do, it disconnects.
5. The answer modem is silent for 250 ms ± 5 ms. Then it sends 2100 Hz
   for 500 ms ± 50 ms to disable the echo suppressors (G.164). Then it
   is silent for 75 ms ± 20 ms.

### Sequence C (at the selected rate)

1. The answer modem sends its echo cancellation sequence until its echo
   canceller is good. Then it is silent for 25 ms ± 3 ms, and sends the
   synchronizing signals and scrambled ZERO bits.
2. The call modem waits for 64 scrambled ZERO bits in a row. Then it
   sends its echo cancellation sequence, is silent for 25 ms ± 3 ms, and
   sends the synchronizing signals and scrambled ZERO bits.
3. When the answer modem receives the signal of the call modem for
   50 ms ± 5 ms, it becomes silent.
4. After 64 ZERO bits from the call modem, the answer modem sends the
   synchronizing signals and scrambled ZERO bits again. The two modems
   now send at the same time.
5. After 64 more ZERO bits, the answer modem sends scrambled ONE bits.
6. After 64 ZERO bits, the call modem sets circuit 109 ON. It sends 128
   scrambled ONE bits, then data.
7. After 64 ONE bits, the answer modem sets circuit 109 ON, and data
   starts.

Rules for the echo cancellation sequence:

- It must not contain more than 32 ZERO bits in a row.
- With echo cancellers in the network (G.165), it must be at least
  650 ms long.
- The detection of ZERO or ONE bits starts only after the synchronizing
  signals.

If the DTE has a time limit between circuit 107 ON and circuit 106 ON,
this limit must be more than 15 s.

## Half duplex mode (7)

This mode is optional. Segment 1 of the synchronizing signals is 32
symbols, or 256 symbols with protection against talker echo. The start-up
has sequences A and B1 only.

| Circuit 106 OFF to ON | With protection | Without protection |
|---|---|---|
| 2400 bit/s | 240 ms ± 10 ms | 55 ms ± 2 ms |
| 1200 bit/s | 267 ms ± 10 ms | 82 ms ± 2 ms |

Circuit 106 ON to OFF is 2 ms or less. Circuit 109 ON to OFF is 5 ms to
15 ms. While circuit 105 is ON, the modem holds circuit 104 at ONE and
circuit 109 OFF. It can continue for 150 ms ± 25 ms after circuit 105
goes OFF.

## Tests (8)

The modem must have loops 2 and 3 of V.54, with the remote control of
loop 2 as specified in V.54.

## V.26 ter in D-Modem

| Item | D-Modem (`dsp/dp_v26.cpp`) |
|---|---|
| Rates | 2400 bit/s and 1200 bit/s. `+MS` limits the rate pattern (01, 03 or 07). |
| Segment 2 | From the registers of Appendix I |
| Rate pattern detection | 32 bits that repeat each 8 bits, then the rotation in Table 7 |
| Echo cancellation sequence | Scrambled ONE bits for 1000 ms |
| Start-up detector | Wide band power, 20 ms meter: ON at -43.4 dBm0, OFF at -47.9 dBm0. It stays ON in the 25 ms silences of sequence C. |
| Circuit 109 in data | `Circuit109`: the same levels. OFF in 6 ms to 11 ms. ON 40 ms to 43 ms after a dropout. No data goes to the DTE while it is OFF. |
| No common rate | The answer modem disconnects. The call modem also stops, but it does not send the pattern of its highest rate first. |
| Not implemented | The half duplex mode (7), the test loops (8), the levels for special quality leased circuits |

### Table 3 and Appendix I

D-Modem makes segment 2 with the scrambler and the registers of
Appendix I. The result agrees with Table 3 for GPC at the two rates, and
for GPA at 2400 bit/s. For GPA at 1200 bit/s, Table 3 gives 0°, 180° in
two positions near the end, where D-Modem sends 180°, 180°. The two rows
of GPA scramble the same ONE bits from the same register. Thus, the 1200
bit/s row must have the bits of the 2400 bit/s row, one bit a symbol.
D-Modem agrees with the 2400 bit/s row. We think that the 1200 bit/s row
of Table 3 has a typographical error.

### Corrections (2026-11)

| Error | Correction |
|---|---|
| In data, circuit 109 used the 20 ms meter of the start-up. It took up to about 220 ms to turn OFF. V.26 ter requires 5 ms to 15 ms (3.3.2). | In data, `Circuit109` turns OFF after 5 ms below the OFF level on a 0.5 ms meter: 6 ms to 11 ms in all. |
| After a dropout, circuit 109 came ON again at once. V.26 ter requires 40 ms to 50 ms (3.3.2). | `Circuit109` turns ON again 40 ms after the level is above the ON level, on a 5 ms meter. |
| While the carrier was lost in data, the receiver sent noise to the DTE. | No data goes to the DTE while circuit 109 is OFF. |

The test `V26.Circuit109` (`dsp/test/pump_test.cpp`) measures the levels
and the times on an 1800 Hz tone.
