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
| Sync | ≥ 10002 | mapped frames of ones (levels A), until the client's MP |
| Record | whole records | the server's record, until the client's E |
| Start | 4080 | mapped ones in the data levels |
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

The sixth sign does not carry data. The I-modem's choice depends on the
last few values and the rest of the frame, but its exact rule is not
known; D-Modem sets it against the running sum. The client appears to
rely on the exact rule (most likely to train on the known frames of
ones): with the I-modem's own octets from the sync segment on, the
Courier starts error correction, and with D-Modem's it hangs up about a
second into data.

Upstream is V.34 data at the client's symbol rate with GPA, expanded
shaping, the 64 state trellis code and non-linear encoding (9.7/V.34,
theta 0.3125), at the highest rate of the client's mask up to 12
(28800); the Courier sent at 24000.
