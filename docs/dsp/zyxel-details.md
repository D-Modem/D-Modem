# ZyXEL 16800 and 19200: technical details

Nobody published the specification of the ZyXEL modes. This page records
what a static trace of the ZyXEL U-1496S firmware gave. For an overview,
refer to [ZyXEL 16800 and 19200](zyxel.md). The trace itself (addresses,
tools and the tables as JSON) is in the project files, in `zyxel/`.

**Status: D-Modem to D-Modem only.** D-Modem has the ZyX modes
(`AT+MS=ZYX`, `dsp/dp_v32.cpp`, `dsp/zyx.h`). For the parts that are not
known, it uses best guesses (refer to
[The version in D-Modem](#the-version-in-d-modem)).

## Where it comes from

The sources are the EPROMs of two U-1496 modems:

| Modem | Firmware | ZyX 16800/19200 |
|---|---|---|
| U-1496S (Plus) | V 5.06B (beta, 1993) | yes |
| U-1496E | V 6.15 M | no (the help text says "only available with Plus models") |

Each modem has two 128 KB EPROMs for a Motorola 68000. The board
connects the data lines of the EPROMs out of order. Thus, the
interleaved image did not look like 68000 code. The low byte (U24) has
its bits in reverse order. The high byte (U25) has the order
`{0:3, 1:2, 2:4, 3:1, 4:5, 5:0, 6:6, 7:7}` (CPU bit to stored bit). After
this decode, the image is clean 68000 code at `0x80000`.

The 68000 does the symbol work: the mapping of the bits, the trellis
encoder, the Viterbi decoder and the decisions. Two DSPs ("DSPA" and
"DSPB" in the self test) do the filters. The 68000 sends commands and
parameter tables to the DSPs. The DSP programs are in the ROM of the DSPs,
not in the EPROMs. Thus, the trace gives all of the symbol level, but
not all of the signal level.

We checked the method on the V.32bis part of the same firmware. Its
V.32bis constellations are the constellations of D-Modem
(`dsp/qam_tables.cpp`), point for point. Its trellis table gives the same
output as the `TrellisEnc` of D-Modem for 2000 random inputs.

## The rate signals

The R and E words are the words of V.32bis (B0 is the first bit). The
firmware uses the bits of V.32terbo:

| Bit | Rate |
|---|---|
| B12 | 14400 |
| B13 | 16800 |
| B14 | 19200 |

- The R of the U-1496S is `0FF9` (all the rates of V.32bis). When the
  ZyXEL modes are enabled, the modem adds B13 and B14 (`0FFF`).
- The E for 19200 is `F993`. The E for 16800 is `F995`. These are the E
  words of V.32terbo (refer to [V.32terbo](v32terbo-details.md)).
- The modem uses B13 and B14 only after it found a ZyXEL peer. If not,
  it ignores them, as a V.32bis modem does.

The same R and E words thus can mean two different signals: V.32terbo at
2400 baud, or ZyX at 2742.857 baud. The detection of the ZyXEL peer
occurs before the rate signals, so the two modems already agree on the
symbol rate.

## The symbol rate

When the modem finds a ZyXEL peer, it sets its ZyXEL flag. Then it
multiplies its measured round trip delay by `0x4924 / 0x4000` (8/7), and
it loads new parameters into the DSP. The delay is in symbols. Thus, the
new symbol rate is 8/7 of 2400:

    2400 x 8 / 7 = 2742.857 baud

The bit counts agree with this rate:

| Mode | Data bits | Bit rate |
|---|---|---|
| ZyX 19200 | 7 in each symbol | 7 x 2742.857 = 19200 |
| ZyX 16800 to 7200 | 7 groups of N bits in 8 symbols (N = 7 to 3) | 7N/8 x 2742.857 = N x 2400 |

We think that the start-up (S, TRN and the rate signals) also uses the
new rate, because the firmware scales the delay before these steps. We
did not read the training code.

## ZyX 19200

ZyX 19200 is V.32bis with one more uncoded bit:

- 7 data bits in each symbol. Q1 Q2 go through the differential encoder
  and the 8 state convolutional encoder of V.32bis. Q3 to Q7 are not
  coded.
- The label is Y0 (bit 0), Y1 Y2 (bits 1-2), then Q3 to Q7 (bits 3-7),
  as in V.32bis.
- The constellation has 256 points on the odd-sum grid of V.32bis. Points
  0 to 127 are exactly the V.32bis 14400 points. Points 128 to 255 are
  outer points. The largest coordinate is 12. The mean power is 81.4
  (units of `v32_points_14400`).

This constellation is not the constellation of V.32terbo. V.32terbo uses
a different grid (refer to [V.32terbo](v32terbo-details.md)).

## ZyX 16800 to ZyX 7200: the frame of 8 symbols

The other ZyX rates use a frame of 8 symbols. The modem reads 7 groups of
N bits from the scrambler for each frame (N = 7, 6, 5, 4, 3 for 16800,
14400, 12000, 9600, 7200). Put the 7 groups in sequence, first group
first, bit 0 of each group first. This gives a stream of 7N bits. The
modem divides the stream as follows:

1. The first K bits are the ring index (bit 0 first is the least
   significant bit).
2. Then each symbol, in sequence, takes M bits. Bits 0 and 1 are Q1 Q2
   (the trellis bits). The other bits are uncoded.

| Rate | N | K | M | Points | Check: K + 8M |
|---|---|---|---|---|---|
| ZyX 16800 | 7 | 9 | 5 | 256 | 49 = 7 x 7 |
| ZyX 14400 | 6 | 10 | 4 | 128 | 42 = 7 x 6 |
| ZyX 12000 | 5 | 11 | 3 | 64 | 35 = 7 x 5 |
| ZyX 9600 | 4 | 12 | 2 | 32 | 28 = 7 x 4 |
| ZyX 7200 | 3 | 5 | 2 | 32 | 21 = 7 x 3 |

The ring index selects a row of the ring table. A row has 8 ring
numbers (0 to 3), one for each symbol of the frame. The label of the
point of a symbol is:

    label = (uncoded << 5) | (ring << 3) | (Y0 | Y1 Y2 << 1)

Y0 Y1 Y2 come from the trellis encoder of V.32bis, which runs
continuously from symbol to symbol. The encoder of the firmware starts in
state 0. Bits 3 and 4 of the label select the ring. In the table of each
rate, each ring has a quarter of the points, and ring 0 has the points
of lowest energy:

| Rate | Mean energy of ring 0, 1, 2, 3 | Mean power, shaped | Mean power, all points equal |
|---|---|---|---|
| ZyX 16800 | 20.5, 61.5, 101.5, 142.0 | 38.9 | 81.4 |
| ZyX 14400 | 10.0, 31.0, 52.0, 71.0 | 21.3 | 41.0 |
| ZyX 12000 | 5.0, 15.0, 27.0, 35.0 | 11.6 | 20.5 |
| ZyX 9600 | 3.0, 7.0, 15.0, 15.0 | 6.5 | 10.0 |
| ZyX 7200 | (the 9600 points) | 3.8 | 10.0 |

### The ring table

The ring table has 4096 rows. The rows are in order of cost (the sum of
the 8 ring numbers): first the row of cost 0, then the 8 rows of cost 1,
the 36 rows of cost 2, and so on. A rate with K bits uses the first 2^K
rows. Thus, each rate uses the cheapest combinations, and inner rings
occur more frequently than outer rings:

| Rate | Ring 0 | Ring 1 | Ring 2 | Ring 3 |
|---|---|---|---|---|
| ZyX 16800 (K = 9) | 65.7 % | 25.0 % | 7.4 % | 1.8 % |
| ZyX 9600 (K = 12) | 51.4 % | 29.1 % | 13.9 % | 5.6 % |

This is the principle of the shell mapping of V.34 (9.3/V.34), on 8
symbols and 4 rings. The firmware keeps the table in ROM. We did not
find the rule for the order of the rows inside one cost. Thus, the
implementation must use the table from the ROM.

A second check: for each rate, the firmware gives the DSP a transmit
scale. The squares of these scales have the same ratios as the shaped
mean powers above (to 1 per cent for 12000, 9600 and 7200). Thus, the
values of K and the ring table are correct.

### The receiver

The receiver of the 68000 is a Viterbi decoder on the labels. We did not
trace the step after the decoder. The receiver must take the frame of 8
labels, get the ring numbers (bits 3 and 4), find the row of the ring
table, and make the 7 groups again.

## The detection of a ZyXEL peer

We found only a part of this step.

- Each modem has a flag that enables the ZyXEL modes (`&N0` or `&N34` to
  `&N39`).
- Early in the start-up, the modem loads a block of 253 words into DSPA.
  Then it reads two status bits from DSPA. When these bits stay set for 3
  reads, the modem sets its ZyXEL flag and changes the symbol rate.
- With the flag, the modem sends other parameters to DSPA (for example,
  command `0xA145`, not `0x9001`).

The DSP makes and detects the signal. Its code is in the ROM of the DSP.
Thus, we do not know the signal yet.

## Not known yet

1. The carrier frequency at 2742.857 baud. (For V.32bis, the firmware
   probably sends `0x3000` with command `0x9001`. This is 1800 Hz at 9600
   samples a second.)
2. The signal for the detection of a ZyXEL peer, and when it occurs.
3. The start-up at the new symbol rate (S, TRN, the rate signals).
4. The scrambler. The serial hardware gives the bits. We think that it is
   the scrambler of V.32bis.
5. The order of the bits in time inside a group. We think that bit 0 is
   the first bit, as Q1 is in V.32bis.

A recording of two real ZyXEL modems can settle items 1 to 5.

We also traced the image of the Elite 2864I 1.11 (1995). Its 68000 code
for ZyX is the same as the code of the U-1496S: the same ×8/7 change of
the delay, the same DSP commands and the same ring table. The image also
holds programs that the 68000 loads into DSPs. These programs are
filters for the ISDN side of the modem, not the modem DSPs. The modem
DSPs of the Elite run from mask ROM, as on the U-1496. Thus, no EPROM
image that we have gives items 1 to 3.

## The version in D-Modem

D-Modem has the ZyX modes as a mode of its V.32bis pump. The parts that
we know come from the firmware:

- The rate signals, the 6 speeds and their E words.
- The 5 constellations of the frame and the 256 point constellation of
  ZyX 19200 (`dsp/zyx_tables.cpp`, made from the ROM).
- The ring table (4096 rows, from the ROM).
- The frame of 8 symbols. A test (`ZyX.FrameMapper` in `pump_test`)
  compares the mapper of D-Modem with the frame split of the firmware,
  which runs in an emulator.
- The trellis code of V.32bis, which starts in state 0.
- The change of the round trip delay by 8/7.

For the parts that we do not know, D-Modem uses these guesses:

| Item | Guess in D-Modem |
|---|---|
| Peer detection | None. `AT+MS=ZYX` on the two ends selects ZyX. |
| Change to 2742.857 baud | After the round trip delay is measured: the answerer when it stops AC, the caller when it stops CC. |
| Carrier | 1800 Hz, as V.32bis. |
| Roll-off of the pulse | 0.2 (V.32bis uses 0.25). The band is 154 to 3446 Hz. |
| Scrambler | The scramblers of V.32bis (the caller GPC, the answerer GPA). The bits of a frame go through it in their sequence. |
| Order of the bits in time | Bit 0 of each group first. |
| B1 | 128 symbols (16 frames) of scrambled ones, through the frame mapper. |
| Transmit level | Each constellation is scaled to the mean power of the V.32bis constellations. For the speeds with a frame, this is the mean power with the ring frequencies of the ring table. |

When a recording of a real call is available, these guesses can change
without a change to the parts from the firmware.
