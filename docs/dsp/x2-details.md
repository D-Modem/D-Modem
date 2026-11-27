# x2: technical details

The data on this page come from two sources. The first source is calls
in courier-emu between the original firmware of a USRobotics Courier (the
client) and the original firmware of an I-modem (the server, Ie030002).
The second source is calls from the Courier to D-Modem. We did not copy
x2 code. The code is `dsp/x2.h`, `dsp/x2.cpp` (the downstream of the
server) and `dsp/v34_x2.h` (its part of the V.34 pump).

## Phase 1: V.8

The JM of the server is the JM of V.34, but the server turns over the
sign of the carrier after each complete JM. The CM of the client is the
CM of V.34.

## Phase 2

- The INFO0a of the server has bits 23 and 24 set (as power reduction 1
  with a symbol rate difference of 6, and CME 1).
- After the L1 of the server (sent at the level of L2, 160 ms) and
  silence, the client sends no L1, L2 or INFO1c. It sends a 7 bit marker
  in an INFO frame (35 bits to the end of its CRC). Bit 0 is the carrier
  (1: high). Bits 4 to 6 are the symbol rate of the client (as in V.34).
  The Courier sent `4d`: 3200 baud, high carrier.
- If no marker comes in 800 ms, the server continues as for V.34.

## Phase 3

The client sends the S, S-bar, PP, TRN and J of V.34 at its symbol rate,
on its carrier, with the scrambler of the answerer (GPA). The PCM of the
server is silent until the J of the client. Then the server sends its
training, in G.711 octets:

| Segment | Length | What |
| --- | --- | --- |
| Preamble | 363 | 22 octets 16 times, then their first 11 with the sign turned |
| TRN | 20004 | scrambled ones (GPC from 0) at ±0x31; positive for a scrambled 0 |
| J words | 1200 | the 12 bit word 09B0 99 times, then 099F, scrambled, at ±0x31 |
| Ones | 6000 | scrambled ones at ±0x41 |
| Alignment | 1152 | `9e 28 a1 21 a8 1e`, frame f turned f places left |
| Sync | ≥ 10002 | mapped frames of ones (levels A), until the MP of the client; it ends at the end of a frame |
| Record | whole records | the record of the server, until the E of the client |
| Start | 4086 | one frame of ones in levels A, then ones in the data levels |
| Data | | mapped data frames |

TRN has a fixed length, because the client starts its phase 4 on the J
words.

## Phase 4

After a silence, the client sends these items in sequence:

1. S, S-bar and TRN (the first part at 4 points, then 16).
2. Its MP.
3. MP with the acknowledge bit.
4. E (20 ones).
5. V.34 data.

Records (in the two directions) have this format: 17 ones, a zero, each
16 bit word least significant bit first followed by a zero, then a
CRC-16. The CRC-16 is calculated only on the bits of the words (from
FFFF, polynomial 8408 reflected, not inverted). The MP of the client has
4 words. The Courier sent `0344 03FE 0000 0500`:

- Word 0, bits 2 to 5: the highest downstream rate index that it accepts
  (1).
- Word 1, bits 0 to 11: the upstream rates that it can send (bit n-1 for
  n × 2400 bit/s).
