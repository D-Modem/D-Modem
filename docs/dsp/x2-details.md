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
