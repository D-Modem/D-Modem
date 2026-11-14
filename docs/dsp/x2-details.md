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
- word 0, bits 11 to 14: the digital pad it measured (0: none, 1: 3 dB,
  2: 6 dB; see [Digital pads](#digital-pads));
- word 3, high byte: the downstream signs a frame (5).

The server's record has 3 words, `F33C 7FFF 0000`, sent padded with
zeros to 96 bits (4 frames).

## Data

Downstream, each frame takes MD + B scrambled bits (GPC, 1 + x^-18 +
x^-23). The first MD are signs; the B others, as a number (least
significant bit first), pick each position's level in mixed radix, the
first position the lowest digit. The levels are sorted largest first.
The MD signs go to the positions of the smallest levels (of equal ones
the first), in their order, each sent as a change from the last one
sent that way (starting from positive). Where the positions' lists
differ it is the level that counts, not the digit: the I-modem's ones
after a rate change decode with no errors that way, and about 1.5% of
their frames go wrong by the digits. At 32000
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

The Quad's mapper (courier-emu's lift of QF060003) has a rule of its
own: the free sign that makes the smallest magnitude of 8 times a
running sum of eighths of the sent values plus the frame's other
five. It is not the I-modem's: on a fresh I-modem call (32000, then
54666 bit/s, 50,666 frames) it agrees with the I-modem's free sign
half the time, against 85% for a sum leaking 1% a sample.

### The rate change

The data starts at the start-up rate. At the data levels' frame 16882
(counting the start segment's from 0; 12.66 s) the I-modem sends, the
same in every call:

- 8 frames `a5 a5 a5 25 25 25` and one `25 25 25 a5 a5 a5`, as they
  are;
- its scrambler from zero (and the last sign positive), then its record
  in levels B (the start-up's data levels, whatever the rate: so after
  a second marker too), 10 times without the acknowledge bit (`733C`)
  and 33 times with it;
- one more frame, then the next rate's levels (see below). That frame and the next
  rate's first 681 carry ones, not data (both I-modem calls; the data
  starts at frame 681 of the next rate). The Courier takes nothing at
  the next rate without them: it sends flags for 2.5 s, renegotiates
  again, and hangs up. With them the call holds.

The next rate is the one the client's renegotiation MP takes (word 0's
bits 2 to 5, n, from 1 to 15), always with MD = 5. B is 19, 22 and 25
for n = 1 to 3 and 22 + n above that, so (27 + n) × 4000/3 bit/s from
n = 3 on.

