# x2: technical details

The data on this page come from two sources. The first source is calls
in courier-emu between the original firmware of a USRobotics Courier (the
client) and the original firmware of an I-modem (the server, Ie030002).
The second source is calls from the Courier to D-Modem. We did not copy
x2 code. The code is `dsp/x2.h`, `dsp/x2.cpp` (the downstream of the
server) and `dsp/v34_x2.h` (its part of the V.34 pump).

## Phase 1: V.8

The JM of the server is the JM of V.34, but the server turns over the
sign of the carrier after each complete JM. The CM of the client is the
CM of V.34.

## Phase 2

- The INFO0a of the server has bits 23 and 24 set (as power reduction 1
  with a symbol rate difference of 6, and CME 1).
- After the L1 of the server (sent at the level of L2, 160 ms) and
  silence, the client sends no L1, L2 or INFO1c. It sends a 7 bit marker
  in an INFO frame (35 bits to the end of its CRC). Bit 0 is the carrier
  (1: high). Bits 4 to 6 are the symbol rate of the client (as in V.34).
  The Courier sent `4d`: 3200 baud, high carrier.
- If no marker comes in 800 ms, the server continues as for V.34.

## Phase 3

The client sends the S, S-bar, PP, TRN and J of V.34 at its symbol rate,
on its carrier, with the scrambler of the answerer (GPA). The PCM of the
server is silent until the J of the client. Then the server sends its
training, in G.711 octets:

| Segment | Length | What |
| --- | --- | --- |
| Preamble | 363 | 22 octets 16 times, then their first 11 with the sign turned |
| TRN | 20004 | scrambled ones (GPC from 0) at ±0x31; positive for a scrambled 0 |
| J words | 1200 | the 12 bit word 09B0 99 times, then 099F, scrambled, at ±0x31 |
| Ones | 6000 | scrambled ones at ±0x41 |
| Alignment | 1152 | `9e 28 a1 21 a8 1e`, frame f turned f places left |
| Sync | ≥ 10002 | mapped frames of ones (levels A), until the MP of the client, ending on a frame |
| Record | whole records | the record of the server, until the E of the client |
| Start | 4086 | one frame of ones in levels A, then ones in the data levels |
| Data | | mapped data frames |

TRN has a fixed length, because the client starts its phase 4 on the J
words.

## Phase 4

After a silence, the client sends these items in sequence:

1. S, S-bar and TRN (the first part at 4 points, then 16).
2. Its MP.
3. MP with the acknowledge bit.
4. E (20 ones).
5. V.34 data.

Records (in the two directions) have this format: 17 ones, a zero, each
16 bit word least significant bit first followed by a zero, then a
CRC-16. The CRC-16 is calculated only on the bits of the words (from
FFFF, polynomial 8408 reflected, not inverted). The MP of the client has
4 words. The Courier sent `0344 03FE 0000 0500`:

- Word 0, bits 2 to 5: the highest downstream rate index that it accepts
  (1).
- Word 1, bits 0 to 11: the upstream rates that it can send (bit n-1 for
  n × 2400 bit/s).
- Word 3, high byte: the downstream signs in each frame (5).

The record of the server has 3 words, `F33C 7FFF 0000`. The server pads
it with zeros to 96 bits (4 frames).

## Data

Downstream, each frame takes MD + B scrambled bits (GPC, 1 + x^-18 +
x^-23). The first MD bits are signs. The other B bits, as a number (least
significant bit first), select the level of each position in mixed radix.
The first position is the lowest digit. The levels are sorted largest
first. The MD signs go to the positions of the smallest levels (for equal
levels, the first), in their order. Each sign is sent as a change from the
last sign that was sent in that mode (starting from positive).

Where the lists of the positions are different, the level is important,
not the digit. With this rule, the ones of the I-modem after a rate change
decode with no errors. With the digits, about 1.5% of their frames are
incorrect. At 32000 bit/s, B = 19, MD = 5, and each position has the same
9 levels:

- Levels A (before the record): `a5 a7 ad af b7 bd c5 cf e5`.
- Levels B (32000 bit/s): `a5 a8 ab ae b3 b9 bf cb df`.

The sixth sign does not send data. In levels A (the sync segment, the
records and the first frame of the start), we know the rule of the
I-modem. The sixth value is negative when the sum of the other five, as
signed linear values (14 bits), is more than 805. If not, it is positive.
This agrees with all frames that the I-modem sent, and D-Modem uses this
rule.

In the data levels, we do not know the rule, and the client does not
need it. In the emulator, we used the octets of the I-modem and changed
only these signs. The Courier then sent exactly the same data. The rule
does not use only the frame. The I-modem sends the sign negative through
the first frames of the start segment. Then it sets the sign against a
running sum that starts from zero at that point. (A leaky sum agrees with
it about 87% of the time on three calls.) D-Modem sets the sign against
the running sum.

### The rate change

The data starts at the start-up rate. At frame 16882 of the data levels
(counted from 0 in the start segment; 12.66 s), the I-modem sends these
items. They are the same in all calls:

- 8 frames `a5 a5 a5 25 25 25` and one frame `25 25 25 a5 a5 a5`, with no
  changes.
- Its scrambler from zero (and the last sign positive). Then its record
  in levels B (the data levels of the start-up, at all rates: thus also
  after a second marker), 10 times without the acknowledge bit (`733C`)
  and 33 times with it.
- One more frame, then the levels of the next rate (refer to the
  subsequent text). That frame and the first 681 frames of the next rate
  send ones, not data (in the two I-modem calls, the data starts at frame
  681 of the next rate). Without them, the Courier accepts nothing at the
  next rate. It sends flags for 2.5 s, renegotiates again and hangs up.
  With them, the call continues.

