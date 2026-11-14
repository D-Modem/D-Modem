# K56flex: technical details

This is the technical companion to [K56flex](k56flex.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

K56flex is Rockwell's and Lucent's 56k PCM protocol from before V.90. There is
no public specification. This page records what is known of it from the one
K56flex client we have, Motorola's SM56 soft modem (`sm56.ko`, its
"A56" code; see [interop](../interop-details.md#against-the-motorola-sm56-soft-modem)).
It was the start of a K56flex **server** (the digital modem) built on
the V.90 server code, and then of a client.

Terms used on this page:

- **PCM** (pulse code modulation): the server sends G.711 codes, one per 8 kHz sample, straight into the network.
- **V.8bis**: the tone and V.21 message exchange that can come before V.8, used here to announce K56flex.
- **NS block**: a non-standard field in a V.8bis message, where a vendor puts its own capabilities.
- **T**: one symbol interval.

Status:

- D-Modem is a K56flex **server**: `AT+MS=56` (or `AT+MS=K56`) on an
  8000 Hz G.711 line answers with V.8bis and K56flex, and with V.90 when the
  caller has no K56flex (`DP_K56` on the V.34 pump, `dsp/v34_k56.h`;
  `Modem::change_dp()` in `core/modem.cpp` maps `DP_K56` to V.90 only when
  no pump is registered for it).
- D-Modem is a K56flex **client** too: `AT+MS=56` on an analogue line at
  9600 Hz calls with K56flex when the answer is V.8bis's MRe, and with V.90
  when it is ANSam (section 6). `DSP_K56A=0` leaves K56flex out. On a
  digital line, calling, it runs V.90 as the digital modem.
- The interop test `sm56_k56flex` (label `sm56`) connects sm56 as the client
  at 44000 bit/s down and 31200 up, with V.42, and passes data [seen].
- The interop tests `open_k56flex*` (label `open`) connect our client to our
  server at 56000 down and 31200 up [seen].
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
| 0 dB | `2f 45 51 57` | `3d 55` |
| 3 dB | `35 4d 58 60` | `44 5d` |
| 6 dB | `3d 54 61 67` | `4c 65` |

The 1-bit order is the client's (`SC_A56_Sym2Bits`: `0x3d` is 0, `0x55`
is 1) [seen]. The server uses 1 bit a symbol only in a rate
renegotiation (section 5).

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

The pattern comes from the client's J [seen in `SC_A56_Gen_JA_RBS`]. In
the 24-bit J (V.34's J and one more octet), the octet is:

| Bits | Meaning |
|---|---|
| 0 to 5 | bit k set: symbol k of the 6 symbol cycle is robbed |
| 6 and 7 | the pad: 0 none, 1 3 dB, 2 6 dB |

Symbol k is counted from the first symbol of TRN1B. The client's
symbol counter (MOD + 0x340c) is set to 0 in
`State_TRN1B_RefDirInit` and then counts modulo 24 into data mode
[seen]. The client's decoder takes the robbed pattern from the first
data symbol. Thus the server must start the data on that grid: the
number of symbols from the start of TRN1B to the first data symbol must
be a multiple of 24. Our server makes TRN2B longer until this is true
(`dsp/v34_k56.h`, `K3_TRN2`) [seen: with NET_RBS on the line to sm56,
its data comes through only with this alignment].

With a pad, the server sends TRN2B, MP, MP' and E at the pad's levels
(section 4) [seen in `K56_TRN2B_8LVL_CONSTL` and `K56_TRN2B_4LVL_CONSTL`].
The data's constellation is the rate's, as without a pad.

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

### Rate renegotiation

Either modem can start a rate renegotiation in data mode [seen in
`SC_A56Data_Handler`, `SC_A56_Rate_Renego_Init` and
`SC_A56_Phase4_Renego_Init`]:

- The client starts it with V.34's S. The server then stops the data at
  a frame's start and sends R.
- The server starts it with R in place of the data. The client finds R
  in its data (`MOD_R_Det` in `State_Data`) and sends S.

After that, both modems do phase 4 again from R:

| Direction | Sequence |
|---|---|
| Up (client) | S 128 T, S-bar 16 T, TRN (4 points), MP until the server's MP, MP', E, B1, data |
| Down (server) | R 288, TRN2A, MARK 32, TRN2B until the client's MP, MP 704, MP' 704, E 24, data (B1 first) |

The differences from the first phase 4 are these [seen]:

- MP, MP' and E down carry 1 bit a symbol, as one of 2 magnitudes
  (section 4). E is thus 24 symbols. The number of codes from MP to the
  data is 1432, and the data must start on TRN1B's grid of 24, as
  before.
- sm56 has time limits for each step (`SC_A56_RnegoState_testRtrn`).
  It looks for MARK from 924 symbols into TRN2A and gives up after
  about 5000. Our server sends 2048 symbols of TRN2A
  (`K56_RN_TRN2A`). The full 16336 of the first phase 4 is too long.
- sm56's front end skips the silence and the first R search. In TRN2B
  it adapts with other step sizes and for 4096 symbols.

When does the client start one? sm56 checks the mean error of its data
decisions every 501 interrupts (`SC_A56_Data_setRtrnOrRnego`). It uses
limits that `SC_A56_getRateNegoThd` gets from the error that each rate
allows. For rate r, with L(r) the error that rate r allows:

| Limit | Value | Result |
|---|---|---|
| Down | (L(r) + L(r − 1)) / 2; none at 32000 bit/s | 4 checks in a row above it: down one rate |
| Retrain | 4 × the down limit | 3 checks in a row above it: a retrain |
| Up | 0.8 L(r + 1) + 0.2 L(r + 2); none at the top, or at the rate that the last renegotiation went down to | 17 checks in a row below it: up |

More than 5 renegotiations down also cause a retrain.

Our modems do the same (`dsp/v34_k56.h`):

- The client (`k56c_watch()`) checks every 500 ms. L(r) is
  d(r)² / (91.4 · 0.675), as in its rate rule (section 6). Our client
  has no K56flex retrain yet. Where sm56 retrains, our client starts a
  renegotiation to the rate that its error allows.
- The client finds R in its data: two frames that agree with R2's first
  16 codes. It finds MARK from the descrambled bits, because the
  server sets the length of TRN2A. It asks for at most one rate less
  when it goes down.
- The server answers the client's S. It starts a renegotiation only for
  a test (`DSP_K56_RENEG`).

`DSP_K56_RENEG=<ms>[:<rate>][a|c]` starts a renegotiation that far into
the data mode, for a test. On the client, rate (1 is 32000 bit/s, 13 is
56000) is the most that it asks for.

Against sm56 (our server starts the renegotiation), sm56 does all of
phase 4 again and goes back to data mode. But its decisions in data
mode then have about 10 times the error of the first data mode, and
it retrains after about 4 s [seen with `SM56_TRACE=1`]. The cause is
not known. sm56 keeps the code levels that it adapted in the first
data mode (MOD + 0x4040, copied to + 0x3840) [inferred: a possible
cause].

## 6. The client: D-Modem as the analogue modem

The client mirrors sm56's, against our server (sections 1 to 5). It is
`dsp/v34_k56.h`'s `KC8_*`, `KC3_*` and `KC4_*` states and the PCM receiver
`K56Rx` (`dsp/k56flex_rx.cpp`).

### Phase 1

- The call progress (`dsp/dp_callprog.cpp`), with `+MS=56`, takes 150 ms
  of 1375 + 2002 Hz (MRe's or CRe's dual tone) as the answer
  (`ANSTONE_V8BIS`) and starts the V.34 pump as the caller.
- The pump waits for MRe to end (its dual tone and its 650 Hz), sends CRd
  150 ms later, takes the CL on V.21's channel 1, checks it for K56flex's
  non-standard block (as `k8_ms_k56()` checks an MS), and sends sm56's MS
  (`11 c9 80 80 80 07 b5 02 00 94 81 83 21`) on channel 2. The server's
  ACK(1) makes K56flex agreed (`k56c_on`). V.90 is then left out of the CM.
- A CL without K56flex, or no CL or ACK(1), goes on to V.8 without it.
  V.8 starts when the server's ANSam has been there 200 ms.
- Without V.8bis (a plain ANSam: a V.90 server) the call is V.90's, as
  with `AT+MS=90` (`open_k56flex_v90`).

### Phase 2

V.34's, the client the call modem. Our INFO1c's frequency offset is 1.
INFO1a's (bits 40:49) must have 1 in its low 2 bits; bit 2 is the law the
PCM receiver decodes. Any other value goes on with V.34.

### Phase 3: the PCM receiver

`K56Rx` takes the line at 9600 Hz to 16 000 Hz (V.90's analogue modem's
interpolator, `V90Interp`) and has V.90's analogue modem's T/2 equaliser
(128 taps), least squares solution and timing loop:

1. **R**: + + − − (2000 Hz) on 8 kHz samples, x(n) close to x(n − 4) and to
   −x(n − 2) for 64 samples; then its reversal (24 symbols before TRN1A),
   where for 4 samples x(n) is −x(n − 4). This gives symbol 0, R's 265th.
2. From there on, the server's training up to Js is `K56Train`'s from R's
   265th symbol, code for code. TRN1A's first 96 symbols centre the delay,
   the next 1020 solve the equaliser, and NLMS follows TRN1A, TRN1B and
   TRN1C on what is known. Once the timing is locked (6000 symbols) it is
   solved again.
3. **Js**: decisions; the even symbols' signs descrambled (from the
   training's own scrambler state at Js's start), 16 bits LSB first, twice.
   Our S follows (`KC3_S`): S 128 T, S-bar 16, PP 288, TRN 2048, then J
   (`V34Tx::j24`: `0x89b0` and the octet that TRN1B found, 12 symbols a J)
   until the server's TRN2A. The echo canceller trains from S until the
   server's R, which it is silent before.
4. **The silence and R**: the equaliser held, the timing on its
   frequency. R is the first loud symbol 12288 symbols on at the least,
   put on the grid of 8 from the silence's start (the server keeps it so).

### Pad and robbed-bit detection

This is a copy of sm56's method (`MOD_Det_DL_RBS`, `mask_RBS`) [seen]:

1. During TRN1B the equaliser adapts only its timing. For each of the
   24 positions of the 24 symbol cycle and each of the 6 TRN1B
   magnitudes (`40 41 4b 4c 50 51`), `K56Rx` adds the received value
   with its sign removed. It ignores the first 240 symbols.
2. A model (`k56_rbs_value()`) gives the value that each magnitude
   becomes for a pad (0, 3 or 6 dB), a robbed bit before the pad (a)
   and one after the pad (b). The pad is a new G.711 encoding of the
   value that is 3 or 6 dB lower. The model gives the same values as
   all 30 rows of sm56's tables `expectTRN1B_Mlaw` and
   `expectTRN1B_Alaw` (test `K56flex.RbsTemplates`).
3. For each position, the receiver finds the model and the gain that
   agree best with the 6 means. The pad that most positions find is the
   line's pad.
4. With that pad, it finds a and b again for each position. A clean
   model wins a near tie (5 %). The 24 positions become 6: a position of
   the 6 is clean only if all 4 of its repeats are clean.
5. The gain is the mean of the positions' gains. The receiver then
   makes a table of the expected value of each code at each position,
   for each sign. The rest of the training and the data use it.

The receiver sends the result in its J's octet. `DSP_K56_NORBS=1`
switches the detection off.

The rate: for each rate, the receiver finds the least distance between
two points of the rate's constellation in the table. At a robbed
position, a code and its last bit flipped are one point. A rate is
possible if 91.4 · 0.675 · e < d², where e is the equaliser's error and
d is that distance, and d is 2 or more. This is sm56's rule
(`MOD_rate_det`) with a measured d. sm56 itself uses fixed tables of d.
With A-law and a 3 dB pad, the rate is 36000 or less, as in sm56. Our MP
asks for that rate or less (`DSP_K56_MAX` makes it lower).

An A-law 6 dB pad moves every value by one segment exactly, so it is
the same as no pad. The receiver finds 0 dB; this has no effect on the
data.

### Phase 4

- R's codes are known; TRN2A and MARK go on decisions of one magnitude
  (`0x4f`), their even symbols' signs descrambled. 32 ones in a row is
  TRN2A: our J' after the whole J, then TRN (16 points when Js asked) for
  2048 T and our MP (rates 14 up, 13 down, mask `0x3fff`, asymmetric).
- From TRN2B on each symbol is one of 4 magnitudes, 2 scrambled bits
  (d0 first). The descrambled bits go through an 88-bit window: a frame
  with V.34's sync and CRC is an MP. The first MP sets the rates (its
  `rate_ac`: down 30000 + 2000·r; its `rate_ca`: up, 16 states) and our
  MP'. The data start 1420 symbols after the first MP (its 16 frames, MP''s
  16, E's 12), or 716 after an MP' that follows an MP 44 symbols before.