The levels for every n are the ones the Quad x2 modem's firmware
(QF060003, USRobotics' Total Control x2 server) builds for it. Its
bank constructor was run in courier-emu (as in its
`tools/trace_x2_rate_handoff.py`) with the record a Courier sends
(words 2 and 3: `0000` and `0500`), and its six banks read back. For
n = 1, 8, 11, 12, 13 and 14 they are the lists the I-modem was seen to
use on the line, entry for entry, and n = 1 is levels B. Its mapper
(courier-emu's lift of it) takes the same digits and signs as D-Modem's
for all 15. The I-modem's own D24A table has the same B for each n.

| n | B | bit/s | levels per position |
| --- | --- | --- | --- |
| 1 | 19 | 32000 | 9 in each (levels B) |
| 2 | 22 | 36000 | 12 in the first, 13 in the others |
| 3 | 25 | 40000 | 18 in each |
| 4 | 26 | 41333 | 21, then 20 |
| 5 | 27 | 42666 | 27, then 22 |
| 6 | 28 | 44000 | 28, then 25 |
| 7 | 29 | 45333 | 28, then 29 |
| 8 | 30 | 46666 | 32 in each |
| 9 | 31 | 48000 | 41, then 35 |
| 10 | 32 | 49333 | 42, then 40 |
| 11 | 33 | 50666 | 47, then 45 |
| 12 | 34 | 52000 | 50, then 51 |
| 13 | 35 | 53333 | 52, then 62 |
| 14 | 36 | 54666 | 64 in each |
| 15 | 37 | 56000 | 82, then 70 |

The lists are in `dsp/x2.cpp`. The first position's list differs from
the others' at most rates. A list can be longer than the digits B bits
reach: at 53333 the last position's digit only reaches 44, so the
I-modem's frames show 45 of its 62 levels.

Repeated and crossed I-modem calls show it is not either side's
settings: with all defaults the Courier's first renegotiation took 12
and its next, about 20 s later, 14, and the I-modem changed rate at
each (the second marker at the 52000 levels' largest, `9e`/`9f`). The
client expects the levels of the rate it took and no other: a Courier
that took 14 and got 52000's levels renegotiated within 3 s (taking 9)
and hung up. D-Modem changes to the rate each MP takes, as the I-modem
does at each renegotiation, to a higher rate or the same one (8 three
times on an 18 dB line).

The Courier renegotiates its upstream on a timer of its own about then:
S in data mode, S-bar, a short TRN, its MP (word 0's bits 2 to 5 now
14 or 12, word 1's mask `1FFE`), on 4 points even when phase 4's TRN
went to 16. It waits for the marker and records before it acknowledges
and sends E; then its upstream goes up (28800 bit/s). D-Modem answers
its MP with the marker and records when they have not gone yet, and
otherwise with its record until E.

The Courier's MP frames come two zeros apart, each a sync of 17 ones
and a zero, then the frame; E is 20 ones in place of the next sync, so
D-Modem takes 20 ones after a frame as E. The upstream's error bursts
can land on it: on lines 8.5 to 10 dB down, with courier-emu's frame
interrupt following the DSP's timer, a burst hit the frame before E and
E itself in all three calls tried, so D-Modem missed E, waited for the
renegotiation to end, and dropped the call 5 s later. After an MP' it
now takes the 20 bits after a frame as E when at least 17 are ones and
the sync's zero is not among them (2 bits in error in those calls),
also after a frame with a bad CRC. The calls at 8.5, 9.5 and 10 dB
then hold to the end of the run, with 94 to 98 lines arriving.

The Courier's S came about 50 ms before the I-modem's marker time in
every closed-loop call. A marker before its S makes it renegotiate
about 100 ms later all the same, but on 16 points and without an MP;
after 2.6 s of TRN it hangs up. A second marker in that TRN does not
help. So D-Modem does not send its marker at the I-modem's time: it
waits for the Courier's MP and answers it, and sends the marker 12 s
later (frame 32882) only if no renegotiation has come by then. In one
call behind a 6 dB pad the Courier's S came 3 s after the I-modem's
time, and a marker at 3 s, 0.1 s before it, lost the call.

Upstream is V.34 data at the client's symbol rate with GPA, expanded
shaping, the 64 state trellis code and non-linear encoding (9.7/V.34,
theta 0.3125), at the highest rate of the client's mask up to 12
(28800); the Courier sent at 24000.

The emulated Courier's upstream has bursts of errors, about 22 a
second at 24000 and at 28800 alike. Each is 20 to 40 symbols of
broadband error near the decision distance in the received signal;
the gain, phase and offset hold, and the equalizer, echo canceller,
trellis code and theta make no difference. They come with the
Courier's lower energy frames: frames with the smaller ring sums take
most of them, and the signal's energy over them is 10 to 15% under
the mean. D-Modem's own transmitter through the same receiver at the
same mean square error decodes with none. The emulated I-modem saw
them too: in the run recorded against it, it rejected the Courier's
I-frames twice among its first 14. V.42 recovers by retransmission;
about one RR in ten is lost.

V.42's detection starts right after E. In its first tenth of a second
D-Modem's receiver is still settling into the 320 point constellation
and may decode a stray HDLC flag; the detection takes two flags in a
row as V.42, so it waits for the Courier's ODPs and answers them.

### Digital pads

A digital pad (a loss on the digital side of the network: the octet
decoded, attenuated and coded again) changes the levels the client
receives. The Courier measures it in the training and reports it in its
MP's word 0, bits 11 to 14: behind a 3 dB pad it sent `0B44`, behind
6 dB `1344`. The Quad's bank constructor takes the same field (it calls
it the mode) and builds banks for 0, 1 and 2 only. Each entry has two
octets: the level the client receives, which the lists, the digits and
the signs go by, and the octet the server sends to arrive at it across
the pad. With 2 (6 dB) the levels received are levels B again, at 32000
bit/s. The I-modem's octets behind a 3 dB pad (rates 1 and 14) are the
Quad's, entry for entry. D-Modem takes the pad from each MP and sends
the Quad's octets for it; another value is refused, as the Quad does.

Behind a 6 dB pad the call held through 52000, 53333 and 54666 bit/s.
In one such call the Courier's first renegotiation came 3 s after the
I-modem's marker time, not 50 ms before it (see
[The rate change](#the-rate-change)), and a marker at 3 s lost it. Behind 3 dB it went to 50666
and then 52000 bit/s with courier-emu's default timing. With the frame
interrupt following the DSP's timer (`COURIER_INT0_TOUT`) the Courier
instead set up no LAPM, renegotiated 5 s into the data asking for rate
0, and hung up, whatever D-Modem sent; the I-modem was only tried with
the default timing.

### A-law

The Quad's mapper has an A-law flag that only turns its µ-law octets
into the A-law octets with the same code number (XOR `2A`): the same
banks, the same levels by number. The Courier, a µ-law modem, does not
take that: in courier-emu, on an A-law line, with those octets it sent
no MP. D-Modem instead sends the µ-law levels as they are, which the
network's codec turns into the nearest A-law octets. With them the
Courier reported a 3 dB pad, took the Quad's levels for it, reached LAPM, and cleared the
call down about 2 s later. A-law with a Courier is not done; a client
that knows A-law may need the Quad's way.

## The client: D-Modem as the analogue modem (plan)

D-Modem is so far only x2's server. This is the plan for its client,
the role of the Courier: on an analogue line (9600 Hz, `dmodemd -a`),
with `AT+MS=X2`, calling an x2 server. It follows the K56flex client
(`dsp/v34_k56.h`'s client states and `K56Rx`, see
[K56flex's client](k56flex-details.md#6-the-client-d-modem-as-the-analogue-modem)),
which already has the parts a PCM client needs: V.90's analogue modem's
interpolator to 16 000 Hz, its T/2 equaliser, least squares solution
and timing loop (`PcmEqualizer`), and an echo canceller.

What the client sends is the Courier's, as courier-emu's calls show it
(the times from the server's log of a Courier calling D-Modem, a line
6 dB down). What it receives is the server's of the sections above,
which D-Modem's own server (`X2Train`, `X2Map`) makes octet for octet
as the I-modem does.

### Stages

| Stage | What | Done when |
| --- | --- | --- |
| A | V.8, INFO0, the server's L1, our marker | D-Modem's server takes the marker |
| B | Our phase 3; the PCM receiver trained on the preamble and TRN | the J words decode |
| C | Our phase 4: TRN, MP; the sync frames, the server's record, MP', E | D-Modem's server connects |
| D | Data mode: the frames decoded at 32000 bit/s | V.42 connects, data both ways |
| E | Our renegotiation to the rate measured, the rate change | data at the new rate |
| F | The pad measured and reported | calls through `NET_PAD=3` and `6` |

### A: phases 1 and 2

- V.8 as the caller, with the Courier's CM: `e0 c1 45 10 10 0d` (V.34
  only, no PCM octet). The server turns its carrier over between its
  JMs; the client does not need to look for it.
- Our INFO0 is V.34's (the Courier's: rates `7f`, 3429 allowed, no
  symbol rate difference, CME clear). The server is x2 when its INFO0
  has bits 23 and 24 set (as the Courier: our bit 18, 3200 baud on the
  high carrier, and the server's bit 23 or 24). Else the call is V.34.
- The probing as V.34's caller up to the server's L1. Then, instead of
  our L1, L2 and INFO1c: the marker, an INFO frame (V.34's fill, sync
  and CRC) with the 7 bits `4d` (bit 0 the carrier, 1 high; bits 4 to 6
  our symbol rate, 3200 baud), as soon as the server's L1 (160 ms)
  ends.
- The server sends nothing back: it is silent until our J.

### B: phase 3

- We send V.34's S, S-bar, PP, TRN (about 6300 symbols, 2.0 s; the
  Courier's) and J at 3200 baud on the high carrier, with the
  answerer's scrambler (GPA), and the echo canceller trains on them. J
  goes on until the preamble has begun (the Courier's went on for about
  150 ms into it), then we are silent.
- The receiver (`X2Rx`, after `K56Rx`) finds the preamble: 22 codes of
  two levels (`ab`, `bd`), 16 times. Its period of 22 at 8000 Hz, then
  the sign turned in its last 11, gives the start of the code grid.
- From there the server's octets are known up to the sync segment:
  the preamble, TRN (GPC from 0, at `b1`), the J words, the ones (`c1`)
  and the alignment pattern are `X2Train`'s. The equaliser is solved on
  the preamble's last cycles and the start of TRN, and follows the rest
  on the known codes; the timing locks in TRN (2.5 s).
- The J words are decoded from the signs, to check the grid and the
  descrambler: `09B0` 99 times, then `099F`.
- Failures: no preamble 3 s after our J, or J words that do not decode,
  hang up (as the K56flex client does).

### C: phase 4

- At the J words we send our phase 4 (the Courier's): after a silence,
  S, S-bar, TRN (4 points, then 16), then our MP, `0344 03FE 0000
  0500`: the start-up's rate index 1 (as the Courier, whatever it
  measured), up to 24000 bit/s up, pad 0, 5 signs. It repeats until the
  server's record has been decoded.
- The receiver runs on through the ones (6000 codes) and the alignment
  pattern (1152), still known, and then decides the sync frames: levels
  A in every position, each frame six octets, MD + B = 24 scrambled
  ones. The decoder is `X2Map`'s inverse (as `x2_test`'s `unmap`): the
  level of each position, its digit, the signs of the 5 smallest
  levels (differential), the B bits mixed radix, then GPC's
  descrambler. Ones that come back are the check.
- After our MP the server's record follows in the same levels: 17
  ones, a zero, its 3 words (`F33C 7FFF 0000` from D-Modem and the
  I-modem) with their CRC. Then our MP with the acknowledge bit, and E
  (20 ones) after a whole MP'.
- The start segment: one frame of ones in levels A, then ones in the
  data levels (levels B at 32000 bit/s) for 680 frames, then data.
  Its first frame in levels B is the data's frame 0.

### D: data mode

- Each frame's six codes are decided to the nearest level of their
  position's list (the levels received: `lv` of the rate and pad), with
  their signs. Then the inverse mapper and the descrambler give MD + B
  bits for V.42.
- The equaliser and the timing follow the decisions, as `K56Rx`'s do.
- Upstream: V.34's data encoder at the highest rate of our mask the
  server's record allows, with GPA, expanded shaping, the 64-state code
  and non-linear encoding (theta 0.3125), as D-Modem's server expects.

### E: our renegotiation

- About 12.7 s into the data the Courier renegotiates, and so do we:
  S in data mode, S-bar, a short TRN (4 points), then our MP with the
  rate index we measured (word 0 bits 2 to 5) and the mask `1FFE`.
- We wait for the server's answer in the downstream: the marker (8
  frames `+++---` at the largest level, one `---+++`), its descrambler
  from zero, its records in the start-up's data levels (10 without the
  acknowledge bit, 33 with it). Then our MP' and E.
- After the records, one frame and 681 of ones at the new rate's
  levels, then data. Our upstream goes to 28800 bit/s.
- If the server sends a marker of its own (the I-modem's at frame 16882
  of the data, when we have not renegotiated) we answer it the same
  way.

### The rate index we take

The decisions' error in the sync segment and the start's ones gives
the noise at the receiver. For each n from 15 down, the smallest
distance between two of its levels (as received, after the
equaliser's gain) is compared with that noise; we take the highest n
whose levels are at least 5.5 standard deviations apart (an error rate
under 1e-7 a decision), and at most what `&N` allows. That is our own
rule: the Courier's (its E2DD metric against a table of 15 thresholds)
is in its firmware, and the server takes any n.

### F: the pad

The equaliser's gain takes up the line's loss, so a pad is found by the
shape of the levels, not their size: the nine levels A as received
(their mean magnitudes in the sync segment), fitted with one gain to
each of three candidates (levels A as they are, across a 3 dB pad and
across a 6 dB pad, each requantized in µ-law). The best fit is word 0's
bits 11 to 14 (0, 1, 2), and its levels are the data's (`x2_rates[mode]`).
The Courier fits four candidates in the same way (its search at
`f632`); which four is not known.

### Tests

- `x2_test`: `X2Rx` on `X2Train`'s and `X2Map`'s codes through a
  simulated line (gain, a delay of a fraction of a sample, noise): the
  preamble found, the J words decoded, the sync frames' ones, data back
  bit for bit at every rate and pad.
- `interop_test`: the open modem as the client (9600 Hz) calls the open
  digital modem (8000 Hz) on netsim, both `AT+MS=X2`: connect, the
  rate change, data both ways; with `NET_PPM`, `NET_PHASE`, an echo,
  `NET_PAD=3` and `6`.
- courier-emu: D-Modem's client against the emulated I-modem
  (Ie030002), the original server firmware, through netsim's codec, as
  the closed loop now runs the Courier against D-Modem's server.

### Not in this plan

- A-law (no x2 client is known to have done it; see
  [A-law](#a-law)).
- Robbed bits: the Courier's measurement of them is not known.
- Symmetric x2 (both ends digital).
