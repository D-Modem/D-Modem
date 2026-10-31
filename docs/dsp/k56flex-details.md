# K56flex: technical details

This is the technical companion to [K56flex](k56flex.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

K56flex is Rockwell's and Lucent's 56k PCM protocol from before V.90. There is
no public specification. This page records what is known of it from the one
K56flex client we have, Motorola's SM56 soft modem (`sm56.ko`, its
"A56" code; see [interop](../interop-details.md#against-the-motorola-sm56-soft-modem)).
It is meant as the start of a K56flex **server** (the digital modem) built on
the V.90 server code.

Terms used on this page:

- **PCM** (pulse code modulation): the server sends G.711 codes, one per 8 kHz sample, straight into the network.
- **V.8bis**: the tone and V.21 message exchange that can come before V.8, used here to announce K56flex.
- **NS block**: a non-standard field in a V.8bis message, where a vendor puts its own capabilities.
- **T**: one symbol interval.

Status:

- D-Modem is a K56flex **server**: `AT+MS=56` (or `AT+MS=K56`) on an
  8000 Hz G.711 line answers with V.8bis and K56flex, and with V.90 when the
  caller has no K56flex (`DP_K56` on the V.34 pump, `dsp/v34_k56.h`). Calling,
  it runs V.90 (`V34Pump::start()`; `Modem::change_dp()` in `core/modem.cpp`
  maps `DP_K56` to V.90 only when no pump is registered for it).
- The interop test `sm56_k56flex` (label `sm56`) connects sm56 as the client
  at 44000 bit/s down and 31200 up, with V.42, and passes data [seen].
- The training is `K56Train` (`dsp/k56flex_train.cpp`), the data mode
  `K56Enc` and `K56Dec` (`dsp/k56flex.cpp`).
- Conexant's HSF engine (`hsfengine.o`) is no help. Its K56flex slot is
  disabled (`"NOTSUPP"` in its `+MS` table), and when forced on, its CM offers
  no modulation. Its `k56*` files are the PCM base of its V.90 receiver.

Each claim carries a mark:

- **[seen]**: read in sm56's code, or seen on the line in our runs against it;
- **[inferred]**: deduced, and not proven.

## Overview

K56flex is asymmetric, like V.90. The digital modem (server) sends PCM
codes downstream; the analogue modem (client) sends a V.34-style signal
upstream. The start-up:

| Phase | What happens | Roles |
|---|---|---|
| 1 | V.8bis (the K56flex capability, in a non-standard field), then V.8 selecting V.34 | the server is the V.8bis initiator and the V.8 answerer |
| 2 | V.34 phase 2, with K56flex in the INFO1 frequency offset field | the server is the V.34 *answer* modem (unlike V.90) |
| 3 | downstream PCM training; upstream S, S-bar, PP, TRN, J | the client waits for the server's signal |
| 4 | downstream R, TRN2, MP, MP', E; upstream J', TRN, MP, MP', E, B1 | |
| data | downstream PCM at 32000 to 56000 bit/s in 2000 bit/s steps; upstream V.34 | section 5 |

## 1. Phase 1: V.8bis, then V.8

The answering server starts V.8bis as the initiating station, before
its answer tone [seen]:

1. The server sends **MRe** (1375 + 2002 Hz for 400 ms, then 650 Hz for
   100 ms). **CRe** (the dual tone, then 400 Hz) works as well.
2. The client answers **CRd** (1529 + 2225 Hz for 400 ms, then 1900 Hz for
   100 ms), about 175 ms after MRe ends.
3. The server sends **CL** (capabilities list) on V.21 channel 1 (980 Hz mark, 1180 Hz space,
   300 bit/s). The framing is HDLC-like: about 30 ones, 3 flags, the octets LSB first with bit
   stuffing, an FCS-16 (as in HDLC), and 2 flags:

       12 c9 80 80 80 07 b5 02 00 94 81 83 61

4. The client sends **MS** (mode select) on V.21 channel 2 (1650 / 1850 Hz) about 10 ms
   after the CL. MS has the same layout, and carries our NS block back with 0x40
   cleared in its last octet:

       11 c9 80 80 80 07 b5 02 00 94 81 83 21

5. The server sends **ACK(1)**, the single octet `14`.
6. The server then runs plain V.8: ANSam from the server, CM from the client, JM from the server.

Timeline of one run (seconds into the recording): MRe 3.040 to 3.545, CRd
3.720 to 4.225, CL 4.345 to 4.985, MS 4.995 to 5.630, ACK 5.815 to 6.135 [seen].

The message octets [seen]:

1. Octet 0 is the message type, in the low nibble: 1 MS, 2 CL, 3 CLR, 4 ACK(1), 5 ACK(2), 8 and up NAKs.
2. Next come the identification field's NPar(1) octets, up to the one with bit 7 set. Bit 0x40 of the first one says that a non-standard field follows.
3. Then the identification field's SPar(1) octets, and the standard field's NPar(1) and SPar(1).
4. Then the NS blocks, each `[L][L octets]`.

The client takes a block as K56flex when all of these hold:

- `L > 6`
- `o[1] == 0xb5` (the T.35 country code for the USA)
- `o[2] == 0x02`
- `o[6] & 1`
- `(o[7] & 0x2f) >= 2`
- `o[7] & 0x40`

It does not check `o[3]` and `o[4]`. A block with bit 0 of `o[6]` clear
gets an MS without the NS block, and the call is V.34 [seen].

Our NS block was copied from the client's own CL template (`... 83 21`) with
0x40 added to the last octet. The 0x40 most likely marks the digital modem
[inferred]. When the client also has V.90 on, its MS sets bits 0 and 1 of
`o[6]` [seen in code].

**V.8.** The CM has no K56flex bit [seen]:

- With `AT+MS=K56FLEX,0` the client's CM is `e0 c1 45`.
- With K56FLEX's automode, or with V90, it is V.90's CM, `e0 c1 65 13 94 27 0d`.

The server's **JM must not select PCM**. If it selects V.90, the client runs V.90 and drops K56flex. A JM selecting V.34
keeps K56flex [seen].

Notes on the client [seen]:

- sm56 takes K56flex with `+MS=K56FLEX` (with or without automode) and with
  `+MS=V90`, but not with V92. With V92 it answers MRe with a quick connect message and
  CRd, and then sends no MS.
- A calling sm56 never starts V.8bis itself. It answers an initiating signal only if
  the signal comes after its blind-dial wait (S6, 2 s). Our MRe 0.3 s after ATD went
  unanswered; the same MRe 3 s after ATD was answered.
- Without V.8bis (with plain V.8) the client calls V.90 or V.34. It sets
  "K56flex agreed" only on a V.8bis exchange that ends in ACK(1).

## 2. Phase 2: V.34, with K56flex in INFO1

Phase 2 is V.34's, with the server as the V.34 **answer** modem (INFO0a, tone
A, INFO1a on the 2400 Hz carrier) and the client as the call modem (INFO0c,
tone B, INFO1c on 1200 Hz) [seen]. INFO0a, INFO0c and the line probing are
standard V.34 [seen]. K56flex uses the 10-bit **frequency offset** field of
the INFO1 frames:

- **INFO1c** bits 79:88 [seen]:
  - `0x001` when the client takes K56flex, its law is µ-law, and its tandem-codec check passed;
  - `0x200` (the normal V.34 value) otherwise;
  - bit 3 carries the client's law (1 = A-law), and bit 4 carries the x of an
    earlier INFO1a [inferred].
- **INFO1a** bits 40:49: the server answers with `1 | law << 2 | x << 3`. That is
  1 or 9 for µ-law, and 5 or 13 for A-law. Any other value, or a law that is not the client's, makes the
  client go on with V.34 [seen: for µ-law, 1 and 9 left V.34; 0, 2, 3, 5, 13,
  0x11 and 0x200 connected V.34]. The meaning of x is unknown. 1 is the
  value to send to a µ-law client.

The client's **tandem-codec check** looks for a second codec in the path. It reads the received power of the L1/L2
tones near 2100, 3300 and 3750 Hz [inferred from the offsets]. It passes when
P3300 ≥ 0.1·P2100 and P3750 ≥ about 0.018 to 0.020·P3300 [seen]. The server's L1/L2 must therefore be
normal V.34 probing. If the check fails, INFO1c says `0x200`.

## 3. Phase 3

After phase 2 a K56flex client does not run V.34's phase 3 [seen]. Instead it waits for
the server's downstream signal. Against our V.34 phase 3 it is silent for about 0.7 s and then retrains.

### Client upstream [seen]

The upstream is V.34-shaped, modulated at 3200 baud (the client's codec runs at 9600 Hz), and goes S, S-bar, PP, TRN and J:

| Signal | Length (T) | Notes |
|---|---|---|
| silence | | until the client has the server's Js |
| S | 128 | |
| S-bar | 16 | |
| PP | 288 | |
| TRN | echo delay + 4000 to 6430 | rate dependent |
| J | | 24 bits again and again: V.34's J (`0x8990` 4-point, `0x89b0` 16-point), then an octet from `SC_A56_Gen_JA_RBS` (the client's robbed-bit findings; 0 on a clean line), all 4-point mapped and differentially coded as V.34's |

The J tells the server how many levels to use in phase 4: `0x8990` means 1 bit
per symbol and `0x89b0` means 2 [seen]. sm56 sends `0x89b0`. Our receiver
(`V34Rx::j24`) takes two Js 24 bits apart as the J, and keeps the octet in
`j_rbs`.

The client's phase 4 upstream is V.34's caller's with J' for S and S-bar: J'
(`0x899f`) right after the last whole J, then TRN, MP, MP', E and B1 [seen].
They are 16-point (V.34's 10.1.3.8: TRN with 4 bits a symbol, MP and E too)
when the server's Js asked for 16 levels, as ours does; `V34Rx::trn16` decodes
them from J' on.

### Server downstream

The client's receiver (its "MOD" front end) goes through these states [seen]:

1. R search, sync search;
2. TRN1A (wait, channel estimate, load balance, reference directed, decision directed);
3. TRN1B (reference directed), then TRN1C (reference directed, transition, decision directed);
4. Js;
5. second R search, second sync search;
6. TRN2A, then TRN2B;
7. MP and E (decision directed), then B1;
8. data.

The durations in its table, in 8 kHz symbols (-1 means "until an event"), are:

    0 48 -1 -1 51 390 10 9000 6 7046 8448 4096 6 12273 2000 -1 -1 76 6
    16328 6 2040 14322 1420 62 13999 0

In the state list the index of a state's length is one more than its
name's place, so `8448` is TRN1B's and `4096` TRN1C's reference-directed part.
Its phase 3 detector (`SC_Phase3_Wait_TRN1B`) moves the front end on to
TRN1C's decision-directed part once 4000 symbols of its reference-directed
part have passed. `SC_Phase3_Det_Js` then decodes Js from the signs, as a
16-bit word of descrambled bits, MSB first: `0x991` means 4-level and `0xd91`
16-level. Only `0xd91` moves the client on: after `0x991` the detector
clears its own flag on the next comparison, so a 4-level Js is never taken
[seen]. These are V.34's J words bit-reversed: the server sends `0x89b0`, LSB
first [seen]. The client's front end then passes 12273 symbols (silence 2)
and looks for the second R.

sm56 also contains a digital-side transmitter (`isdn_tx`) that is never called
[seen]. It agrees with the client on R, TRN1A, TRN1C (its "TRN1B"), Js, TRN2A
and TRN2B. It has no TRN1B of the client's 3-bit kind, though, so it may be an
older version of the protocol [inferred]. Without that TRN1B the client takes
our Js 8448 symbols too early and never finds it [seen]. Its sequence, with
lengths in 8 kHz codes, is:

| Segment | Length | Codes |
|---|---|---|
| Silence 1 | 512 | code 0 |
| R | until an event | ±`0x54` in the pattern `+ + - -` (2 kHz); a reversal ends it |
| TRN1A | 16368 | 2-level: magnitude `0x4f` or `0x4e`, the sign from the scrambled bit |
| TRN1B | 8448 | `0x4f` with the sign of the scrambled bit (even symbols) or against the DC sum (odd symbols) |
| Js | 128 | as TRN1B, the scrambler fed with J's bits, LSB first |
| Silence 2 | 12288 | code 0 |
| R | until an event | as above |
| TRN2A | 16336 | 2-level, scrambled ones |
| MARK | 32 | scrambled zeros |
| TRN2B | 16368 | 4 magnitudes `0x2f 0x45 0x51 0x57` |
| MP | 704 | 8 MPs of 88 bits |
| MP' | 704 | MP with its acknowledge bit set |
| E | 12 | 24 scrambled ones |
| data | | |

The client's reference generators (what its decision-directed training
expects) [seen]:

| Segment | Codes |
|---|---|
| TRN1A | scrambled bit → `0x4f` or `0xcf` (its sign) |
| TRN1B | 3 scrambled bits (`MOD_RefGenTRN1B`) → magnitude `40 41 4b 4c 50 51 50 51` (b0 + 2·b1 + 4·b2), then the sign |
| TRN1C, TRN2A | 1 bit → `0x4f` or `0xcf`, corrected for robbed bits and digital loss |
| TRN2B | 3 bits → the 4 magnitudes `0x2f 0x45 0x51 0x57` and a sign |

The scrambler is V.34's (GPA, `1 + x^-5 + x^-23`) [seen]. It runs on through
TRN1A, TRN1B and TRN1C (the client's reference generators share one state),
and TRN1B scrambles 3 bits every symbol, the sign symbols' too. TRN1B's signs:
every other one (from its first symbol on) the bit 23 scrambled bits back, set
for positive; the others against the running sum [seen in
`MOD_RefGenTRN1B`].

The server's sequence that sm56 takes, as D-Modem sends it (`K56Train`):

| Segment | Length |
|---|---|
| Silence 1, R, TRN1A | 512, 288, 16368, as `isdn_tx` |
| TRN1B | 8448, the client's 3-bit kind |
| TRN1C | 8448, `isdn_tx`'s "TRN1B" |
| Js | 128, J = `0x89b0` |
| Silence 2 | until the client's J, 12288 at least, rounded up to 8 symbols |
| R, TRN2A, MARK, TRN2B | 288, 16336, 32, then TRN2B until the client's MP and 2048 symbols at least |
| MP, MP', E | 704, 704, 12 |
| data | B1 first (6 frames of ones) |

After its J the client waits only about 0.75 s for the second R, so the
server must hear that J (its 24-bit kind) and not wait for a timeout [seen].

Code conventions [seen]:

- In `isdn_tx`, a code with bit 7 clear is a positive magnitude index,
  `seg << 4 | mant`. A negative code is the one's complement of the positive
  one. It converts its code `c` to µ-law as `c & 0x80 ? c ^ 0x7f : c`,
  inverted, which makes silence `0xff`.
- The magnitude is `((2·mant + 33) << seg) - 33` in the 14-bit scale; `0x4f` is ±975.
- The client's decoder (`putBIOS`) and D-Modem (`K56Enc`, `K56Train`,
  `k56_g711()`) take bit 7 the other way, as G.711's: set is positive. The
  client's reference tables above are in that convention. Sent with bit 7
  negative, our data reached the client with every sign flipped [seen].

## 4. Phase 4

The client [seen]:

1. Sends J-bar (V.34's J', `0x899f`) once the front end is in TRN2A.
2. Waits for 16 descrambled ones followed by 16 zeros: TRN2A's end and MARK.
3. Takes the server's **MP**: V.34's type 0 MP (88 bits), sent as 2 bits per
   symbol with LSB first, descrambled, and checked by V.34's MP detector. `rate_ca`, `rate_ac`
   and the rate mask must all be non-zero, or it clears down.
4. Sets the upstream rate to min(MP's `rate_ca`, 12 or 13), and the
   downstream code r to min(`rate_ac`, its own). The downstream rate is
   30000 + 2000·r bit/s, so r = 1 is 32000 and r = 13 is 56000 [inferred].
5. Takes MP' (MP with ack = 1), then E (24 ones), waits 14 symbols, and
   takes B1 (48 symbols; the content is not checked). Then data mode starts.

The client's own MP is V.34's type 0, with [seen]:

- the acknowledge bit set;
- rate mask `0x7fff`;
- asymmetric rates;
- bits 52 to 59 = 1 and bits 60 to 67 = 0 (not V.34's zeros);
- a CRC-16 (as in V.34) at bits 69 to 84.

The server's MP and E symbols each carry 2 bits (d1, d0) as one of 4
magnitudes, or 1 bit as one of 2. The sign carries no data. On even symbols it
is history bit 24 of the scrambler; on odd symbols it is chosen against the
running DC sum [seen in `isdn_tx`]. The levels depend on the digital pad (the network's attenuation: 0, 3 or 6 dB) [seen]:

| Pad | 2 bits (`d1 d0` = 0..3) | 1 bit (0, 1) |
|---|---|---|
| 0 dB | `2f 45 51 57` | `55 3d` |
| 3 dB | `35 4d 58 60` | `5d 44` |
| 6 dB | `3d 54 61 67` | `65 4c` |

## 5. Data mode

The data mode encoder is `K56Enc` in `dsp/k56flex.cpp`, its decoder
`K56Dec`, and the constants are in `dsp/k56flex_tables.cpp`. They were read
from sm56's client decoder (`pstn_init_rx`, `putBIOS`, `shelldemapper`,
`putinfo`) and its unused digital-side encoder (`isdn_init_tx`, `getBIOS`,
`getinfo`, `shellmapper`, `datamerge`), which share the tables [seen]. The
test `k56flex_sm56` (label `sm56`) runs our encoder into sm56's own decoder at
every rate, pad and a set of robbed-bit patterns, and the data comes out
[seen]. Two Rockwell patents describe the same two techniques: shell mapping
for PCM modems (US 6,081,555 and US 6,259,742) and a sign chosen against the
running digital sum, which leaves the shell mapper's rings alone
(US 6,278,744).

### Frames and rates

The downstream is sent in **frames of 8 symbols** (8 kHz codes). A frame
carries R / 1000 data bits, where R is the rate: 32000 to 56000 bit/s in
steps of 2000. The rate code r of the MP gives R = 30000 + 2000·r [seen:
`pstn_init_rx(30 + 2r)` in `SC_Phase4_Rec_MPs`]. Frames go in a cycle of
3 (24 symbols, 4 cycles of the network's 6 symbol robbed-bit pattern).

Each rate has these parameters (index i = (R − 32000) / 2000):

| R (kbit/s) | 32 | 34 | 36 | 38 | 40 | 42 | 44 | 46 | 48 | 50 | 52 | 54 | 56 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| K (shell bits) | 28 | 30 | 24 | 26 | 26 | 28 | 30 | 24 | 25 | 27 | 29 | 31 | 25 |
| u | 1 | 1 | 2 | 2 | 2 | 2 | 2 | 3 | 3 | 3 | 3 | 3 | 4 |
| r (shaping period) | 2 | 2 | 2 | 2 | 4 | 4 | 4 | 4 | 8 | 8 | 8 | 8 | 8 |
| M, 0 dB pad | 13 | 15 | 9 | 11 | 11 | 13 | 15 | 9 | 10 | 11 | 13 | 16 | 9 |
| M, 3 dB pad | 13 | 15 | 9 | 11 | 11 | 13 | 15 | 9 | 10 | 11 | 14 | 16 | 10 |
| M, 6 dB pad | 13 | 15 | 9 | 11 | 11 | 13 | 15 | 9 | 10 | 11 | 13 | 16 | 10 |

K bits a frame go through the shell mapper, which picks 8 rings out of M.
Each symbol then takes q = u − 1 bits to pick one of 2^q points in its ring,
and a sign. Every r-th symbol's sign is not data (see "Shaping"). So a frame
carries K + 8q + 8 − 8/r bits: 28 + 0 + 8 − 4 = 32 at 32000 bit/s, and
25 + 24 + 8 − 1 = 56 at 56000 bit/s. sm56 also has tables for 58000 and
60000 bit/s (r = 16; with r = 16 every other frame's K is one less), which
its client never selects.

### Scrambler and bit order

The data bits are scrambled with V.34's GPA scrambler (1 + x^-5 + x^-23),
one bit at a time, least significant bit of each octet first [seen: the
client's `descramble`]. The frame's bits are then numbered in that order
from 0: bits 0 to K' − 1 are the shell mapper's index, the rest go to the
symbols. (sm56's unused encoder scrambles each octet most significant bit
first, which its own decoder does not undo; the decoder is the reference.)

### The shell mapper

It is V.34's 8-ring shell mapper (9.3/V.34, with V.34's g2, g4, g8 and z8
tables for M rings). The index picks rings m0 to m7; each pair (a, b) of sum
s and index i is a = i, b = s − i when s < M, else b = M − 1 − i,
a = s − b. Ring m0 goes to symbol 0, and so on.

The ring number m is the ring's cost rank, not its place in the
constellation: a per rate and pad table (`k56_ring`) gives the group g of
each ring. The point is p = g · 2^q + (q bits), and the constellation table
(`k56_bios`, M · 2^q entries) gives each point's magnitude as a µ-law code
index (0 the smallest, 127 the largest). For example, at 56000 bit/s with no
pad the 72 magnitudes are 3, 8, 12, 16, 18, ... 101.

### Signs and shaping

A symbol's sign (bit 7 of the code: 1 positive) is a data bit, the symbol's
first: the code's q bits are the next q. Every r-th symbol (counted over the
whole call, across frames) has no sign bit. Its sign is chosen against the
running sum of all sent values: negative when the sum is above 0,
positive otherwise. The values are µ-law's in 14 bit units
(`((2m + 33) << seg) − 33`), or A-law's for an A-law line. This keeps the
downstream's DC near 0, and since a point and its mirror are in the same
ring, the shell mapper is not affected (US 6,278,744).

### Robbed bits

Above 34000 bit/s, the robbed symbols (from the 6 symbol pattern, here the
6 entries that `mask_RBS` fills from the RBS detector: 2 means not robbed)
lose their code's last bit:

- A robbed symbol's q bits have their last bit forced to 0, so it carries one
  bit fewer; the decoder takes the code and its last bit flipped as the same.
- A frame loses L bits, L = 0, 2, 4, 4, 6, 8, 8 for 0 to 6 robbed symbols in 6.
  With 5 robbed, all 6 are treated as robbed.
- With n robbed symbols in a frame (it depends on the frame's place in the 3
  frame cycle), the shell mapper takes K' = K − (L − n) bits, and the index is
  shifted left by L − n, so that its range stays the same. With 3 robbed in 6,
  K' = K and the frame carries R/1000 − n bits instead.

### Codes on the line

The encoder's code is the magnitude index with the sign in bit 7, set for
positive. As a G.711 octet this is 0xff − m (positive) or 0x7f − m (negative)
for µ-law, and the code xor 0x55 for A-law (`k56_g711()`). sm56's encoder
writes its own codes with `c ^ 0x7f` for negative codes and leaves the final
inversion to its hardware layer [seen in `isdn_tx`]. No A-law client has been
tried.

### Data after phase 4

In sm56's encoder, data mode starts right after E, and B1 is data. The
client discards its first 48 symbols (B1) [seen].

## Open questions

- Whether other clients take a 4-level Js (sm56's detector cannot), and
  TRN1C's real length: sm56 takes anything from about 4000 symbols on.
- An A-law client: what sets its law, and whether the INFO1a codes 5 and 13 work. Not tested.
- The meaning of INFO1a's x bit and of NS octets 3 to 7.
- Retrains and rate renegotiation.
