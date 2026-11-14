# V.FC: technical details

V.FC's specification was never published. This page records what
D-Modem's version (`dsp/vfc_data.cpp`, `dsp/dp_vfc.cpp`) is based on.
For an overview, see [V.FC](vfc.md).

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
   starts at state 0. The quadrant z does not start at 0: it continues
   from the last training point (0x3da).

The scrambler is V.32's. The caller sends with 1 + x^-18 + x^-23 and the
answerer with 1 + x^-5 + x^-23 (bit 0 of data word 0x6f selects the
transmitter's, bit 1 the receiver's). The Courier scrambles 8 bits at a
time (0x8cc2, descrambler 0x8cf3), least significant bit first. A model
of the two routines gives the same bits as the bit-by-bit scrambler of
V.34.

The Courier also has a precoder (0xc9c5) and a non-linear encoder
(0xc9f8) like V.34's, with one more term in the latter's series. In the
calls that we examined, the precoder's taps (data 0x856 to 0x85b) were
0 and the non-linear encoder was off (bit 10 of 0x879). D-Modem uses
neither.

## Check against the Courier's data

The emulated Courier answered a call from D-Modem at 28800 bit/s. The
bits that the Courier's scrambler sent were read from the emulator (at
0x8cc2). D-Modem's encoder then encoded the same bits, with z set to
the Courier's start value:

- All 324 symbols of the record agree with the points that D-Modem
  received, within the receiver's noise.
- The frame pattern agrees: the first frame short, then a long frame
  and four short ones (data word 0x3a3, loaded from 0x3a2).
- With z started at 0, a third of the points are different. z must
  continue from the training.

## The start-up

The start-up is in four phases. The Courier's scripts are tables of
steps in the overlay. D-Modem's pump (`dsp/dp_vfc.cpp`) sends the same
steps.

### Phase 1: the tones

1. The answerer sends ANS (2100 Hz) in segments of 1.4 s (0xa633, then
   0xa635).
2. The caller sends 800 Hz for 300 ms. The tone must start about 1.43 s
   after the start of ANS.
3. The answerer stops ANS when it hears the tone.

The answerer measures when the tone starts. A counter at 7200 Hz
(0x379) starts at about -2310 at the start of each ANS segment. The
answerer keeps D32b = max(0, counter - 0x8c). D32b sets two lengths in
phase 3: the first S signal and the zeros at the end. If the tone is
late, these signals are very long and the call fails. D-Modem's caller
therefore starts the tone 1090 ms after the call progress pump finds
ANS (that pump finds it about 340 ms after it starts), or 1330 ms after
its own detection (100 ms).

### Phase 2: 800 baud

Both modems send at the same time at 800 baud, differential QPSK. The
caller's carrier is 1025 Hz (1825 Hz and 1975 Hz in the first steps),
the answerer's 2400 Hz (1950 Hz for its probe). The steps are:

| Step | Caller (0xa64b) | Answerer (0xa677) |
|---|---|---|
| 1 | Silence, 8 symbols | Silence, 72 symbols |
| 2 | Reversals, 80 | Reversals, 80 |
| 3 | Probe, until the second reversals | Probe, 176 |
| 4 | Silence, 10 | Silence, 10 |
| 5 | Reversals, 80 | Reversals, 80 |
| 6 | Ones, 80 | Ones, 80, then ones until it hears a message |
| 7 | Messages, until the other's type 0 | Messages |
| 8 | Message type 7, value 0x7f (done) | Message type 7, value 0x7f (done) |

A message has 20 dibits (40 bits): the sync 0xd849 (8 dibits), the type
(4 bits), the value (12 bits, with a fixed dibit 2, 3 in it), and a 4 bit
check, ~((v >> 4) + (v >> 8) + v + t) & 0xf.

