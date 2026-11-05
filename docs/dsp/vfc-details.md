# V.FC: technical details

V.FC's specification was never published. This page records what
D-Modem's version (`dsp/vfc_data.cpp`) is based on. For an overview, see
[V.FC](vfc.md).

## Where it comes from

The Courier V.Everything's DSP (a TMS320C5x) runs V.FC from overlay 7 of
its firmware image IDSDL302 (flash offset 0x3c2f0, loaded at program
0xb000). The [courier-emu](https://github.com/Kitsok/courier-emu)
project (`docs/vfc.md`) found the overlay, the symbol rate table, the
point table and the two trellis codes. courier-emu has no licence for
most of its files, so D-Modem takes its findings as facts and copies none
of its code.

For this work the overlay was run in a small C5x simulator of D-Modem's
own, with known bits in place of the scrambler's output, and a model of
the mapper was written until its points matched the firmware's: 80
symbols at each of the 35 rates below with the 32 state code, and
several with the 8 state one. `dsp/test/vfc_test.cpp` checks D-Modem's
encoder against 48 of the Courier's points at five of them. All
addresses below are program addresses in that overlay or in the resident
code under it.

## Rates

The rate index r (0 to 6) gives (r + 6) x 2400 bit/s, 14400 to 28800
(0xb77c reads it from the rate word). The symbol rates are V.34's six.
Above the speeds in the table in [V.FC](vfc.md) a frame's points would
leave the point table, so those are taken to be the limits.

## Frames

The data goes in frames of 8 symbols. A frame carries b = ceil(8R/S)
bits for rate R at symbol rate S. Where 8R/S is not a whole number, some
frames carry one bit less: in each cycle of 5 frames (7 at 2800 baud)
the long ones come first, then the short ones, and the very first frame
of the call is a short one (0xc88b, the pattern at 0x3a2).

From b, with q = floor((b - 32) / 8) and k = b - 32 - 8q:

| Per symbol | Bits |
|---|---|
| quadrant change | 2 |
| uncoded | q |
| for the shell mapper | 2 or 3 |

The 16 + k shell mapping bits of a frame pick the 8 symbols' rings with
V.34's shell mapping (9.3/V.34), M rings with M the smallest for which
M^8 >= 2^(16 + k) (4 to 8). Which symbols carry 3 bits and which 2 is a
16-bit pattern by k (0xcb66: `aaaa aaab aaaf aabf aaff abff afff
bfff`, 2 bits a symbol from the low end); a short frame takes one bit
from the last symbol.

The bits are taken in order, least significant first: for each symbol
its 2 + q bits (the low 2 the quadrant change), then its shell mapping
bits, which go into the shell mapper's number at the next higher bits.

## Points and trellis codes

For each symbol:

1. The quadrant z = (z + the 2 bits) mod 4.
2. The point index is the uncoded bits plus ring << q.
3. The subset c = `0 5 6 1 2 7 4 3`[s0 + 2z] (0xcb2e), where s0 is the
   trellis encoder state's low bit.
4. The point is entry 2 x index + (c >> 2) of a quarter-plane table
   (0xaecf in the resident code, `vfc_points` in D-Modem), turned
   clockwise by c & 3 quarter turns. The table's points lie on the
   lattice of x + y odd, which is V.34's lattice turned by 45 degrees;
   courier-emu found that its energy shells hold the same points as
   V.34's, in another order.
5. The encoder's next state: the bits of state | z << 5 each pick a tap
   word (seven taps), the taps are XORed into B, and the state is
   (B ^ ((B >> 5) & (B >> 7) & 3)) & 0x1f. One set of taps (0xcb5f) is
   V.32's 8 state code, the other (0xcb58) a 32 state one. The encoder
   starts at state 0 and z at 0.

The scrambler is V.32's (1 + x^-18 + x^-23 or 1 + x^-5 + x^-23), which
the Courier runs on bytes; D-Modem runs V.34's bit by bit, which has not
been checked against the Courier's byte version.

The Courier also has a precoder (0xc9c5) and a non-linear encoder
(0xc9f8) like V.34's, with one more term in the latter's series;
D-Modem uses neither.

## D-Modem's start-up

With `+MS=VFC` the V.34 pump runs V.34's start-up and sets bit 34 of its
MP (reserved in V.34). When both MPs have it, both directions use V.FC's
data mode at the fastest rate up to V.34's choice that V.FC has at that
symbol rate, with the 32 state code when the MP asks for 32 or 64 V.34
states and the 8 state code for 16. If there is no such rate, V.34's
data mode runs.

## The Courier's own start-up

What is known so far from the overlay (scripts of signals at 2400 baud,
run by 0xc529):

- The answerer (entry 0xb000, script 0xc560) sends 16 symbols of
  silence, 256 of alternating points (like V.32's AC), 16 of the other
  pair (CA), 256 two-phase and 1024 four-phase training symbols, 16
  more, then repeats a rate word until it hears the caller.
- The caller (entry 0xb028) stays silent until its receiver has chosen
  one of five scripts.
- The training uses V.32's 4800 bit/s points and its scrambler.
- The DSP reports rate choices from the signal to noise ratio to the
  80186 supervisor (call 0x83c8), which takes part in the negotiation.

The line probing, the symbol rate change and the rate negotiation have
not been read.

## Not done

- V.FC's own start-up, and so a call to a real V.FC modem.
- The rates below 14400.
- Precoding and the non-linear encoder.
- Rate renegotiation in V.FC's terms (D-Modem's is V.34's, then V.FC's
  data mode again).