- Word 0, bits 11 to 14: the digital pad that it measured (0: none, 1: 3 dB,
  2: 6 dB. Refer to [Digital pads](#digital-pads)).
- Word 3, high byte: the downstream signs in each frame (5).

The record of the server has 3 words, `F33C 7FFF 0000`. The server pads
it with zeros to 96 bits (4 frames).

## Data

Downstream, each frame takes MD + B scrambled bits (GPC, 1 + x^-18 +
x^-23). The first MD bits are signs. The other B bits, as a number (least
significant bit first), select the level of each position in mixed radix.
The first position is the lowest digit. The levels are sorted largest
first. The MD signs go to the positions of the smallest levels (for equal
levels, the first), in their order. Each sign is sent as a change from the
last sign that was sent in that mode (the first reference is positive).

Where the lists of the positions are different, the level is important,
not the digit. With this rule, the ones of the I-modem after a rate change
decode with no errors. With the digits, about 1.5% of their frames are
incorrect. At 32000 bit/s, B = 19, MD = 5, and each position has the same
9 levels:

- Levels A (before the record): `a5 a7 ad af b7 bd c5 cf e5`.
- Levels B (32000 bit/s): `a5 a8 ab ae b3 b9 bf cb df`.

The sixth sign does not send data. In levels A (the sync segment, the
records and the first frame of the start), we know the rule of the
I-modem. The sixth value is negative when the sum of the other five, as
signed linear values (14 bits), is more than 805. If not, it is positive.
This agrees with all frames that the I-modem sent, and D-Modem uses this
rule.

In the data levels, we do not know the rule, and the client does not
use it. In the emulator, we used the octets of the I-modem and changed
only these signs. The Courier then sent exactly the same data. The rule
does not use only the frame. The I-modem sends the sign negative through
the first frames of the start segment. Then it sets the sign against a
running sum that starts from zero at that point. (A leaky sum agrees with
it about 87% of the time on three calls.) D-Modem sets the sign against
the running sum.

The mapper of the Quad (the lift of QF060003 in courier-emu) has a
different rule. It selects the free sign that gives the smallest magnitude of 8 times a
running sum of eighths of the sent values plus the other
five values of the frame. This is not the rule of the I-modem. On a new I-modem call (32000, then
54666 bit/s, 50,666 frames), it agrees with the free sign of the I-modem
half of the time. A sum that leaks 1% for each sample agrees 85% of the time.

### The rate change

The data starts at the start-up rate. At frame 16882 of the data levels
(counted from 0 in the start segment; 12.66 s), the I-modem sends these
items. They are the same in all calls:

- 8 frames `a5 a5 a5 25 25 25` and one frame `25 25 25 a5 a5 a5`, with no
  changes.
- Its scrambler from zero (and the last sign positive). Then its record
  in levels B (the data levels of the start-up, at all rates: thus also
  after a second marker), 10 times without the acknowledge bit (`733C`)
  and 33 times with it.
- One more frame, then the levels of the next rate (refer to the
  subsequent text). That frame and the first 681 frames of the next rate
  send ones, not data (in the two I-modem calls, the data starts at frame
  681 of the next rate). Without them, the Courier accepts nothing at the
  next rate. It sends flags for 2.5 s, renegotiates again and hangs up.
  With them, the call continues.

The next rate is the rate that the renegotiation MP of the client
accepts (bits 2 to 5 of word 0, n, from 1 to 15). MD is always 5. B is
19, 22 and 25 for n = 1 to 3, and 22 + n for larger n. Thus, from n = 3,
the rate is (27 + n) × 4000/3 bit/s.

The levels for each n are the levels that the firmware of the Quad x2
modem (QF060003, the Total Control x2 server of USRobotics) builds for
it. We ran its bank constructor in courier-emu (as in its
`tools/trace_x2_rate_handoff.py`) with the record that a Courier sends
(words 2 and 3: `0000` and `0500`). Then we read back its six banks. For
n = 1, 8, 11, 12, 13 and 14, they are the lists that the I-modem used
on the line, entry for entry, and n = 1 is levels B. Its mapper (the
lift in courier-emu) gives the same digits and signs as the mapper of
D-Modem for all 15 rates. The D24A table of the I-modem has the same B
for each n.

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

The lists are in `dsp/x2.cpp`. At most rates, the list of the first
position is different from the lists of the other positions. A list can
have more levels than the digits of B bits can select. At 53333, the
digit of the last position goes only to 44. Thus, the frames of the
I-modem show 45 of its 62 levels.

Repeated and crossed I-modem calls show that the settings of the two
sides do not cause it. With all defaults, the first renegotiation of the
Courier accepted 12. Its next renegotiation, about 20 s later, accepted
14. The I-modem changed rate at each renegotiation (the second marker at
the largest level of 52000, `9e`/`9f`). The client expects the levels of
the rate that it accepted and no other levels. A Courier that accepted 14
and got the levels of 52000 renegotiated in 3 s (and accepted 9). Then it
hung up.

D-Modem changes to the rate that each MP accepts. The I-modem does the
same at each renegotiation, to a higher rate or to the same rate (8
three times on an 18 dB line).

At about that time, the Courier renegotiates its upstream on its own
timer. It sends S in data mode, S-bar, a short TRN and its MP (bits 2 to
5 of word 0 now 14 or 12, the mask of word 1 `1FFE`). It sends these on 4
points, also when the TRN of phase 4 went to 16. It waits for the marker
and records before it acknowledges and sends E. Then its upstream
increases (28800 bit/s). If D-Modem did not send the marker and records
yet, it answers the MP with them. If not, it answers with its record
until E.

The MP frames of the Courier come two zeros apart. Each MP frame is a
sync of 17 ones and a zero, then the frame. E is 20 ones in place of the
next sync. Thus, D-Modem accepts 20 ones after a frame as E. The error
bursts of the upstream can occur on E. On lines 8.5 to 10 dB down, with
the frame interrupt of courier-emu that follows the timer of the DSP, a
burst hit the frame before E and E itself in all three calls that we
tried. Without the rule that follows, D-Modem did not find E. It waited
for the end of the renegotiation, and dropped the call 5 s later.

After an MP', D-Modem accepts the 20 bits after a frame as E if 17 or
more are ones and the zero of the sync is not among them. (In those
calls, 2 bits were in error.) It also does this after a frame with a bad
CRC. With this change, the calls at 8.5, 9.5 and 10 dB continue to the
end of the run, and 94 to 98 lines arrive.

In each closed-loop call, the S of the Courier came about 50 ms before
the marker time of the I-modem. If a marker comes before its S, the
Courier renegotiates about 100 ms later all the same, but on 16 points and
without an MP. After 2.6 s of TRN, it hangs up. A second marker in that
TRN does not help. Thus, D-Modem does not send its marker at the time of
the I-modem. It waits for the MP of the Courier and answers it. It sends
the marker 12 s later (frame 32882) only if no renegotiation came before
then. In one call behind a 6 dB pad, the S of the Courier came 3 s after
the time of the I-modem. A marker at 3 s, 0.1 s before the S, caused the
loss of the call.

Upstream is V.34 data at the symbol rate of the client with GPA,
expanded shaping, the 64 state trellis code and non-linear encoding
(9.7/V.34, theta 0.3125). The rate is the highest rate of the mask of the
client up to 12 (28800). The Courier sent at 24000.

The upstream of the emulated Courier has bursts of errors, about 22 each
second at 24000 and also at 28800. Each burst is 20 to 40 symbols of
broadband error near the decision distance in the received signal. The
gain, phase and offset stay stable. The equalizer, echo canceller,
trellis code and theta have no effect on them. They come with the frames
of lower energy of the Courier. Frames with the smaller ring sums have
most of them, and the energy of the signal over them is 10 to 15% below
the mean. The transmitter of D-Modem, through the same receiver at the
same mean square error, decodes with no errors. The emulated I-modem also
saw them. In the run that we recorded with it, it rejected the I-frames
of the Courier two times in its first 14. V.42 recovers by
retransmission. About one RR in ten is lost.

The detection of V.42 starts immediately after E. In the first tenth of a
second, the receiver of D-Modem is not yet stable on the 320 point
constellation. It can decode an unwanted HDLC flag. The detection accepts
two flags in a row as V.42. Thus, it waits for the ODPs of the Courier and
answers them.

### Digital pads

A digital pad is a loss on the digital side of the network: the network
decodes the octet, attenuates it and encodes it again. The pad changes
the levels that the client receives. The Courier measures the pad in the
training and reports it in bits 11 to 14 of word 0 of its MP. Behind a
3 dB pad, it sent `0B44`. Behind a 6 dB pad, it sent `1344`. The bank
constructor of the Quad uses the same field (it calls it the mode). It
builds banks only for 0, 1 and 2.

Each entry has two octets:

- The level that the client receives. The lists, the digits and the
  signs use this level.
- The octet that the server sends, so that this level arrives after the pad.

With 2 (6 dB), the received levels are levels B again, at 32000
bit/s. Behind a 3 dB pad, the octets of the I-modem (rates 1 and 14) are the
octets of the Quad, entry for entry. D-Modem takes the pad from each MP and sends
the octets of the Quad for it. D-Modem refuses a different value, as the Quad does.

Behind a 6 dB pad, the call continued through 52000, 53333 and 54666 bit/s.
In one of these calls, the first renegotiation of the Courier came 3 s after the
marker time of the I-modem, not 50 ms before it (refer to
[The rate change](#the-rate-change)). A marker at 3 s caused the loss of the call.
Behind a 3 dB pad, the call went to 50666 and then 52000 bit/s with the
default timing of courier-emu. With the frame interrupt that follows the
timer of the DSP (`COURIER_INT0_TOUT`), the result was different. The
Courier did not start LAPM, renegotiated 5 s into the data with a request
for rate 0, and hung up. This occurred for all the data that D-Modem
sent. We tried the I-modem only with the default timing.

### A-law

The mapper of the Quad has an A-law flag. This flag only changes its
µ-law octets into the A-law octets with the same code number (XOR `2A`):
the same banks, and the same levels by number. The Courier is a µ-law
modem, and it does not accept this. In courier-emu, on an A-law line,
with those octets, it sent no MP. Thus, D-Modem sends the µ-law levels
without a change, and the codec of the network changes them into the
nearest A-law octets. With these levels, the Courier reported a 3 dB pad
and used the levels of the Quad for it. It got to LAPM, and cleared down
the call about 2 s later. A-law with a Courier is not complete. A client
that supports A-law possibly needs the method of the Quad.

## The client: D-Modem as the analogue modem

D-Modem can also be the x2 client, which is the role of the Courier. It
operates on an analogue line (9600 Hz, `dmodemd -a`) with `AT+MS=X2`,
and calls an x2 server. `DSP_X2C=0` disables it (the call is then V.90).

The client has the same design as the K56flex client (the client states
of `dsp/v34_k56.h` and `K56Rx`; refer to
[the K56flex client](k56flex-details.md#6-the-client-d-modem-as-the-analogue-modem)).
It uses these parts of the V.90 analogue modem:

- the interpolator to 16 000 Hz;
- the T/2 equaliser, its least squares solution and its timing loop
  (`PcmEqualizer`);
- an echo canceller.

The receiver is `X2Rx` (`dsp/x2_rx.h`). The states of the client are in
`dsp/v34_x2.h` (`XC2_MARK` to `XC5_E`).

The client sends what the Courier sends in the calls of courier-emu.
(The times come from the log of the server in a call from a Courier to
D-Modem, on a line 6 dB down.) The client receives the signals of the
server that the sections above give. The server of D-Modem (`X2Train`,
`X2Map`) makes them octet for octet, as the I-modem does.

### Phases 1 and 2

- V.8 is as for a V.34 caller, with only V.34 (no PCM octet, as in the
  CM of the Courier). The server turns over its carrier between its JMs.
  The client does not look for this.
- Our INFO0 is the INFO0 of V.34, with power reduction (bit 20) set, as
  in the INFO0c of the Courier. The server is an x2 server when its INFO0
  has bit 23 (a symbol rate difference of 4 or more) and bit 24 (CME)
  set. If not, the call is V.34.
- The probing is as for a V.34 caller, up to the L1 of the server. Then,
  170 ms after the start of that L1, we do not send L1, L2 and INFO1c.
  In their place, we send two symbols of our tone and then the marker.
  The marker is an INFO frame (with the fill, sync and CRC of V.34) that
  holds the 7 bits `4d`:
  - bit 0: the carrier (1 is high);
  - bits 1 to 3: 6;
  - bits 4 to 6: our symbol rate (3200 baud).
- The server sends nothing back. It is silent until our J.

### Phase 3

- 70 ms after the marker, we send the S, S-bar, PP, TRN (5120 symbols)
  and J (`89B0`: phase 4 at 16 points) of V.34. We send them at
  3200 baud on the high carrier, with the scrambler of the answerer
  (GPA). The echo canceller trains on them. J continues until the
  preamble of the server has been present for 150 ms. Then we are silent.
- `X2Rx` finds the preamble. Its lag-22 correlation (at 8000 Hz) over
  44 samples must be above 0.8 for 88 samples. Then the receiver finds
  the turn, where the correlation over the last 11 samples goes to -1
  (code 362). This gives the code grid. The best correlation with the
  first 96 codes of TRN (in 20 samples at 16 kHz) centres it.
- From there, the codes of the server are known up to the sync segment
  (as `X2Train` makes them). The receiver solves the equaliser by least
  squares on 1020 codes of TRN. Then NLMS follows the known codes. The
  receiver solves the equaliser again when the timing loop is locked
  (from code 6000).
- The signs of TRN go through the GPC descrambler, and the receiver
  decodes the J words from them: `09B0` 99 times, then `099F`. If fewer
  than 90 words are correct, or if `099F` does not come, the call falls
  back to V.34.

### Phase 4

- At the J words (code 20367), we send our phase 4 at 16 points: S,
  S-bar, TRN (5120 symbols), then our MP, `0344 03FE 0000 0500`. The MP
  holds:
  - the rate index 1 of the start-up (as the Courier sends, whatever it
    measured);
  - 13 in bits 6 to 9 (as the Courier sends);
  - the rate mask up to 24000 bit/s;
  - the pad that we measured;
  - 5 signs.

  Behind a pad, words 2 and 3 are `8888 0588`, as the Courier sends.
  Each MP has 2 zeros after it (104 bits, which is a whole number of
  16 point symbols). The MP repeats until the receiver has decoded the
  record of the server.
- The receiver continues through the ones and the alignment pattern,
  which are known. From code 28719, it decides the sync frames: levels A
  in all positions, decoded by the inverse of `X2Map`. (That is: the
  digit of each position, the signs of the 5 smallest levels as changes,
  the B bits in mixed radix, then the GPC descrambler.) The check is 24
  frames of ones in sequence.
- The receiver measures the pad at the end of our phase 4 TRN, from the
  sync frames that it has at that time (about 700). Refer to
  [The pad](#the-pad).
- After our MP, the record of the server follows in the same levels: 17
  ones, a zero, its 3 words (`F33C 7FFF 0000`) and their CRC. Then we
  send our MP' (the acknowledge bit set), and E (20 ones, 5 symbols)
  after a whole MP'.
- After our E, the receiver decodes each frame in two ways: in levels A
  and in the data levels of the start-up. The first frame that the data
  levels decode to ones (with half the error of levels A or less) is the
  first frame of the start segment in those levels. The data starts 680
  frames of ones later.

### Data mode

- The receiver decides each of the six codes of a frame to the nearest
  level in the list of its position. Then the inverse mapper and the
  descrambler give MD + B bits for V.42. The equaliser and the timing
  follow the decisions.
- Upstream is the data encoder of V.34 at the highest rate of our mask
  that the record of the server permits (a maximum of 12: 24000 bit/s
  with our first mask). It uses GPA, expanded shaping, the 64-state code
  and non-linear encoding (theta 0.3125), as the server of D-Modem
  expects.

### Our renegotiation

- 12.7 s into the data, we renegotiate, as the Courier does
  (`DSP_X2C_RENEG=<ms>` sets the time, and 0 disables it). We send S in
  data mode, S-bar, TRN (512 symbols, 4 points), then our MP. The MP has
  the rate index that we measured (bits 2 to 5 of word 0;
  `DSP_X2C_DOWN=<n>` sets a different index) and the mask `1FFE`.
- The receiver waits for the answer of the server: 4 frames of the
  marker (`+++---`, each position at its largest level), then the last
  frame of the marker (`---+++`). From there, the descrambler starts
  from zero, the sign reference is positive, and the levels are the
  levels of the start-up. The records follow (`733C` 10 times, `F33C`
  33 times). At the first record, we send MP', then E. Our upstream then
  continues at 28800 bit/s.
- After the records (170 frames or more), the server sends a frame of
  ones, then ones at the next rate. The receiver selects the rate whose
  levels decode them to ones. It tries the rate that we asked for first,
  then the other rates from 15 down. Thus, the receiver also follows a
  marker that the server starts itself, at a rate that we did not ask
  for. The data at the new rate starts 681 frames later.

### The selection of the rate index

The error of the decisions on the data at the rate of the start-up (or,
before the data, on the sync frames) gives the noise at the receiver.
For each n from 15 down, the receiver finds the smallest distance
between two of its values. (It uses the values as received, after the
gain of the equaliser, and also the distance of a value from its
negative.) We select the highest n where half that distance is a
minimum of 5.5 standard deviations of the noise (an error rate of less
than 1e-7 for each decision).

Behind a pad, the sync frames alone are not sufficient, because the
equaliser is not yet stable on the levels of the pad (the error is
22 rms at first, and 2 rms 10 s later).

This rule is our own. The rule of the Courier (its E2DD metric against
a table of 15 thresholds) is in its firmware. The server accepts any n.

### The pad

The gain of the equaliser removes the loss of the line. Thus, the
receiver finds a pad from the shape of the levels, not from their size:

1. It calculates the mean magnitude of each of the nine levels A in the
   sync frames.
2. It fits these means, with one gain, to each of three candidates: the
   levels A through no pad, through a 3 dB pad and through a 6 dB pad.
   Each candidate is quantised again in µ-law, and scaled by the gain of
   the TRN octet `b1` through the same pad (the equaliser removed that
   gain).
3. The best fit (least squares) gives the pad. The residual against the
   power of the means is about 1e-8 for the correct pad and 1e-4 for the
   others.

The pad goes into bits 11 to 14 of word 0 (0, 1 or 2). From then on, the
receiver decides on the levels of `x2_rates[mode]` (the received `lv`,
at that gain). The Courier fits four candidates in the same way (its
search at `f632`). We do not know which four.

### Tests

- `x2_rx_test`: `X2Rx` on the codes of `X2Train` through a simulated
  line (a short channel, a gain and noise, at 8000 Hz, and at 9600 Hz
  through a resampler a fraction of a sample late). It checks these
  items:
  - the preamble, the J words, the ones of the sync frames, the record
    of the server and the start;
  - 3000 frames of data, which come back bit for bit;
  - behind 3 dB and 6 dB pads, the pad and the data in its levels;
  - a lower rate when the noise increases.
- `interop_test` (`open_x2*`): the open modem as the client (9600 Hz)
  calls the open server (8000 Hz) on netsim, with `AT+MS=X2` on the two
  modems. The call connects at 24000/32000, and the renegotiation goes
  to 28800/56000. Up to 300 000 octets go in each direction. Other tests
  use `NET_PAD=6` (the receiver finds the pad), `NET_FLOOR=20` (a lower
  rate) and `NET_PPM=100` with `NET_PHASE=0.3` (the timing loop finds
  +100 ppm).
- courier-emu: the client of D-Modem (`courier_peer` with
  `COURIER_PEER_ANALOG=1`, the modem at 9600 Hz behind a resampling
  codec, and `COURIER_PEER_DIAL`) calls the emulated I-modem (Ie030002).
  The I-modem answers with the settings and the timing of the Courier
  calls that it answered with x2 (`S54=0S58=48&A3&B1Q0`, the call at
  95 M instructions, `ATA` after it). It does not offer x2 to us: its
  INFO0a has no x2 mark (a difference of 0, CME 0). Thus, our client
  continues as V.34, and the call connects at 31200/33600.

In the courier-emu calls, the DSP of the I-modem gets the same commands
as in the calls of the Courier. Our CM is the CM of the Courier, octet
for octet (`e0 c1 45 10 10 0d`). Our INFO0c has power reduction set, as
the INFO0c of the Courier has. With these items, the I-modem still
answers with V.34. We do not know what else in phase 1 of the Courier
tells the I-modem that the Courier is an x2 client. The own client of
courier-emu (`v90_engine_peer`) also did not get x2 from it. Thus, we
tested the x2 parts of the client only against the server of D-Modem.

### Not implemented

- A-law (we do not know of an x2 client that used A-law; refer to
  [A-law](#a-law)).
- Robbed bits: we do not know how the Courier measures them.
- Symmetric x2 (two digital ends).
- More than one renegotiation of our own, or a retrain. If the line of
  a call changes, the call stays at the rate that it selected.
