# K56flex

K56flex is Rockwell's and Lucent's 56k PCM protocol from before V.90. There is
no public specification. This page records what is known of it from the one
K56flex client we have, Motorola's SM56 soft modem (`sm56.ko`, the
"A56" code; see [interop](../interop.md#against-the-motorola-sm56-soft-modem)),
as the start of a K56flex **server** (digital modem) on the V.90 server code.

Status:

- D-Modem has no K56flex yet: `AT+MS=56` runs V.90 (`Modem::change_dp()`).
- Known: the start-up from the first tone to the end of phase 4 (sections
  1 to 4).
- Not known: the data-mode encoder (section 5). Without it the server can
  train with a K56flex client but not carry data.
- Conexant's HSF engine (`hsfengine.o`) is no help: its K56flex slot is
  disabled (`"NOTSUPP"` in its `+MS` table), and forced on, its CM offers
  no modulation. Its `k56*` files are the PCM base of its V.90 receiver.

Marks: **[seen]** was read in sm56's code or seen on the line in our runs
against it; **[inferred]** is deduced and not proven.

## Overview

K56flex is asymmetric, like V.90. The digital modem (server) sends PCM
codes downstream; the analogue modem (client) sends a V.34-style signal
upstream. The start-up:

| Phase | What happens | Roles |
|---|---|---|
| 1 | V.8bis (the K56flex capability, in a non-standard field), then V.8 selecting V.34 | the server is the V.8bis initiator and the V.8 answerer |
| 2 | V.34 phase 2, with K56flex in the INFO1 frequency offset field | the server is the V.34 *answer* modem (unlike V.90) |
| 3 | downstream PCM training; upstream S, S-bar, PP, TRN, J | the client waits for the server's signal |
| 4 | downstream TRN2, MP, MP', E; upstream J-bar, TRN, MP, MP', E, B1 | |
| data | downstream PCM at 32000 to 56000 bit/s in 2000 bit/s steps; upstream V.34 | encoder not known |

## 1. Phase 1: V.8bis, then V.8

The answering server starts V.8bis as the initiating station (V.8bis) before
its answer tone [seen]:

1. The server sends **MRe** (1375 + 2002 Hz for 400 ms, then 650 Hz for
   100 ms). **CRe** (the dual tone, then 400 Hz) works as well.
2. The client answers **CRd** (1529 + 2225 Hz for 400 ms, then 1900 Hz for
   100 ms), about 175 ms after MRe ends.
3. The server sends **CL** on V.21 channel 1 (980 Hz mark, 1180 Hz space,
   300 bit/s). The framing is HDLC: about 30 ones, 3 flags, octets LSB first with bit
   stuffing, FCS-16 (as HDLC), and 2 flags:

       12 c9 80 80 80 07 b5 02 00 94 81 83 61

4. The client sends **MS** on V.21 channel 2 (1650 / 1850 Hz) about 10 ms
   after the CL. MS has the same layout and carries our non-standard (NS) block back with 0x40
   cleared in its last octet:

       11 c9 80 80 80 07 b5 02 00 94 81 83 21

5. The server sends **ACK(1)**, the single octet `14`.
6. Then the server runs plain V.8: ANSam from it, CM from the client, JM from the server.

Timeline of one run (seconds into the recording): MRe 3.040 to 3.545, CRd
3.720 to 4.225, CL 4.345 to 4.985, MS 4.995 to 5.630, ACK 5.815 to 6.135 [seen].

The message octets [seen]: octet 0 is the message type, in the low nibble
(1 MS, 2 CL, 3 CLR, 4 ACK(1), 5 ACK(2), 8 and up NAKs). Next come the
identification field's NPar(1) octets, up to the one with bit 7 set
(bit 0x40 of the first one: a non-standard field follows), its SPar(1)
octets, the standard field's NPar(1) and SPar(1), and then the NS blocks, each
`[L][L octets]`. The client takes a block as K56flex when all of these hold:

- `L > 6`
- `o[1] == 0xb5` (the T.35 country code for the USA)
- `o[2] == 0x02`
- `o[6] & 1`
- `(o[7] & 0x2f) >= 2`
- `o[7] & 0x40`

It does not check `o[3]` and `o[4]`. A block with `o[6]` bit 0 clear
gets an MS without the NS block, and the call is V.34 [seen].

Our NS block was copied from the client's own CL template (`... 83 21`) with
0x40 added to the last octet. The 0x40 most likely marks the digital modem
[inferred]. When the client also has V.90 on, its MS sets bits 0 and 1 of
`o[6]` [seen in code].

**V.8.** The CM has no K56flex bit [seen]. With `AT+MS=K56FLEX,0` the client's CM is
`e0 c1 45`. With K56FLEX's automode or V90 it is V.90's CM,
`e0 c1 65 13 94 27 0d`. The server's **JM must not select PCM**: if
it selects V.90, the client runs V.90 and drops K56flex. A JM selecting V.34
keeps it [seen].

Notes on the client [seen]:

- sm56 takes K56flex with `+MS=K56FLEX` (with or without automode) and with
  `+MS=V90`, not with V92. With V92 it answers MRe with a quick connect message and
  CRd, and then sends no MS.
- A calling sm56 never starts V.8bis. It answers an initiating signal only if
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
  - `0x001` when the client takes K56flex, its law is µ-law and its tandem-codec check passed;
  - `0x200` (the normal V.34 value) otherwise;
  - bit 3 carries the client's law (1 = A-law), and bit 4 carries the x of an
    earlier INFO1a [inferred].
- **INFO1a** bits 40:49: the server answers with `1 | law << 2 | x << 3`. That is
  1 or 9 for µ-law and 5 or 13 for A-law. Any other value, or a law that is not the client's, makes the
  client go on with V.34 [seen: for µ-law, 1 and 9 left V.34; 0, 2, 3, 5, 13,
  0x11 and 0x200 connected V.34]. The meaning of x is unknown. 1 is the
  value to send to a µ-law client.

The client's **tandem-codec check** reads the received power of the L1/L2
tones near 2100, 3300 and 3750 Hz [inferred from the offsets]. It passes when
P3300 ≥ 0.1·P2100 and P3750 ≥ about 0.018 to 0.020·P3300 [seen]. The server's L1/L2 must therefore be
normal V.34 probing. If the check fails, INFO1c says `0x200`.

## 3. Phase 3

After phase 2 a K56flex client does not run V.34's phase 3 [seen]. Against
our V.34 phase 3 it is silent for about 0.7 s and then retrains. It waits for
the server's downstream signal.

### Client upstream [seen]

The upstream is V.34-shaped and modulated at 3200 baud (the client's codec runs at 9600 Hz), as S, S-bar, PP, TRN and J:

| Signal | Length (T) | Notes |
|---|---|---|
| silence | | until the client has the server's Js |
| S | 128 | |
| S-bar | 16 | |
| PP | 288 | |
| TRN | echo delay + 4000 to 6430 | rate dependent |
| J | | V.34's J (`0x8990` 4-point, `0x89b0` 16-point), 4-point mapped; also carries the client's robbed-bit findings (2 bits) |

The J tells the server how many levels to use in phase 4: `0x8990` means 1 bit
per symbol and `0x89b0` means 2 [seen].

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

The durations in its table, in 8 kHz symbols with -1 meaning "until an event", are:

    0 48 -1 -1 51 390 10 9000 6 7046 8448 4096 6 12273 2000 -1 -1 76 6
    16328 6 2040 14322 1420 62 13999 0

Its phase 3 detector advances when the front end is in TRN1C with more than
4000 symbols counted. It then decodes Js from the signs, as a 16-bit word
MSB first of descrambled bits: `0x991` means 4-level and `0xd91` 16-level,
and the latter moves the client on [seen]. These are V.34's J words
bit-reversed, so the server sends `0x8990` or `0x89b0` LSB first [inferred].
Phase 3 ends when the front end reaches TRN2A.

sm56 also contains a digital-side transmitter (`isdn_tx`) that is never called
[seen]. It agrees with the client on R, TRN1C, Js, TRN2A and TRN2B. It has no
TRN1B of the client's 3-bit kind, though, and its lengths differ from the
client's table, so it may be an older protocol version [inferred]. Its
sequence, with lengths in 8 kHz codes:

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
| TRN1B | 3 bits → one of `0x40 0x41 0x4b 0x4c 0x50 0x51`, then the sign |
| TRN1C, TRN2A | 1 bit → `0x4f` or `0xcf`, corrected for robbed bits and digital loss |
| TRN2B | 3 bits → the 4 magnitudes `0x2f 0x45 0x51 0x57` and a sign |

The scrambler is V.34's (GPA, `1 + x^-5 + x^-23`) [seen].

Code conventions [seen]:

- A code with bit 7 clear is a positive magnitude index `seg << 4 | mant`.
  A negative code is the one's complement of the positive one.
- The magnitude is `((2·mant + 33) << seg) - 33` in the 14-bit scale; `0x4f` is ±975.
- The transmitter converts its code `c` to µ-law as `c & 0x80 ? c ^ 0x7f : c`
  inverted. The final inversion is [inferred]; it makes silence `0xff`.

## 4. Phase 4

The client [seen]:

1. Sends J-bar once the front end is in TRN2A.
2. Waits for 16 descrambled ones followed by 16 zeros: TRN2A's end and MARK.
3. Takes the server's **MP**: V.34's type 0 MP (88 bits), sent as 2 bits per
   symbol with LSB first, descrambled, and checked by V.34's MP detector. `rate_ca`, `rate_ac`
   and the rate mask must all be non-zero, or it clears down.
4. Sets the upstream rate to min(MP's `rate_ca`, 12 or 13) and the
   downstream code r to min(`rate_ac`, its own). The downstream rate is
   30000 + 2000·r bit/s: r = 1 is 32000 and r = 13 is 56000 [inferred].
5. Takes MP' (MP with ack = 1), then E (24 ones), waits 14 symbols, and
   takes B1 (48 symbols, content not checked). Then data mode starts.

The client's own MP is V.34's type 0 with: acknowledge, rate mask `0x7fff`,
asymmetric rates, bits 52 to 59 = 1 and bits 60 to 67 = 0 (not V.34's zeros),
and CRC-16 (as V.34) at bits 69 to 84 [seen].

The server's MP and E symbols: each symbol carries 2 bits (d1, d0) as one of 4
magnitudes, or 1 bit as one of 2. The sign carries no data: on even symbols it
is history bit 24 of the scrambler, and on odd symbols it is chosen against the
running DC sum [seen in `isdn_tx`]. The levels depend on the digital pad (0, 3 or 6 dB) [seen]:

| Pad | 2 bits (`d1 d0` = 0..3) | 1 bit (0, 1) |
|---|---|---|
| 0 dB | `2f 45 51 57` | `55 3d` |
| 3 dB | `35 4d 58 60` | `5d 44` |
| 6 dB | `3d 54 61 67` | `65 4c` |

## 5. Data mode: not known

Downstream, the server encodes the data as PCM codes in frames of 8 symbols,
at 32000 to 56000 bit/s [seen in the client's rate range]. Robbed-bit
signalling lowers the rate per robbed slot [seen]. How the server maps data
bits to codes is **not recovered**, and this page does not describe it.
Without it a K56flex server can train but not connect. The upstream in data
mode is V.34's [inferred].

## Open questions

- The data-mode encoder (section 5).
- When the server ends each R: the inference is R until the client is ready,
  then a reversal and TRN1A; and the second R until the client's J is in, then
  a reversal and TRN2A.
- Which downstream sequence today's clients expect: `isdn_tx`'s or one with
  the client's 3-bit TRN1B.
- The polarity of the Js word.
- An A-law client: what sets its law, and the INFO1a codes 5 and 13. Not tested.
- The meaning of INFO1a's x bit and of NS octets 3 to 7.
- Retrains and rate renegotiation.