- Our E after a whole MP' once the server's MP' is in; then CONNECT.

### Data mode

Each symbol is decided to the nearest magnitude of the rate's
constellation (`K56Dec`'s points) with its sign, the equaliser and the
timing following the decisions. `K56Dec` takes the frames of 8; the
first 6 (B1) are dropped. Up, V.34's data encoder.

### Results [seen]

On netsim's quiet line the equaliser comes to 78 dB, and the data's error
is 0.55 rms (77 dB below the signal). The tests:

| Test | What |
|---|---|
| `open_k56flex` | 56000/31200, 200000 octets each way |
| `open_k56flex_alaw` | A-law (INFO1a 0x5) |
| `open_k56flex_ppm` | our codec 100 ppm fast, 0.3 of a sample off: the timing finds +100 ppm |
| `open_k56flex_echo` | our own echo 10 dB down (`DSP_TEST_ECHO`): the canceller takes 54 dB of it away |
| `open_k56flex_v90` | a V.90 server: V.90 |
| `open_k56flex_pad3` | a 3 dB pad: found, J's octet `0x40`, 54000 |
| `open_k56flex_rbs` | robbed bits (`NET_RBS=0x24`): found as `0x12`, 50000 to 59000 |
| `open_k56flex_pad6_rbs` | a 6 dB pad and robbed bits: J's octet `0x98`, 46000 |
| `open_k56flex_reneg` | the client renegotiates to 50000, then its watch goes up to 56000 again; 200000 octets each way |
| `open_k56flex_reneg_dm` | the server renegotiates (its R in the data) |
| `open_k56flex_noise` | noise from 4 s into the data mode: the client renegotiates to 4x000, and the data has no more code errors |
| `sm56_k56flex_rbs` | sm56 with robbed bits on the codes to it (`NET_RBS_TO=a`): its J's octet `0x04`, data on the grid of 24 |

netsim's `NET_RBS_TO=a` (or `b`) puts `NET_PAD` and `NET_RBS` on the
8000 Hz codes to that modem when its line is digital. This is for a
PCM client, such as sm56, that takes the codes as they are.

A pad on the line to sm56 makes its training fail [seen with
`SM56_TRACE=1` in `half_sm56`]. Its error in TRN2B is very large, and it
retrains with cause `0x2a` (no rate possible, from
`SC_Phase4_Wait_TRN`). `State_TRN2B_Init` sets its decision thresholds
from the table at MOD + 0x3840 (the values before the pad), not from
the pad's values [inferred: a fault in sm56]. Thus sm56 cannot train
on a padded line, with our server or with any other.

### Not done

- 8000 Hz analogue lines and retrains.
- sm56's data after a renegotiation (section 5).

## 7. The firmwares

Two K56flex modems' firmwares were read: a Rockwell VDI56SP (its
controller is a C39, a 65C02 CPU) and a Zoom modem with a Lucent chip
set (a Z180 CPU) [seen]. They contain only the controller's code. The
signal processor's code is in a mask ROM that we do not have, so they
cannot tell how the protocol works. The NS block's octets are not in
either firmware as a literal.

They show what the modems report about a call:

- Rockwell `AT&V` (call report): "RBS Pattern", "Digital Pad" (-3dB,
  -6dB or None), "V8bis K56Flex successful" (or "not successful"),
  "Rate Drop", and the local and remote retrain counts.
- Zoom `ATI11`: "Robbed-Bit Signaling", "Digital Loss (dB)", "Remote
  Server ID" and "Last PCM S PTR". Its string pointer table starts at
  0x3b2c. Its K56flex rates are 32000 to 60000 bit/s.

## Open questions

- Whether other clients take a 4-level Js (sm56's detector cannot), and
  TRN1C's real length: sm56 takes anything from about 4000 symbols on.
- An A-law client: what sets its law, and whether the INFO1a codes 5 and 13 work. Not tested.
- The meaning of INFO1a's x bit and of NS octets 3 to 7.
- Retrains.
- Why sm56's data after a renegotiation is wrong.
