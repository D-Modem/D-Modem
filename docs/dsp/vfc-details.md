# V.FC: technical details

Nobody published the specification of V.FC. This page records the sources of the
version of D-Modem (`dsp/vfc_data.cpp`). For an overview, refer to
[V.FC](vfc.md).

## Where it comes from

The DSP of the Courier V.Everything (a TMS320C5x) runs V.FC from overlay 7 of
its firmware image IDSDL302 (flash offset 0x3c2f0, loaded at program
0xb000). The [courier-emu](https://github.com/Kitsok/courier-emu)
project (`docs/vfc.md`) found the overlay, the symbol rate table, the
point table and the two trellis codes. Most of the files of courier-emu have no licence.
Thus, D-Modem uses its findings as facts, and copies none
of its code.

For this work, we ran the overlay in a small C5x simulator of D-Modem.
Known bits replaced the output of the scrambler. We wrote a model of
the mapper and changed it until its points were the same as the points of the firmware:
80 symbols at each of the 35 rates below with the 32 state code, and
some with the 8 state code. `dsp/test/vfc_test.cpp` compares the
encoder of D-Modem with 48 of the points of the Courier at five of these rates. All
addresses below are program addresses in that overlay or in the resident
code below it.

## Rates

The rate index r (0 to 6) gives (r + 6) x 2400 bit/s, 14400 to 28800
(0xb77c reads it from the rate word). The symbol rates are the six symbol rates of V.34.
Above the speeds in the table in [V.FC](vfc.md), the points of a frame would
go outside the point table. Thus, we think that these speeds are the limits.

## Frames

The data goes in frames of 8 symbols. A frame carries b = ceil(8R/S)
bits for rate R at symbol rate S. Where 8R/S is not a whole number, some
frames carry one bit less. In each cycle of 5 frames (7 at 2800 baud),
the long frames come first, then the short frames. The first frame
of the call is a short frame (0xc88b, the pattern at 0x3a2).

From b, with q = floor((b - 32) / 8) and k = b - 32 - 8q:

| Per symbol | Bits |
|---|---|
| quadrant change | 2 |
| uncoded | q |
| for the shell mapper | 2 or 3 |

The 16 + k shell mapping bits of a frame select the rings of the 8 symbols with
the shell mapping of V.34 (9.3/V.34). There are M rings, where M is the smallest number for which
M^8 >= 2^(16 + k) (4 to 8). A 16-bit pattern for each k tells which symbols carry 3 bits and which
carry 2 (0xcb66: `aaaa aaab aaaf aabf aaff abff afff
bfff`, 2 bits for each symbol from the low end). A short frame takes one bit
from the last symbol.

The bits are taken in sequence, least significant first. For each symbol, there are
its 2 + q bits (the low 2 are the quadrant change), then its shell mapping
bits. These bits go into the number of the shell mapper at the next higher bits.

## Points and trellis codes

For each symbol:

1. The quadrant z = (z + the 2 bits) mod 4.
2. The point index is the uncoded bits plus ring << q.
3. The subset c = `0 5 6 1 2 7 4 3`[s0 + 2z] (0xcb2e), where s0 is the
   low bit of the state of the trellis encoder.
4. The point is entry 2 x index + (c >> 2) of a quarter-plane table
   (0xaecf in the resident code, `vfc_points` in D-Modem), turned
   clockwise by c & 3 quarter turns. The points of the table are on the
   lattice of x + y odd. This is the lattice of V.34, turned by 45 degrees.
   courier-emu found that its energy shells hold the same points as the
   energy shells of V.34, in a different sequence.
5. The next state of the encoder: each bit of state | z << 5 selects a tap
   word (seven taps). The taps are XORed into B, and the state is
   (B ^ ((B >> 5) & (B >> 7) & 3)) & 0x1f. One set of taps (0xcb5f) is
   the 8 state code of V.32. The other set (0xcb58) is a 32 state code. The encoder
   starts at state 0 and z at 0.

The scrambler is the scrambler of V.32 (1 + x^-18 + x^-23 or 1 + x^-5 + x^-23).
The Courier runs it on bytes. D-Modem runs the scrambler of V.34 bit by bit. We did not
compare it with the byte version of the Courier.

The Courier also has a precoder (0xc9c5) and a non-linear encoder
(0xc9f8), as V.34 has. The series of the non-linear encoder has one more term.
D-Modem uses neither of them.

## D-Modem's start-up

With `+MS=VFC`, the V.34 pump runs the start-up of V.34 and sets bit 34 of its
MP (reserved in V.34). When the two MPs have this bit, the two directions use the
data mode of V.FC. The rate is the fastest rate of V.FC at that symbol rate, up to the
rate that V.34 selected. The code is the 32 state code when the MP asks for 32 or 64 V.34
states, and the 8 state code for 16. If there is no such rate, the data mode of V.34
runs.

## The Courier's own start-up

We know these items from the overlay now (scripts of signals at 2400 baud,
run by 0xc529):

- The answerer (entry 0xb000, script 0xc560) sends 16 symbols of
  silence, 256 of alternating points (as the AC of V.32), 16 of the other
  pair (CA), 256 two-phase and 1024 four-phase training symbols, and 16
  more. Then it repeats a rate word until it hears the caller.
- The caller (entry 0xb028) stays silent until its receiver has selected
  one of five scripts.
- The training uses the 4800 bit/s points of V.32 and its scrambler.
- The DSP reports rate selections from the signal to noise ratio to the
  80186 supervisor (call 0x83c8). The supervisor is a part of the negotiation.

We did not read the line probing, the symbol rate change and the rate negotiation yet.

## Not done

- The start-up of V.FC itself. Thus, a call to a real V.FC modem is not possible.
- The rates below 14400.
- Precoding and the non-linear encoder.
- Rate renegotiation in the terms of V.FC (D-Modem uses the renegotiation of V.34, then the
  data mode of V.FC again).