| Type | Value | Meaning |
|---|---|---|
| 0 | 0x100 + the symbol rate bits | The symbol rates that the sender has (bit i: V.34's rate i) |
| 1 | 0x083 | Sent by the answerer only |
| 2 | 0x170 | |
| 3 | 0x000 | |
| 5 | 0x255 | |
| 7 | 0x07f | Done: the sender has all the messages of the other |

The modems use the fastest symbol rate in both type 0 messages. The
meaning of types 1, 2, 3 and 5 is not known. D-Modem sends the values
that the Courier sends.

The transmit level is important. At an rms level of 2000 the Courier
takes about 1 s to hear D-Modem's messages. At the Courier's own level
(about 4900) it hears them at once.

### Phase 3: the trainings

At the new symbol rate, the answerer trains the caller's receiver, then
the caller trains the answerer's receiver. The script (0xc576) is:

| Entry | Routine | Signal | Symbols |
|---|---|---|---|
| 0xc576 | 0xc5ea | Silence (answerer only) | 16 |
| 0xc579 | 0xc601 | S (points 0 and 2 in turn) | (D33a + 0x41) & ~1, about 250 |
| 0xc57c | 0xc60a | S | 256 |
| 0xc57f | 0xc61a | S-bar (points 3 and 1 in turn) | 16 |
| 0xc582 | 0xc62b | 2-phase training, scrambler from 0 | 256 |
| 0xc585 | 0xc642 | 4-phase training | 1024 |
| 0xc588 | 0xc660 | Ones, differential | 6912 |
| 0xc58b | 0xc64b | Zeros, differential | about 344 |

D33a comes from D32b of phase 1. The caller starts its own training
when it has received about 172 symbols of the answerer's zeros. The
answerer goes to phase 4 after its script ends and it has received at
least 202 symbols of the caller's zeros.

### Phase 4: the table and the rates

Both modems send at the same time (0xc58f):

1. S (256), S-bar (16), 2-phase (256) and 4-phase (1024) training, and
   ones.
2. When its receiver is trained, the table (0xc5a2): six entries, each
   the sync word 0xd9ab (8 symbols), a byte pair (0x0a0a, 0x0909,
   0x0808, 0x0202, 0x0101, 0x0000; 4 symbols) and a zero word (8
   symbols).
3. Ones (4), the word 0xaaaa (4 symbols).
4. The rate mask (0xc682), until it has the other's mask: 0x8885 and a
   bit for each data rate that the sender allows (0x0040 14400, 0x0100
   16800, 0x0200 19200, 0x0400 21600, 0x1000 24000, 0x2000 26400, 0x4000
   28800).
5. The final word (0xc678, 32 symbols): 0x000a and the bit of the rate
   chosen, the fastest in both masks.
6. The data (0xc5e6, then 0xc6bf).

A word goes 2 bits a symbol, scrambled and differential. Symbol i of a
step of C symbols carries the bits (w >> ((-2(C - i)) & 15)) & 3.

The Courier sends its table as soon as its receiver is trained, but it
reads the other table only after it sends its own. D-Modem therefore
sends its table 20 ms after it receives the Courier's table. If no table comes, D-Modem
sends its table 1000 ms (caller) or 500 ms (answerer) after its own
receiver is trained.

### The data

The data starts 24 symbols after the first symbol of the final word.
The Courier first sends 56 scrambled ones (0xc98e, 7 times), then the
data from its controller. The trellis state starts at 0. z continues
from the training.

## D-Modem's receiver

D-Modem's receiver for phases 3 and 4 and the data:

- 3422.2 baud at 3429 (30800 / 9, the Courier's codec rate / 3), carrier
  4/7 of the symbol rate.
- Roll-off 0.12, filter span 16 symbols, equalizer 32 taps.
- No equalizer adaptation on S: the phase only.
- A new receiver for phase 4.
- The training is found when the last 24 symbols match the known
  2-phase training to 0.85. (At 0.6 a partial match one symbol early was
  taken, and the training then failed.)

After training, the receiver's mean square error is about 0.001 of the
signal power (about 30 dB). In the data it is about 0.0025. That is too
much for 28800 bit/s, so D-Modem offers 26400 at most. At 26400 and below
two D-Modems pass data without errors.

## The emulator

These fixes to D-Modem's C5x emulator (`dsp/test/hw/tms320c5x.hpp`) were
necessary for the Courier's V.FC:

- XPL, OPL and APL set TC when the result is 0.
- Program 0x2000 to 0x23ff is the C51's single-access RAM, which is also
  data 0x0800 to 0x0bff. The V.FC receiver keeps its filter taps there.
- There is no interrupt between RPT and the instruction that it repeats.

Two emulated Couriers still do not finish V.FC: they stop at the table.

## Not done

- 28800 bit/s (the receiver's noise floor).
- Data from D-Modem to the Courier: the Courier decodes the first frames
  (about 260 bits at 24000 bit/s) and then loses the data.
- Calls from the Courier: it does not reply to D-Modem's answer tone.
- The meaning of the phase 2 message types 1, 2, 3 and 5, and of the
  table.
- The symbol rates other than 3429: their carriers are from the
  Courier's table, but no call has used them.
- The rates below 14400, precoding and the non-linear encoder.
- Rate renegotiation.
