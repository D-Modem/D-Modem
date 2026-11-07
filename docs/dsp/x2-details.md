# x2: technical details

The facts here come from watching calls in courier-emu between the
original firmware of a USRobotics Courier (the client) and of an I-modem
(the server, Ie030002), and from the Courier calling D-Modem. No x2 code
was copied. The code is `dsp/x2.h`, `dsp/x2.cpp` (the server's
downstream) and `dsp/v34_x2.h` (its part of the V.34 pump).

## Phase 1: V.8

The server's JM is V.34's, except that the carrier's sign is turned
over after each complete JM. The client's CM is V.34's.

## Phase 2

- The server's INFO0a has bits 23 and 24 set (as power reduction 1 with
  a symbol rate difference of 6, and CME 1).
- After the server's L1 (sent at L2's level, 160 ms) and silence, the
  client sends no L1, L2 or INFO1c. It sends a 7 bit marker in an INFO
  frame (35 bits to the end of its CRC). Bit 0 is the carrier (1: high),
  bits 4 to 6 the client's symbol rate (as V.34's). The Courier sent
  `4d`: 3200 baud, high carrier.
- With no marker within 800 ms the server goes on as for V.34.

## Phase 3

The client sends V.34's S, S-bar, PP, TRN and J at its symbol rate, on
its carrier, with the answerer's scrambler (GPA). The server's PCM is
silent until the client's J; then its training, in G.711 octets:

| Segment | Length | What |
| --- | --- | --- |
| Preamble | 363 | 22 octets 16 times, then their first 11 with the sign turned |
| TRN | 20004 | scrambled ones (GPC from 0) at ±0x31; positive for a scrambled 0 |
| J words | 1200 | the 12 bit word 09B0 99 times, then 099F, scrambled, at ±0x31 |
| Ones | 6000 | scrambled ones at ±0x41 |
| Alignment | 1152 | `9e 28 a1 21 a8 1e`, frame f turned f places left |
| Sync | ≥ 10002 | mapped frames of ones (levels A), until the client's MP, ending on a frame |
| Record | whole records | the server's record, until the client's E |
| Start | 4086 | one frame of ones in levels A, then ones in the data levels |
| Data | | mapped data frames |

TRN has a fixed length: the client starts its phase 4 on the J words.

## Phase 4

After a silence the client sends S, S-bar, TRN (the first part at 4
points, then 16), then its MP, then MP with the acknowledge bit, then E
(20 ones), then V.34 data.

Records (both directions): 17 ones, a zero, each 16 bit word least
significant bit first followed by a zero, then a CRC-16 of the words'
bits only (from FFFF, polynomial 8408 reflected, not inverted). The
client's MP has 4 words; the Courier sent `0344 03FE 0000 0500`:

- word 0, bits 2 to 5: the highest downstream rate index it takes (1);
- word 1, bits 0 to 11: the upstream rates it can send (bit n-1 for
  n × 2400 bit/s);
- word 3, high byte: the downstream signs a frame (5).

The server's record has 3 words, `F33C 7FFF 0000`, sent padded with
zeros to 96 bits (4 frames).

## Data

Downstream, each frame takes MD + B scrambled bits (GPC, 1 + x^-18 +
x^-23). The first MD are signs; the B others, as a number (least
significant bit first), pick each position's level in mixed radix, the
first position the lowest digit. The levels are sorted largest first.
The MD signs go to the positions of the highest digits (the smallest
levels; of equal ones the first), in their order, each sent as a change
from the last one sent that way (starting from positive). At 32000
bit/s B = 19, MD = 5 and every position has the same 9 levels:

- levels A (before the record): `a5 a7 ad af b7 bd c5 cf e5`;
- levels B (32000 bit/s): `a5 a8 ab ae b3 b9 bf cb df`.

The sixth sign does not carry data. In levels A (the sync segment,
the records and the first frame of the start) the I-modem's rule is
known: the sixth value is negative when the other five, as signed
linear values (14 bits), sum to more than 805, and positive otherwise.
This matches every frame the I-modem sent, and D-Modem uses it.

In the data levels the rule is not known, and the client does not
need it: with the I-modem's own octets in the emulator and only these
signs changed, the Courier sent exactly the same. It is not
frame-local. The I-modem sends it negative through the first frames of
the start segment, then sets it against a running sum that starts from
zero there (a leaky sum agrees with it about 87% of the time on three
calls). D-Modem sets it against the running sum.

### The rate change

The data starts at the start-up rate. At the data levels' frame 16882
(counting the start segment's from 0; 12.66 s) the I-modem sends, the
same in every call:

- 8 frames `a5 a5 a5 25 25 25` and one `25 25 25 a5 a5 a5`, as they
  are;
- its scrambler from zero (and the last sign positive), then its record
  in the data levels, 10 times without the acknowledge bit (`733C`)
  and 33 times with it;
- one more frame, then the next rate's levels. That frame and the next
  rate's first 681 carry ones, not data (both I-modem calls; the data
  starts at frame 681 of the next rate). The Courier takes nothing at
  the next rate without them: it sends flags for 2.5 s, renegotiates
  again, and hangs up. With them the call holds. With the Courier's
  settings of the closed-loop tests that is 64 levels in each position
  (the first position's list one larger at the top and without `ce`):
  B = 36, MD = 5, 54666 bit/s. With the Courier's defaults the I-modem
  went to 50 and 51 levels (B = 34, 52000 bit/s). How the server learns
  which is not known: nothing the client sends differs.

The Courier renegotiates its upstream on a timer of its own about then:
S in data mode, S-bar, a short TRN, its MP (word 0's bits 2 to 5 now
14 or 12, word 1's mask `1FFE`), on 4 points even when phase 4's TRN
went to 16. It waits for the marker and records before it acknowledges
and sends E; then its upstream goes up (28800 bit/s). D-Modem answers
its MP with the marker and records when they have not gone yet, and
otherwise with its record until E. In some calls the Courier's S comes
just after the marker and it then sends a long TRN without an MP;
that case is not handled yet.

Upstream is V.34 data at the client's symbol rate with GPA, expanded
shaping, the 64 state trellis code and non-linear encoding (9.7/V.34,
theta 0.3125), at the highest rate of the client's mask up to 12
(28800); the Courier sent at 24000.

V.42's detection starts right after E. In its first tenth of a second
D-Modem's receiver is still settling into the 320 point constellation
and may decode a stray HDLC flag; the detection takes two flags in a
row as V.42, so it waits for the Courier's ODPs and answers them.