The next rate is the rate that the renegotiation MP of the client
accepts (bits 2 to 5 of word 0, n). B = 22 + n and MD = 5. Thus, the rate
is (27 + n) × 4000/3 bit/s. The levels come from calls on lines 0 to
18 dB down. (A Courier on a worse line accepts a lower rate.)

| n | bit/s | levels per position |
| --- | --- | --- |
| 8 | 46666 | 32 in each: `a3`-`b0`, `b2`-`c0` by 2, `c3 c6 c9 cc cf d4 d9 de e8 f5` |
| 11 | 50666 | 47 in the first, 45 in the others (see below) |
| 12 | 52000 | 50 in the first, 51 in the others (see below) |
| 13 | 53333 | 52 in the first, 62 in the next four, 45 in the last |
| 14 | 54666 | 64 in each (the first's list one larger at the top and without `ce`) |

For 11: the first position has `a0`-`bf`, `c1`-`cf` by 2,
`d2 d6 da de e5 ec f8`. The other positions have the same, from `a2`.
For 12:

- First position: `9e`-`bf`, `c1 c3 c5 c7 c9 cb cd d1 d4 d7 da dd e2 e8 ee f9`.
- Other positions: `9f`-`bf`, `c1 c3 c5 c7 c9 cb cd cf d1 d4 d7 da dd e0 e5 ea
  f0 fa`.

For 13: the first position has `9e`-`bf`, `c1`-`cf` by 2,
`d2 d5 d8 db de e2 e7 ec f1 fa`. The next four have `a0`-`cf`, `d1`-`df`
by 2, `e2 e6 ea ee f4 fb`. The last has `a0`-`cc`. With these lists and
the level rule above, the ones of the I-modem after each change decode
with no errors.

Repeated and crossed I-modem calls show that the settings of the two
sides do not cause it. With all defaults, the first renegotiation of the
Courier accepted 12. Its next renegotiation, about 20 s later, accepted
14. The I-modem changed rate at each renegotiation (the second marker at
the largest level of 52000, `9e`/`9f`). The client expects the levels of
the rate that it accepted and no other levels. A Courier that accepted 14
and got the levels of 52000 renegotiated in 3 s (and accepted 9). Then it
hung up.

D-Modem changes to the rate that an MP accepts when it knows that rate
(8, 11 to 14). The I-modem does the same at each renegotiation, to a
higher rate or to the same rate (8 three times on an 18 dB line). For all
other rates, D-Modem answers with its record and no marker. The Courier
waits for a marker, and then hangs up.

At about that time, the Courier renegotiates its upstream on its own
timer. It sends S in data mode, S-bar, a short TRN and its MP (bits 2 to
5 of word 0 now 14 or 12, the mask of word 1 `1FFE`). It sends these on 4
points, also when the TRN of phase 4 went to 16. It waits for the marker
and records before it acknowledges and sends E. Then its upstream
increases (28800 bit/s). If D-Modem did not send the marker and records
yet, it answers the MP with them. If not, it answers with its record
until E.

The MP frames of the Courier come two zeros apart. Each MP frame is a
sync of 17 ones and a zero, then the frame. E is 20 ones in place of the
next sync. Thus, D-Modem accepts 20 ones after a frame as E. The error
bursts of the upstream can occur on E. On lines 8.5 to 10 dB down, with
the frame interrupt of courier-emu that follows the timer of the DSP, a
burst hit the frame before E and E itself in all three calls that we
tried. Thus, D-Modem did not find E. It waited for the end of the
renegotiation, and dropped the call 5 s later.

Now, after an MP', D-Modem accepts the 20 bits after a frame as E if 17 or
more are ones and the zero of the sync is not among them. (In those
calls, 2 bits were in error.) It also does this after a frame with a bad
CRC. With this change, the calls at 8.5, 9.5 and 10 dB continue to the
end of the run, and 94 to 98 lines arrive.

In each closed-loop call, the S of the Courier came about 50 ms before
the marker time of the I-modem. If a marker comes before its S, the
Courier renegotiates about 100 ms later all the same, but on 16 points and
without an MP. After 2.6 s of TRN, it hangs up. A second marker in that
TRN does not help. Thus, D-Modem does not send its marker at the time of
the I-modem. It waits for the MP of the Courier and answers it. It sends
the marker 3 s later (frame 20882) only if no renegotiation came before
then.

Upstream is V.34 data at the symbol rate of the client with GPA,
expanded shaping, the 64 state trellis code and non-linear encoding
(9.7/V.34, theta 0.3125). The rate is the highest rate of the mask of the
client up to 12 (28800). The Courier sent at 24000.

The upstream of the emulated Courier has bursts of errors, about 22 each
second at 24000 and also at 28800. Each burst is 20 to 40 symbols of
broadband error near the decision distance in the received signal. The
gain, phase and offset stay stable. The equalizer, echo canceller,
trellis code and theta have no effect on them. They come with the frames
of lower energy of the Courier. Frames with the smaller ring sums have
most of them, and the energy of the signal over them is 10 to 15% below
the mean. The transmitter of D-Modem, through the same receiver at the
same mean square error, decodes with no errors. The emulated I-modem also
saw them. In the run that we recorded with it, it rejected the I-frames
of the Courier two times in its first 14. V.42 recovers by
retransmission. About one RR in ten is lost.

The detection of V.42 starts immediately after E. In the first tenth of a
second, the receiver of D-Modem is still settling into the 320 point
constellation. It can decode an unwanted HDLC flag. The detection accepts
two flags in a row as V.42. Thus, it waits for the ODPs of the Courier and
answers them.
