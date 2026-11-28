# V.17: technical details

This page gives the technical details for [V.17](v17.md). The clause
numbers are the clause numbers of V.17 (02/91). This page is our own
description, not a copy of the Recommendation.

## The signal (2)

| Item | Value |
|---|---|
| Carrier | 1800 Hz ± 1 Hz |
| Frequency offset | The receiver must operate with an offset of up to ± 7 Hz. |
| Modulation rate | 2400 symbols each second ± 0.01 % |
| Mode | Half duplex, synchronous |

| Rate | Bits each symbol | Points |
|---|---|---|
| 14400 bit/s | 6 | 128 |
| 12000 bit/s | 5 | 64 |
| 9600 bit/s | 4 | 32 |
| 7200 bit/s | 3 | 16 |

Q1 and Q2 go through the differential code of Table 1. The eight-state
convolutional encoder of Figure 1 makes the redundant bit Y0 from Y1 and
Y2. Y0, Y1, Y2 and the uncoded bits select the point (Figures 2 to 5).
The code and the points are the same as in [V.32 bis](v32bis-details.md).

## Interchange circuits (3)

| Circuit | Rule |
|---|---|
| 106 | After the training, it follows circuit 105 in 3.5 ms. |
| 109, threshold (3.7) | ON above -43 dBm. OFF below -48 dBm. The ON level is at least 2 dB above the OFF level. The levels apply to scrambled ONE bits. |
| 109, time (3.6) | OFF 30 ms to 50 ms after the level falls. After a dropout, ON 40 ms to 205 ms after the level comes back. The first ON is part of the training. |
| Clamping (3.8) | While circuit 105 is ON, circuit 104 is ONE and circuit 109 is OFF. Optionally, this continues for 150 ms ± 25 ms after circuit 105 goes OFF. |

## Scrambler (4)

The scrambler is self-synchronizing, with the polynomial 1 +
x<sup>-18</sup> + x<sup>-23</sup>.

## Training sequences (5.1, Table 3)

| Sequence | Segment 1 | Segment 2 | Segment 3 | Segment 4 | Total |
|---|---|---|---|---|---|
| Long train | 256 | 2976 | 64 | 48 | 3344 symbols (1393 ms) |
| Short train | 256 | 38 | None | 48 | 342 symbols (142 ms) |

- Segment 1: A and B in turn.
- Segment 2: the equalizer training signal. Scrambled ONE bits at 4800
  bit/s, without the differential code. Each dibit selects a point: 00
  is C, 01 is D, 11 is A, 10 is B. The first scrambler state gives the
  points CDCDCDCDCDCDBDBD (Table 4).
- Segment 3: the bridge signal, only in the long train. It is a 16-bit
  word, eight times. In the word, B7, B11 and B15 are 1, and the other
  bits are 0. The modem scrambles the word and sends it at 4800 bit/s
  with the differential code of Table 6. The differential code starts
  from the last symbol of segment 2. A receiver
  ignores the bits B4 to B6, B8 to B10 and B12 to B14.
- Segment 4: scrambled ONE bits at the rate of the data, for 48 symbols.
  The convolutional encoder starts at zero. The differential code starts
  from the first symbol of segment 3 (long train) or from the last
  symbol of segment 2 (short train). The scrambler continues from
  segment 3. Circuit 109 goes ON during segment 4. At the end, circuit
  106 goes ON and the data starts.

### Segment 3 differential code (Table 6)

| Dibit | Phase change |
|---|---|
| 00 | +90° |
| 01 | 0° |
| 10 | 180° |
| 11 | -90° |

## Turn-off sequence (5.2, Table 7)

After circuit 105 goes OFF and the last data is sent, the modem sends:

| Segment | Content | Length |
|---|---|---|
| A | Scrambled ONE bits | 32 symbols |
| B | No energy | 48 symbols |

The total is 80 symbols (33 ms). If circuit 105 goes ON during the
turn-off sequence, the modem completes the sequence first.

## TEP (5.3)

The talker echo protection signal is optional. It is an unmodulated
carrier for 185 ms to 200 ms, then silence for 20 ms to 25 ms, before
the training.

## V.17 in D-Modem

| Item | D-Modem (`dsp/fax_modems.cpp`) |
|---|---|
| Rates | 14400, 12000, 9600 and 7200 bit/s, with the long train and the short train |
| Trellis code and points | The tables of `dsp/qam_tables.cpp`, the same as V.32 bis. `SpandspV17` tests them against spandsp. |
| Segment 2 | 256 + 2976 symbols (long) or 256 + 38 (short). The scrambler starts at 0x2ECDD5. This gives the points of Table 4. |
| Segment 3 | The word 0x8880, B0 first, eight times |
| Segment 4, long train | 48 symbols. The convolutional encoder starts at zero. |
| Segment 4, short train | 240 symbols (100 ms). The trellis encoder starts with the differential state 3 and the convolutional state 7. |
| Before the training | 48 symbols of silence. No TEP. |
| Turn-off | 80 symbols of scrambled ONE bits (segment A), then 48 symbols of silence |
| Circuit 109 | ON at -43.4 dBm0. OFF at -46.9 dBm0, or 12 dB below the signal. OFF 34 ms to 41 ms after the signal stops. |
| Receiver after a short train | It keeps the equalizer of the last long train (`FaxRxMemory`). The timing loop keeps its training gain for the first 100 data symbols, because the timing of the long train is not correct for the new signal. |

The short train and the turn-off are longer than V.17 specifies, and
the short train starts the trellis encoder in a different state. The
receiver of the original library that D-Modem replaced needs these. It
starts the data at a 20 ms frame boundary, and at 14400 bit/s it needs
up to 200 symbols to converge. It can also lose the last data in its
last frame. A receiver of V.17 that conforms gets more ONE bits before
the data and after it. It does not get errors in the data.

### Corrections (2026-11)

| Error | Correction |
|---|---|
| Circuit 109 went OFF 5 ms to 14 ms after the signal stopped (30 ms to 50 ms required). | `V17_OFF_MS` adds 27 ms before OFF. Circuit 109 goes OFF 34 ms to 41 ms after the signal stops. |
| The transmitter put its symbols at 1/4 of a sample period. At this phase the receiver of spandsp had approximately 3 dB more error. At 14400 bit/s, after a short train, an MR page through mu-law lost 12 bits. | The symbols are at 1/6 of a sample period. The errors are smallest at this phase, at all rates. |
| After a short train with 48 symbols of segment 4 (as V.17 specifies), the timing loop was too slow at the start of the data. With 3 of 46 start times against spandsp, the first rows of the page were bad. | The timing loop keeps its training gain for the first 100 data symbols (`V17_ST_FAST_SYMS`). All 46 start times now give a correct page. |

The test `V17.Circuit109` (`dsp/test/fax_test.cpp`) measures the levels
and the OFF time. The tests `Calls/T30Spandsp.SendsTheDocument`
(`dsp/test/t30_spandsp.cpp`) send pages from spandsp, with the short
train of V.17. The test `SpandspV17Ours.ShortTrainPageIntoSpandsp`
(`dsp/test/spandsp_test.cpp`) sends our TCF and an MR page into the
receiver of spandsp at all four rates.
