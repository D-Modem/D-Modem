# K56flex: technical details

This page gives the technical details for [K56flex](k56flex.md). Read that overview first. This page gives the names in the code, the numbers and the test lists.

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
- **[inferred]**: we think that it is true, but we did not prove it.

## Overview

K56flex is asymmetric, as V.90 is. The digital modem (server) sends PCM
codes downstream. The analogue modem (client) sends a signal of the V.34
type upstream. The start-up:

| Phase | What happens | Roles |
|---|---|---|
| 1 | V.8bis (the K56flex capability, in a non-standard field), then V.8, which selects V.34 | the server is the V.8bis initiator and the V.8 answerer |
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
and added 0x40 to the last octet. Possibly, the 0x40 identifies the
digital modem [inferred]. We also set bit 8 of that octet (`... 83 e1`).
The CL parser of the Conexant HCF controller (hcfpcimodem 1.21) reads 2
more octets when bit 8 of the 7th octet is 0, and refuses a 7-octet
block without it [seen in emulation]. The client accepts both [seen].
Refer to [v8bis-details.md](v8bis-details.md#against-other-modems). When the client also has V.90 enabled, its MS
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
| R, TRN2A, MARK, TRN2B | 288, 16336, 32, then TRN2B until the client's MP and 2048 symbols at least; TRN2A starts with `+ - - +` (see below) |
| MP, MP', E | 704, 704, 12 |
| data | B1 first (6 frames of ones) |

After its J, the client waits only about 0.75 s for the second R. Thus,
the server must hear that J (its 24-bit type) and not wait for a timeout
[seen].

#### The second sync of the client

The client finds the start of TRN2A from the end of the R-bar of the
second R (`State_2ndSyncSearch`, `MOD_Sync`) [seen]. `MOD_Sync` keeps
the received signal at 2 samples a symbol. For each symbol, it takes
the difference of each symbol from the symbol before it, at 6 points 4
symbols apart, less the same differences 24 symbols earlier. It does
this on both samples of the symbol, and adds the squares. It adds the
last 4 of these values. The sync is the first symbol where this sum
falls, if the sum before it is more than 9.6 times the energy of the
signal. Then the front end waits 76 symbols (`State_TRN2A_Wait`) and
starts TRN2A with its symbol parity (MOD + 0xb59c) at 1. The sign of
each second TRN2A symbol is against the running sum. sm56 descrambles
only the other symbols.

R-bar is only 24 symbols long, so the sum has its maximum at the last
symbol of R-bar and almost the same value at the first symbol of TRN2A.
In a model of `MOD_Sync` on our codes, the two values are different by
about 0.1 % [inferred]. Thus, the phase of the samples of the client
decides which symbol is the sync. With the later symbol, the parity of
sm56 is incorrect. It then descrambles the signs against the running sum,
it stays in TRN2A (state 19), and it retrains with cause `0x21` [seen
with `SM56_TRACE=1`].

With the codes of `isdn_tx`, our TRN2A starts with `- + + -`. In our
runs, sm56 trained for only 9 of 26 delays of our CL (steps of 4
samples), and the result changed with the start of its DMA. Our server
selects the data bits of the first two scrambled symbols of TRN2A
so that TRN2A always starts with `+ - - +` (`K56Train::next()`). With
this start, sm56 trained for each delay from -10 to 40 samples (steps
of 1 sample) and with each delay of its DMA that we tried (0 to 180
codec samples), with and without robbed bits [seen]. The cost is a
few incorrect descrambled bits at the start of TRN2A, and sm56 trains with
them [seen]. Other starts gave these results for the 26 delays:
`+ - + -` 17 or 21 (this depends on the symbols after it), `- + + -`
and `- + - +` 9 [seen]. `k56flex_sm56` compares our codes with those of
`isdn_tx` with the start of `isdn_tx` (`K56Train::as_isdn`).

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
| 0 dB | `2f 45 51 57` | `3d 55` |
| 3 dB | `35 4d 58 60` | `44 5d` |
| 6 dB | `3d 54 61 67` | `4c 65` |

The 1-bit order is the order of the client (`SC_A56_Sym2Bits`: `0x3d` is 0, `0x55`
is 1) [seen]. The server uses 1 bit for each symbol only in a rate
renegotiation (section 5).

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

The pattern comes from the J of the client [seen in `SC_A56_Gen_JA_RBS`]. The
24-bit J is the J of V.34 and one more octet. This table gives the octet:

| Bits | Meaning |
|---|---|
| 0 to 5 | bit k set: symbol k of the 6 symbol cycle is robbed |
| 6 and 7 | the pad: 0 none, 1 3 dB, 2 6 dB |

Symbol k is counted from the first symbol of TRN1B. The robbed symbols
are on the grid of the network, so k changes with the timing of the call.
For example, with `NET_RBS=0x08` sm56 sent each of the 6 octets
`0x01` to `0x20` for different delays of our CL, and each time it was the
position of the robbed symbol from the TRN1B that netsim passed [seen]. The symbol counter
of the client (MOD + 0x340c) is set to 0 in
`State_TRN1B_RefDirInit`. Then it counts modulo 24 into data mode
[seen]. The decoder of the client takes the robbed pattern from the first
data symbol. Thus, the server must start the data on that grid: the
number of symbols from the start of TRN1B to the first data symbol must
be a multiple of 24. Our server makes TRN2B longer until this is true
(`dsp/v34_k56.h`, `K3_TRN2`) [seen: with NET_RBS on the line to sm56,
its data arrives correctly only with this alignment].

With a pad, the server sends TRN2B, MP, MP' and E at the levels of the pad
(section 4) [seen in `K56_TRN2B_8LVL_CONSTL` and `K56_TRN2B_4LVL_CONSTL`].
The constellation of the data is the constellation of the rate, as without a pad.

### Codes on the line

The code of the encoder is the magnitude index with the sign in bit 7,
set for positive. As a G.711 octet, this is 0xff − m (positive) or
0x7f − m (negative) for µ-law, and the code xor 0x55 for A-law
(`k56_g711()`). The encoder of sm56 writes its own codes with `c ^ 0x7f`
for negative codes, and lets its hardware layer do the final inversion
[seen in `isdn_tx`]. We tried A-law only with our own client
(`open_k56flex_alaw`).

### Data after phase 4

In the encoder of sm56, data mode starts immediately after E, and B1 is
data. The client discards its first 48 symbols (B1) [seen].

### Rate renegotiation

Each of the two modems can start a rate renegotiation in data mode [seen in
`SC_A56Data_Handler`, `SC_A56_Rate_Renego_Init` and
`SC_A56_Phase4_Renego_Init`]:

- The client starts it with the S of V.34. Then the server stops the data at
  the start of a frame and sends R.
- The server starts it with R in place of the data. The client finds R
  in its data (`MOD_R_Det` in `State_Data`) and sends S.

After that, the two modems do phase 4 again from R:

| Direction | Sequence |
|---|---|
| Up (client) | S 128 T, S-bar 16 T, TRN (4 points), MP until the MP of the server, MP', E, B1, data |
| Down (server) | R 288, TRN2A, MARK 32, TRN2B until the MP of the client, MP 704, MP' 704, E 24, data (B1 first) |

These are the differences from the first phase 4 [seen]:

- MP, MP' and E down carry 1 bit for each symbol, as one of 2 magnitudes
  (section 4). Thus, E is 24 symbols. The number of codes from MP to the
  data is 1432. The data must start on the grid of 24 of TRN1B, as
  before.
- sm56 has time limits for each step (`SC_A56_RnegoState_testRtrn`).
  It looks for MARK from 924 symbols into TRN2A. It stops after
  about 5000 symbols. Our server sends 2048 symbols of TRN2A
  (`K56_RN_TRN2A`). The full 16336 symbols of the first phase 4 are too long.
- The front end of sm56 does not do the silence and the first R search. In TRN2B,
  it adapts with different step sizes and for 4096 symbols.

When does the client start a renegotiation? sm56 checks the mean error of
the decisions of its data each 501 interrupts (`SC_A56_Data_setRtrnOrRnego`).
It uses limits that `SC_A56_getRateNegoThd` calculates from the error
that each rate accepts. In this table, L(r) is the error that rate r
accepts:

| Limit | Value | Result |
|---|---|---|
| Down | (L(r) + L(r − 1)) / 2; none at 32000 bit/s | 4 checks in sequence above it: down one rate |
| Retrain | 4 × the down limit | 3 checks in sequence above it: a retrain |
| Up | 0.8 L(r + 1) + 0.2 L(r + 2); none at the top, or at the rate that the last renegotiation went down to | 17 checks in sequence below it: up |

More than 5 renegotiations down also cause a retrain.

Our modems do the same (`dsp/v34_k56.h`):

- The client (`k56c_watch()`) checks each 500 ms. L(r) is
  d(r)² / (91.4 · 0.675), as in its rate rule (section 6). When sm56
  retrains, our client also retrains (refer to "Retrains" below).
- The client finds R in its data: two frames that agree with the first
  16 codes of R2. It finds MARK from the descrambled bits, because the
  server sets the length of TRN2A. When it goes down, it asks for a
  maximum of one rate less.
- The server answers the S of the client. It starts a renegotiation only for
  a test (`DSP_K56_RENEG`).

For a test, `DSP_K56_RENEG=<ms>[:<rate>][a|c]` starts a renegotiation
at that time in the data mode. On the client, rate (1 is 32000 bit/s,
13 is 56000) is the maximum rate that it asks for.

Against sm56 (our server starts the renegotiation), sm56 does all of
phase 4 again and goes back to data mode. But then its decisions in data
mode have about 10 times the error of the first data mode, and
it retrains after about 4 s [seen with `SM56_TRACE=1`]. The cause is
not known. sm56 keeps the code levels that it adapted in the first
data mode (MOD + 0x4040, copied to + 0x3840) [inferred: a possible
cause]. After this retrain, the data is correct again (refer to "Retrains"
below).

### Retrains

A retrain is phase 2 again, as in V.34 (11.5/V.34), and then phases 3
and 4 as in a new call. The modem that starts it sends silence and
then its tone (the client tone B, the server tone A). The other modem
finds the tone in data mode and does the same. There is no new
CONNECT [seen in our modems; the retrain causes of sm56 are in
`SC_A56_Data_setRtrnOrRnego`].

Our modems (`dsp/v34_k56.h`, `V34Retrain::retrain()`):

- In data mode, the server finds the tone B of the client in its
  upstream receiver. The client finds the tone A of the server on the PCM line.
- The client retrains in these conditions, as sm56 does (causes `0x10`
  and `0x23`): its data error is more than 4 times the down limit for 3
  checks in sequence, or the 6th renegotiation down occurs.
- If a phase of a retrain fails (for example, no MP), the modem
  retrains again. After 4 retrains, it stops the call.
- The time limit of a retrain is 30 s, because phase 3 is long.
- For a test, `DSP_V34_RETRAIN=<ms>[a|c]` starts a retrain at that time
  in the data mode. A test renegotiation (`DSP_K56_RENEG`) occurs only
  one time in a call, also when there are retrains.

Against sm56 [seen with `SM56_TRACE=1`]:

- sm56 retrains with tone B, for example after the renegotiation of our
  server (cause `0x10`). Our server answers, and the call goes
  back to data mode with correct data.
- When our server starts the retrain, sm56 does not identify our tone A as
  a retrain. Its front end goes to the R search of a renegotiation
  (state 16), and its time limit expires (cause `0x26` or `0x27`). Its V.34 part does
  phase 2 again with us.
- In phases 3 and 4 of a retrain, sm56 failed about half of the time.
  Its front end stays in TRN2A (state 19) for more than 840 calls of
  `SC_A56Phase4_Handler`, and it retrains with cause `0x21`. Then our
  modem retrains again. This is the fault of the second sync (refer to
  "The second sync of the client"), and the start of TRN2A that our
  server sends possibly removes it [inferred: we did not test the
  retrain again]. `State_TRN2A_DecDir` takes its
  references from the table at MOD + 0x3840. sm56 copies this table from
  the table that it adapted in data mode (+ 0x4040). The first
  training uses the table from power-up [seen]. Thus, the adapted table
  is a possible cause of the bad data after a renegotiation [inferred].
  We did not add the sm56 retrain tests to the test suite, because they
  did not pass each time.

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
   PP 288, TRN 2048, then J (`V34Tx::j24`: `0x89b0` and the octet that
   TRN1B found, 12 symbols for each J) until the TRN2A of the server. The echo
   canceller trains from S until the R of the server. The server is silent
   before R.
4. **The silence and R**: the equaliser is held, and the timing stays on
   its frequency. R is the first loud symbol after a minimum of 12288
   symbols. It is put on the grid of 8 from the start of the silence (the
   server keeps it there).

### Pad and robbed-bit detection

This is a copy of the method of sm56 (`MOD_Det_DL_RBS`, `mask_RBS`) [seen]:

1. During TRN1B, the equaliser adapts only its timing. `K56Rx` adds the
   received value without its sign for each of the 24 positions of the
   24 symbol cycle and each of the 6 TRN1B magnitudes
   (`40 41 4b 4c 50 51`). It ignores the first 240 symbols.
2. A model (`k56_rbs_value()`) gives the value of each magnitude for
   these conditions: a pad (0, 3 or 6 dB), a robbed bit before the pad
   (a), and a robbed bit after the pad (b). The pad is a new G.711
   encoding of the value that is 3 or 6 dB lower. The model gives the
   same values as all 30 rows of the tables `expectTRN1B_Mlaw` and
   `expectTRN1B_Alaw` of sm56 (test `K56flex.RbsTemplates`).
3. For each position, the receiver finds the model and the gain that
   agree best with the 6 means. The pad that most positions find is the
   pad of the line.
4. With that pad, the receiver finds a and b again for each position. A clean
   model wins when the result is almost equal (5 %). The 24 positions become 6: a position of
   the 6 is clean only if all 4 of its repeats are clean.
5. The gain is the mean of the gains of the positions. Then the receiver
   makes a table of the expected value of each code at each position,
   for each sign. The remaining training and the data use this table.

The receiver sends the result in the octet of its J. `DSP_K56_NORBS=1`
disables the detection.

The rate: for each rate, the receiver finds the minimum distance between
two points of the constellation of the rate in the table. At a robbed
position, a code and the same code with its last bit inverted are one point. A rate is
possible if 91.4 · 0.675 · e < d², where e is the error of the equaliser and
d is that distance, and d is 2 or more. This is the rule of sm56
(`MOD_rate_det`) with a measured d. sm56 itself uses fixed tables of d.
With A-law and a 3 dB pad, the rate is 36000 or less, as in sm56. Our MP
asks for that rate or less (`DSP_K56_MAX` makes it lower).

An A-law 6 dB pad moves each value by exactly one segment. Thus, it is
the same as no pad. The receiver finds 0 dB. This has no effect on the
data.

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
| `open_k56flex_pad3` | a 3 dB pad: found, J's octet `0x40`, 54000 |
| `open_k56flex_rbs` | robbed bits (`NET_RBS=0x24`): found as `0x12`, 50000 to 59000 |
| `open_k56flex_pad6_rbs` | a 6 dB pad and robbed bits: J's octet `0x98`, 46000 |
| `open_k56flex_reneg` | the client renegotiates to 50000, then its watch goes up to 56000 again; 200000 octets each way |
| `open_k56flex_reneg_dm` | the server renegotiates (its R in the data) |
| `open_k56flex_noise` | noise from 4 s into the data mode: the error is more than 4 times the limit, the client retrains, and the new training gives 4x000 |
| `open_k56flex_noise_ramp` | noise that increases during 20 s: the client renegotiates down one rate at a time |
| `open_k56flex_retrain` | the client retrains (`DSP_V34_RETRAIN=2000c`), the server finds its tone B; 200000 octets each way |
| `open_k56flex_retrain_dm` | the server retrains, the client finds its tone A |
| `sm56_k56flex_rbs` | sm56 with robbed bits on the codes to it (`NET_RBS_TO=a`): one robbed symbol in its J's octet, data on the grid of 24 |

If the line of a modem is digital, `NET_RBS_TO=a` (or `b`) of netsim
applies `NET_PAD` and `NET_RBS` to the 8000 Hz codes to that modem. This
is for a PCM client, for example sm56, that takes the codes without a
change.

A pad on the line to sm56 causes its training to fail [seen with
`SM56_TRACE=1` in `half_sm56`]. Its error in TRN2B is very large, and it
retrains with cause `0x2a` (no rate possible, from
`SC_Phase4_Wait_TRN`). `State_TRN2B_Init` sets its decision thresholds
from the table at MOD + 0x3840 (the values before the pad), not from
the values of the pad [inferred: a fault in sm56]. Thus, sm56 cannot train
on a line with a pad, with our server or with a different server.

### Not done

- 8000 Hz analogue lines.
- The data of sm56 after a renegotiation, and the training of sm56 in a
  retrain (section 5).

## 7. The firmwares

We read the firmware of two K56flex modems: a Rockwell VDI56SP (its
controller is a C39, a 65C02 CPU) and a Zoom modem with a Lucent chip
set (a Z180 CPU) [seen]. The firmware contains only the code of the
controller. The code of the signal processor is in a mask ROM that we do
not have. Thus, the firmware does not show how the protocol operates.
The octets of the NS block are not in the two firmwares as a literal.

The firmware shows the data that the modems report about a call:

- Rockwell `AT&V` (call report): "RBS Pattern", "Digital Pad" (-3dB,
  -6dB or None), "V8bis K56Flex successful" (or "not successful"),
  "Rate Drop", and the local and remote retrain counts.
- Zoom `ATI11`: "Robbed-Bit Signaling", "Digital Loss (dB)", "Remote
  Server ID" and "Last PCM S PTR". Its table of string pointers starts at
  0x3b2c. Its K56flex rates are 32000 to 60000 bit/s.

## Open questions

- Do other clients accept a 4-level Js (the detector of sm56 cannot)? What
  is the real length of TRN1C? sm56 accepts all lengths from about 4000
  symbols.
- A real A-law client: what sets its law, and does it accept the INFO1a
  codes 5 and 13? We tested only our own client, with code 5.
- The meaning of the x bit of INFO1a and of NS octets 3 to 7.
- Which NS block does a real K56flex server send? The HCF controller of
  Conexant reads 9 octets (`b5 02 00 94 81 83 xx yy zz`) when bit 8 of
  the 7th octet is 0. Our block has 7 octets, with that bit set.
- How does a real K56flex server start TRN2A? With the codes of
  `isdn_tx`, the second sync of sm56 is nearly a tie (refer to "The
  second sync of the client").
- Why is the data of sm56 after a renegotiation incorrect? Why does its
  training in a retrain fail about half of the time? The probable cause
  is the table that sm56 adapts in data mode.
