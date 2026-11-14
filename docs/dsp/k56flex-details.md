# K56flex: technical details

This page gives the technical details for [K56flex](k56flex.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

K56flex is the 56k PCM protocol of Rockwell and Lucent from before V.90.
There is no public specification. This page records what we know of it
from the one K56flex client that we have, the SM56 soft modem of Motorola
(`sm56.ko`, its "A56" code; refer to
[interop](../interop-details.md#against-the-motorola-sm56-soft-modem)).
It was the start of a K56flex **server** (the digital modem) built on the
V.90 server code, and then of a client.

This page uses these terms:

- **PCM** (pulse code modulation): the server sends G.711 codes, one for each 8 kHz sample, directly into the network.
- **V.8bis**: the tone and V.21 message exchange that can come before V.8. Here, it tells that K56flex is available.
- **NS block**: a non-standard field in a V.8bis message, where a vendor puts its own capabilities.
- **T**: one symbol interval.

Status:

- D-Modem is a K56flex **server**. `AT+MS=56` (or `AT+MS=K56`) on an
  8000 Hz G.711 line answers with V.8bis and K56flex. If the caller has no
  K56flex, it answers with V.90 (`DP_K56` on the V.34 pump,
  `dsp/v34_k56.h`; `Modem::change_dp()` in `core/modem.cpp` maps `DP_K56`
  to V.90 only when no pump is registered for it).
- D-Modem is also a K56flex **client**. `AT+MS=56` on an analogue line at
  9600 Hz calls with K56flex when the answer is the MRe of V.8bis. When the
  answer is ANSam, it calls with V.90 (section 6). `DSP_K56A=0` removes
  K56flex. On a digital line, when it calls, it operates V.90 as the
  digital modem.
- The interop test `sm56_k56flex` (label `sm56`) connects sm56 as the
  client at 44000 bit/s down and 31200 up, with V.42, and sends data
  [seen].
- The interop tests `open_k56flex*` (label `open`) connect our client to
  our server at 56000 down and 31200 up [seen].
- The training is `K56Train` (`dsp/k56flex_train.cpp`). The data mode is
  `K56Enc` and `K56Dec` (`dsp/k56flex.cpp`).
- The HSF engine of Conexant (`hsfengine.o`) does not help. Its K56flex
  slot is disabled (`"NOTSUPP"` in its `+MS` table). When we force it on,
  its CM offers no modulation. Its `k56*` files are the PCM base of its
  V.90 receiver.

Each claim has a mark:

- **[seen]**: we read it in the code of sm56, or saw it on the line in our runs against it.
- **[inferred]**: we deduced it, but did not prove it.

## Overview

K56flex is asymmetric, as V.90 is. The digital modem (server) sends PCM
codes downstream. The analogue modem (client) sends a signal of the V.34
type upstream. The start-up:

| Phase | What happens | Roles |
|---|---|---|
| 1 | V.8bis (the K56flex capability, in a non-standard field), then V.8 selecting V.34 | the server is the V.8bis initiator and the V.8 answerer |
| 2 | V.34 phase 2, with K56flex in the INFO1 frequency offset field | the server is the V.34 *answer* modem (unlike V.90) |
| 3 | downstream PCM training; upstream S, S-bar, PP, TRN, J | the client waits for the server's signal |
| 4 | downstream R, TRN2, MP, MP', E; upstream J', TRN, MP, MP', E, B1 | |
| data | downstream PCM at 32000 to 56000 bit/s in 2000 bit/s steps; upstream V.34 | section 5 |

## 1. Phase 1: V.8bis, then V.8

The answering server starts V.8bis as the initiating station, before its
answer tone [seen]:

1. The server sends **MRe** (1375 + 2002 Hz for 400 ms, then 650 Hz for
   100 ms). **CRe** (the dual tone, then 400 Hz) also operates.
2. The client answers **CRd** (1529 + 2225 Hz for 400 ms, then 1900 Hz for
   100 ms), about 175 ms after MRe ends.
3. The server sends **CL** (capabilities list) on V.21 channel 1 (980 Hz
   mark, 1180 Hz space, 300 bit/s). The framing is similar to HDLC: about
   30 ones, 3 flags, the octets LSB first with bit stuffing, an FCS-16 (as
   in HDLC), and 2 flags:

       12 c9 80 80 80 07 b5 02 00 94 81 83 61

4. The client sends **MS** (mode select) on V.21 channel 2 (1650 / 1850 Hz)
   about 10 ms after the CL. MS has the same layout. It sends our NS block
   back with 0x40 cleared in its last octet:

       11 c9 80 80 80 07 b5 02 00 94 81 83 21

5. The server sends **ACK(1)**, the single octet `14`.
6. Then the server does plain V.8: ANSam from the server, CM from the
   client, JM from the server.

Timeline of one run (seconds into the recording): MRe 3.040 to 3.545, CRd
3.720 to 4.225, CL 4.345 to 4.985, MS 4.995 to 5.630, ACK 5.815 to 6.135
[seen].

The message octets [seen]:

1. Octet 0 is the message type, in the low nibble: 1 MS, 2 CL, 3 CLR, 4 ACK(1), 5 ACK(2), 8 and higher NAKs.
2. Then the NPar(1) octets of the identification field come, up to the octet with bit 7 set. Bit 0x40 of the first one tells that a non-standard field follows.
3. Then the SPar(1) octets of the identification field, and the NPar(1) and SPar(1) of the standard field.
4. Then the NS blocks, each `[L][L octets]`.

The client accepts a block as K56flex when all of these conditions are
true:

- `L > 6`
- `o[1] == 0xb5` (the T.35 country code for the USA)
- `o[2] == 0x02`
- `o[6] & 1`
- `(o[7] & 0x2f) >= 2`
- `o[7] & 0x40`

It does not do a check of `o[3]` and `o[4]`. A block with bit 0 of `o[6]`
clear gets an MS without the NS block, and the call is V.34 [seen].

We copied our NS block from the CL template of the client (`... 83 21`)
and added 0x40 to the last octet. Most possibly, the 0x40 identifies the
digital modem [inferred]. When the client also has V.90 enabled, its MS
sets bits 0 and 1 of `o[6]` [seen in code].

**V.8.** The CM has no K56flex bit [seen]:

- With `AT+MS=K56FLEX,0`, the CM of the client is `e0 c1 45`.
- With the automode of K56FLEX, or with V90, it is the CM of V.90,
  `e0 c1 65 13 94 27 0d`.

The **JM of the server must not select PCM**. If it selects V.90, the
client operates V.90 and stops K56flex. A JM that selects V.34 keeps
K56flex [seen].

Notes on the client [seen]:

- sm56 accepts K56flex with `+MS=K56FLEX` (with or without automode) and
  with `+MS=V90`, but not with V92. With V92, it answers MRe with a quick
  connect message and CRd, and then sends no MS.
- A calling sm56 never starts V.8bis itself. It answers an initiating
  signal only if the signal comes after its blind-dial wait (S6, 2 s). It
  did not answer our MRe 0.3 s after ATD. It answered the same MRe 3 s
  after ATD.
- Without V.8bis (with plain V.8), the client calls V.90 or V.34. It sets
  "K56flex agreed" only on a V.8bis exchange that ends in ACK(1).

## 2. Phase 2: V.34, with K56flex in INFO1

Phase 2 is that of V.34. The server is the V.34 **answer** modem (INFO0a,
tone A, INFO1a on the 2400 Hz carrier) and the client is the call modem
(INFO0c, tone B, INFO1c on 1200 Hz) [seen]. INFO0a, INFO0c and the line
probing are standard V.34 [seen]. K56flex uses the 10-bit **frequency
offset** field of the INFO1 frames:

- **INFO1c** bits 79:88 [seen]:
  - `0x001` when the client accepts K56flex, its law is µ-law, and its
    tandem-codec check passed.
  - `0x200` (the usual V.34 value) in all other conditions.
  - Bit 3 has the law of the client (1 = A-law), and bit 4 has the x of an
    earlier INFO1a [inferred].
- **INFO1a** bits 40:49: the server answers with `1 | law << 2 | x << 3`.
  That is 1 or 9 for µ-law, and 5 or 13 for A-law. If the value is
  different, or the law is not the law of the client, the client
  continues with V.34 [seen: for µ-law, 1 and 9 left V.34; 0, 2, 3, 5, 13,
  0x11 and 0x200 connected V.34]. We do not know the meaning of x. 1 is
  the value to send to a µ-law client.

The **tandem-codec check** of the client looks for a second codec in the
path. It reads the received power of the L1/L2 tones near 2100, 3300 and
3750 Hz [inferred from the offsets]. It passes when P3300 ≥ 0.1·P2100 and
P3750 ≥ about 0.018 to 0.020·P3300 [seen]. Thus, the L1/L2 of the server
must be usual V.34 probing. If the check fails, INFO1c gives `0x200`.

## 3. Phase 3

After phase 2, a K56flex client does not do the phase 3 of V.34 [seen].
In its place, it waits for the downstream signal of the server. Against
our V.34 phase 3, it is silent for about 0.7 s and then retrains.

### Client upstream [seen]

The upstream has the shape of V.34, modulated at 3200 baud (the codec of
the client operates at 9600 Hz). It has S, S-bar, PP, TRN and J:

| Signal | Length (T) | Notes |
|---|---|---|
| silence | | until the client has the server's Js |
| S | 128 | |
| S-bar | 16 | |
| PP | 288 | |
| TRN | echo delay + 4000 to 6430 | rate dependent |
| J | | 24 bits again and again: V.34's J (`0x8990` 4-point, `0x89b0` 16-point), then an octet from `SC_A56_Gen_JA_RBS` (the client's robbed-bit findings; 0 on a clean line), all 4-point mapped and differentially coded as V.34's |

The J tells the server how many levels to use in phase 4: `0x8990` gives
1 bit for each symbol and `0x89b0` gives 2 [seen]. sm56 sends `0x89b0`.
Our receiver (`V34Rx::j24`) accepts two Js 24 bits apart as the J, and
keeps the octet in `j_rbs`.

The phase 4 upstream of the client is that of the V.34 caller, with J' for
S and S-bar: J' (`0x899f`) immediately after the last whole J, then TRN,
MP, MP', E and B1 [seen]. They are 16-point (10.1.3.8 of V.34: TRN with 4
bits for each symbol, also MP and E) when the Js of the server asked for
16 levels, as ours does. `V34Rx::trn16` decodes them from J'.

### Server downstream

The receiver of the client (its "MOD" front end) goes through these
states [seen]:

1. R search, sync search.
2. TRN1A (wait, channel estimate, load balance, reference directed, decision directed).
3. TRN1B (reference directed), then TRN1C (reference directed, transition, decision directed).
4. Js.
5. Second R search, second sync search.
6. TRN2A, then TRN2B.
7. MP and E (decision directed), then B1.
8. Data.

The durations in its table, in 8 kHz symbols (-1 tells "until an event"),
are:

    0 48 -1 -1 51 390 10 9000 6 7046 8448 4096 6 12273 2000 -1 -1 76 6
    16328 6 2040 14322 1420 62 13999 0

In the state list, the index of the length of a state is one more than
the position of its name. Thus, `8448` is the length of TRN1B, and `4096`
the length of the reference-directed part of TRN1C. Its phase 3 detector
(`SC_Phase3_Wait_TRN1B`) moves the front end to the decision-directed part
of TRN1C after 4000 symbols of its reference-directed part. Then
`SC_Phase3_Det_Js` decodes Js from the signs, as a 16-bit word of
descrambled bits, MSB first. `0x991` tells 4-level and `0xd91` tells
16-level. Only `0xd91` moves the client to the next step. After `0x991`,
the detector clears its own flag on the next comparison. Thus, the client
never accepts a 4-level Js [seen]. These are the J words of V.34,
bit-reversed. The server sends `0x89b0`, LSB first [seen]. Then the front
end of the client waits 12273 symbols (silence 2) and looks for the
second R.

sm56 also contains a digital-side transmitter (`isdn_tx`) that the code
never calls [seen]. It agrees with the client on R, TRN1A, TRN1C (its
"TRN1B"), Js, TRN2A and TRN2B. But it has no TRN1B of the 3-bit type of
the client. Thus, it is possibly an older version of the protocol
[inferred]. Without that TRN1B, the client takes our Js 8448 symbols too
early and never finds it [seen]. Its sequence, with lengths in 8 kHz
codes, is:

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

The reference generators of the client (what its decision-directed
training expects) [seen]:

| Segment | Codes |
|---|---|
| TRN1A | scrambled bit → `0x4f` or `0xcf` (its sign) |
| TRN1B | 3 scrambled bits (`MOD_RefGenTRN1B`) → magnitude `40 41 4b 4c 50 51 50 51` (b0 + 2·b1 + 4·b2), then the sign |
| TRN1C, TRN2A | 1 bit → `0x4f` or `0xcf`, corrected for robbed bits and digital loss |
| TRN2B | 3 bits → the 4 magnitudes `0x2f 0x45 0x51 0x57` and a sign |

The scrambler is that of V.34 (GPA, `1 + x^-5 + x^-23`) [seen]. It
continues through TRN1A, TRN1B and TRN1C (the reference generators of the
client share one state). TRN1B scrambles 3 bits each symbol, also on the
sign symbols. The signs of TRN1B: each second sign (from its first symbol)
is the scrambled bit 23 bits back, set for positive. The other signs are
against the running sum [seen in `MOD_RefGenTRN1B`].

The sequence of the server that sm56 accepts, as D-Modem sends it
(`K56Train`):

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

After its J, the client waits only about 0.75 s for the second R. Thus,
the server must hear that J (its 24-bit type) and not wait for a timeout
[seen].

Code conventions [seen]:

- In `isdn_tx`, a code with bit 7 clear is a positive magnitude index,
  `seg << 4 | mant`. A negative code is the one's complement of the
  positive code. It converts its code `c` to µ-law as
  `c & 0x80 ? c ^ 0x7f : c`, inverted. This makes silence `0xff`.
- The magnitude is `((2·mant + 33) << seg) - 33` in the 14-bit scale. `0x4f` is ±975.
- The decoder of the client (`putBIOS`) and D-Modem (`K56Enc`,
  `K56Train`, `k56_g711()`) use bit 7 in the opposite direction, as G.711
  does: set is positive. The reference tables of the client above use that
  convention. When we sent our data with bit 7 negative, it got to the
  client with all signs inverted [seen].

## 4. Phase 4

The client [seen]:

1. Sends J-bar (the J' of V.34, `0x899f`) when the front end is in TRN2A.
2. Waits for 16 descrambled ones followed by 16 zeros: the end of TRN2A
   and MARK.
3. Accepts the **MP** of the server: the type 0 MP of V.34 (88 bits),
   sent as 2 bits for each symbol with LSB first, descrambled, and checked
   by the MP detector of V.34. `rate_ca`, `rate_ac` and the rate mask must
   all be non-zero. If not, it clears down.
4. Sets the upstream rate to min(the `rate_ca` of MP, 12 or 13), and the
   downstream code r to min(`rate_ac`, its own). The downstream rate is
   30000 + 2000·r bit/s. Thus, r = 1 is 32000 and r = 13 is 56000
   [inferred].
5. Accepts MP' (MP with ack = 1), then E (24 ones), waits 14 symbols, and
   accepts B1 (48 symbols; it does not check the content). Then data mode
   starts.

The own MP of the client is the type 0 MP of V.34, with [seen]:

- The acknowledge bit set.
- Rate mask `0x7fff`.
- Asymmetric rates.
- Bits 52 to 59 = 1 and bits 60 to 67 = 0 (not the zeros of V.34).
- A CRC-16 (as in V.34) at bits 69 to 84.

Each MP and E symbol of the server sends 2 bits (d1, d0) as one of 4
magnitudes, or 1 bit as one of 2. The sign sends no data. On even
symbols, it is history bit 24 of the scrambler. On odd symbols, it is
selected against the running DC sum [seen in `isdn_tx`]. The levels
depend on the digital pad (the attenuation of the network: 0, 3 or 6 dB)
[seen]:

| Pad | 2 bits (`d1 d0` = 0..3) | 1 bit (0, 1) |
|---|---|---|
| 0 dB | `2f 45 51 57` | `55 3d` |
| 3 dB | `35 4d 58 60` | `5d 44` |
| 6 dB | `3d 54 61 67` | `65 4c` |

## 5. Data mode

The data mode encoder is `K56Enc` in `dsp/k56flex.cpp`, and its decoder is
`K56Dec`. The constants are in `dsp/k56flex_tables.cpp`. We read them from
the client decoder of sm56 (`pstn_init_rx`, `putBIOS`, `shelldemapper`,
`putinfo`) and its unused digital-side encoder (`isdn_init_tx`, `getBIOS`,
`getinfo`, `shellmapper`, `datamerge`), which share the tables [seen].
The test `k56flex_sm56` (label `sm56`) sends the output of our encoder
into the own decoder of sm56 at all rates, pads and a set of robbed-bit
patterns, and the data comes out [seen]. Two patents of Rockwell give a
description of the same two techniques: shell mapping for PCM modems
(US 6,081,555 and US 6,259,742), and a sign selected against the running
digital sum, which does not change the rings of the shell mapper
(US 6,278,744).

### Frames and rates

The downstream is sent in **frames of 8 symbols** (8 kHz codes). A frame
sends R / 1000 data bits, where R is the rate: 32000 to 56000 bit/s in
steps of 2000. The rate code r of the MP gives R = 30000 + 2000·r [seen:
`pstn_init_rx(30 + 2r)` in `SC_Phase4_Rec_MPs`]. Frames go in a cycle of
3 (24 symbols, 4 cycles of the 6 symbol robbed-bit pattern of the
network).

Each rate has these parameters (index i = (R − 32000) / 2000):

| R (kbit/s) | 32 | 34 | 36 | 38 | 40 | 42 | 44 | 46 | 48 | 50 | 52 | 54 | 56 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| K (shell bits) | 28 | 30 | 24 | 26 | 26 | 28 | 30 | 24 | 25 | 27 | 29 | 31 | 25 |
| u | 1 | 1 | 2 | 2 | 2 | 2 | 2 | 3 | 3 | 3 | 3 | 3 | 4 |
| r (shaping period) | 2 | 2 | 2 | 2 | 4 | 4 | 4 | 4 | 8 | 8 | 8 | 8 | 8 |
| M, 0 dB pad | 13 | 15 | 9 | 11 | 11 | 13 | 15 | 9 | 10 | 11 | 13 | 16 | 9 |
| M, 3 dB pad | 13 | 15 | 9 | 11 | 11 | 13 | 15 | 9 | 10 | 11 | 14 | 16 | 10 |
| M, 6 dB pad | 13 | 15 | 9 | 11 | 11 | 13 | 15 | 9 | 10 | 11 | 13 | 16 | 10 |

K bits in each frame go through the shell mapper, which selects 8 rings
out of M. Then each symbol uses q = u − 1 bits to select one of 2^q points
in its ring, and a sign. The sign of each r-th symbol is not data (refer to
"Shaping"). Thus, a frame sends K + 8q + 8 − 8/r bits: 28 + 0 + 8 − 4 = 32
at 32000 bit/s, and 25 + 24 + 8 − 1 = 56 at 56000 bit/s. sm56 also has
tables for 58000 and 60000 bit/s (r = 16; with r = 16, the K of each second
frame is one less), which its client never selects.

### Scrambler and bit order

The data bits are scrambled with the GPA scrambler of V.34
(1 + x^-5 + x^-23), one bit at a time, least significant bit of each octet
first [seen: the `descramble` of the client]. Then the bits of the frame
are numbered in that order from 0. Bits 0 to K' − 1 are the index of the
shell mapper. The other bits go to the symbols. (The unused encoder of
sm56 scrambles each octet most significant bit first, which its own
decoder does not reverse. The decoder is the reference.)

### The shell mapper

It is the 8-ring shell mapper of V.34 (9.3/V.34, with the g2, g4, g8 and
z8 tables of V.34 for M rings). The index selects rings m0 to m7. Each pair
(a, b) of sum s and index i is a = i, b = s − i when s < M. If not,
b = M − 1 − i, a = s − b. Ring m0 goes to symbol 0, and so on.

The ring number m is the cost rank of the ring, not its position in the
constellation. A table for each rate and pad (`k56_ring`) gives the group
g of each ring. The point is p = g · 2^q + (q bits). The constellation
table (`k56_bios`, M · 2^q entries) gives the magnitude of each point as a
µ-law code index (0 the smallest, 127 the largest). For example, at 56000
bit/s with no pad, the 72 magnitudes are 3, 8, 12, 16, 18, ... 101.

### Signs and shaping

The sign of a symbol (bit 7 of the code: 1 positive) is a data bit, the
first bit of the symbol. The q bits of the code are the next q bits. Each
r-th symbol (counted over the full call, across frames) has no sign bit.
Its sign is selected against the running sum of all sent values: negative
when the sum is above 0, if not positive. The values are those of µ-law in
14 bit units (`((2m + 33) << seg) − 33`), or those of A-law for an A-law
line. This keeps the DC of the downstream near 0. A point and its mirror
are in the same ring. Thus, the shell mapper does not change
(US 6,278,744).

### Robbed bits

Above 34000 bit/s, the robbed symbols lose the last bit of their code.
(The robbed symbols come from the 6 symbol pattern. Here, these are the 6
entries that `mask_RBS` fills from the RBS detector: 2 tells not robbed.)

- The last bit of the q bits of a robbed symbol is forced to 0. Thus, it
  sends one bit fewer. The decoder accepts the code and the code with its
  last bit inverted as the same.
- A frame loses L bits, L = 0, 2, 4, 4, 6, 8, 8 for 0 to 6 robbed symbols
  in 6. With 5 robbed symbols, the encoder operates all 6 as robbed.
- With n robbed symbols in a frame (this depends on the position of the
  frame in the 3 frame cycle), the shell mapper takes K' = K − (L − n)
  bits. The index is shifted left by L − n, so that its range stays the
  same. With 3 robbed in 6, K' = K, and the frame sends R/1000 − n bits.

### Codes on the line

The code of the encoder is the magnitude index with the sign in bit 7,
set for positive. As a G.711 octet, this is 0xff − m (positive) or
0x7f − m (negative) for µ-law, and the code xor 0x55 for A-law
(`k56_g711()`). The encoder of sm56 writes its own codes with `c ^ 0x7f`
for negative codes, and lets its hardware layer do the final inversion
[seen in `isdn_tx`]. Nobody has tried an A-law client.

### Data after phase 4

In the encoder of sm56, data mode starts immediately after E, and B1 is
data. The client discards its first 48 symbols (B1) [seen].

## 6. The client: D-Modem as the analogue modem

The client is the mirror of the client of sm56, against our server
(sections 1 to 5). It is the `KC8_*`, `KC3_*` and `KC4_*` states of
`dsp/v34_k56.h` and the PCM receiver `K56Rx` (`dsp/k56flex_rx.cpp`).

### Phase 1

- The call progress (`dsp/dp_callprog.cpp`), with `+MS=56`, accepts
  150 ms of 1375 + 2002 Hz (the dual tone of MRe or CRe) as the answer
  (`ANSTONE_V8BIS`). It starts the V.34 pump as the caller.
- The pump waits until MRe ends (its dual tone and its 650 Hz). It sends
  CRd 150 ms later, and accepts the CL on channel 1 of V.21. It does a
  check of the CL for the non-standard block of K56flex (as `k8_ms_k56()`
  does a check of an MS). Then it sends the MS of sm56
  (`11 c9 80 80 80 07 b5 02 00 94 81 83 21`) on channel 2. The ACK(1) of
  the server makes K56flex agreed (`k56c_on`). Then V.90 is removed from
  the CM.
- If the CL has no K56flex, or if there is no CL or ACK(1), the pump
  continues to V.8 without it. V.8 starts when the ANSam of the server has
  been there for 200 ms.
- Without V.8bis (a plain ANSam: a V.90 server), the call is a V.90 call,
  as with `AT+MS=90` (`open_k56flex_v90`).

### Phase 2

That of V.34, with the client as the call modem. The frequency offset of
our INFO1c is 1. The frequency offset of INFO1a (bits 40:49) must have 1
in its low 2 bits. Bit 2 is the law that the PCM receiver decodes. With a
different value, the client continues with V.34.

### Phase 3: the PCM receiver

`K56Rx` changes the line from 9600 Hz to 16 000 Hz (the interpolator of
the V.90 analogue modem, `V90Interp`). It has the T/2 equaliser (128
taps), least squares solution and timing loop of the V.90 analogue modem:

1. **R**: + + − − (2000 Hz) on 8 kHz samples: x(n) is near x(n − 4) and
   near −x(n − 2) for 64 samples. Then its reversal (24 symbols before
   TRN1A), where for 4 samples x(n) is −x(n − 4). This gives symbol 0, the
   265th symbol of R.
2. From there, the training of the server up to Js is that of `K56Train`
   from the 265th symbol of R, code for code. The first 96 symbols of
   TRN1A centre the delay. The next 1020 symbols solve the equaliser. NLMS
   follows TRN1A, TRN1B and TRN1C on the known symbols. When the timing is
   locked (6000 symbols), the receiver solves the equaliser again.
3. **Js**: decisions. The signs of the even symbols are descrambled (from
   the state of the scrambler of the training at the start of Js), 16 bits
   LSB first, two times. Our S follows (`KC3_S`): S 128 T, S-bar 16,
   PP 288, TRN 2048, then J (`V34Tx::j24`: `0x89b0` and the robbed-bit
   octet 0, 12 symbols for each J) until the TRN2A of the server. The echo
   canceller trains from S until the R of the server. The server is silent
   before R.
4. **The silence and R**: the equaliser is held, and the timing stays on
   its frequency. R is the first loud symbol after a minimum of 12288
   symbols. It is put on the grid of 8 from the start of the silence (the
   server keeps it there).

### Phase 4

- The codes of R are known. TRN2A and MARK continue on decisions of one
  magnitude (`0x4f`), with the signs of their even symbols descrambled.
  32 ones in a row is TRN2A. Then the client sends our J' after the whole
  J, then TRN (16 points when Js asked for it) for 2048 T, and our MP
  (rates 14 up, 13 down, mask `0x3fff`, asymmetric).
- From TRN2B, each symbol is one of 4 magnitudes, 2 scrambled bits (d0
  first). The descrambled bits go through an 88-bit window. A frame with
  the sync and CRC of V.34 is an MP. The first MP sets the rates (its
  `rate_ac`: down 30000 + 2000·r; its `rate_ca`: up, 16 states) and our
  MP'. The data starts 1420 symbols after the first MP (its 16 frames, the
  16 of MP', the 12 of E). Or it starts 716 symbols after an MP' that
  follows an MP 44 symbols before.
- Our E comes after a whole MP' when the MP' of the server is in. Then
  CONNECT.

### Data mode

The receiver decides each symbol to the nearest magnitude of the
constellation of the rate (the points of `K56Dec`) with its sign. The
equaliser and the timing follow the decisions. `K56Dec` takes the frames
of 8. It discards the first 6 (B1). Up, the data encoder of V.34.

### Results [seen]

On the quiet line of netsim, the equaliser gets to 78 dB, and the error of
the data is 0.55 rms (77 dB below the signal). The tests:

| Test | What |
|---|---|
| `open_k56flex` | 56000/31200, 200000 octets each way |
| `open_k56flex_alaw` | A-law (INFO1a 0x5) |
| `open_k56flex_ppm` | our codec 100 ppm fast, 0.3 of a sample off: the timing finds +100 ppm |
| `open_k56flex_echo` | our own echo 10 dB down (`DSP_TEST_ECHO`): the canceller takes 54 dB of it away |
| `open_k56flex_v90` | a V.90 server: V.90 |

### Not done

- No pad or robbed-bit measurement. The octet of our J is 0, and `K56Dec`
  operates with no pad and no robbed bits. The client of sm56 finds them
  in TRN1A to TRN1C (`SC_A56_Gen_JA_RBS`). We did not read this code.
- 8000 Hz analogue lines, retrains and rate renegotiation.

## Open questions

- Do other clients accept a 4-level Js (the detector of sm56 cannot)? What
  is the real length of TRN1C? sm56 accepts all lengths from about 4000
  symbols.
- An A-law client: what sets its law, and do the INFO1a codes 5 and 13
  operate? Not tested.
- The meaning of the x bit of INFO1a and of NS octets 3 to 7.
- Retrains and rate renegotiation.
