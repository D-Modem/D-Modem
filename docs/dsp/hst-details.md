# HST: technical details

HST's specification was never published. This page records what
D-Modem's version (`dsp/dp_hst.cpp`) is based on and what it had to make
up. For an overview, see [HST](hst.md).

## Where it comes from

The physical layer comes from the Courier V.Everything's DSP firmware (a
TMS320C5x), as analysed by the
[courier-emu](https://github.com/Kitsok/courier-emu) project
(`docs/hst.md`) and by reading the DSP's code and its mask ROM dump
again for this work. courier-emu has no licence for most of its files,
so D-Modem takes its findings as facts and copies none of its code;
D-Modem's tables were read out of the ROM dump with its own scripts.
Every figure below that is not marked as D-Modem's own is the
firmware's.

The Courier's 80186 supervisor runs the start-up, the choice of speed,
the turning around of the channels and HST's own link layer. Its
start-up comes from two sources: a recording of a Courier answering a
call (both directions demodulated, every HDLC frame's FCS good), and
the supervisor's code in the same firmware image (IDSDL302, file
offsets below). Its own link layer and its channel reversal come from
two emulated Couriers calling each other (below); its speed changes
have not been read.

## Forward channel

2400 baud on an 1800 Hz carrier.

| Speed | Bits a symbol | Constellation | Coding |
|---|---|---|---|
| 4800 | 2 | V.32's 4 points | differential, as V.32's 4800 |
| 7200 to 14400 | 3 to 6 | V.32bis's | V.32's 8 state trellis code |
| 16800 | 7 | HST's own 256 points (mask ROM 0x0340) | the same trellis code |

The trellis encoder and the 7200 to 14400 bit/s constellations are
exactly V.32bis's (`v32_points_*`), so D-Modem uses its own. The 16800
table is `hst_points_16800` in `qam_tables.cpp`: 256 little-endian ROM
words, the high byte the real part and the low byte the imaginary part,
divided by 8.

The scrambler is 1 + x^-18 + x^-23 (`HstScr`), seeded with 0x001f7310.

Training, in symbols:

1. 72 symbols alternating between 4800's points 0 and 2,
2. 200 scrambled 4800 bit/s symbols (3096 for 12000 bit/s and up),
   whose scrambler input is ones,
3. 16 symbols (24 for 12000 and up) of scrambled ones at the data rate,
4. then data.

The input is ones because the DSP's symbol engine sets it to 0x7f
before each symbol (resident 0xca3f) and the training's handlers keep
the n bits they need of it. (The point's table index, which the same
handlers leave in that word, is overwritten before the next symbol.)

The firmware's script starts with 180 symbols of silence, which D-Modem
leaves out. The first rate symbol at 4800 is coded from the last
training symbol's scrambler output, and the trellis coder starts with
its differential state set from that output and its convolutional state
at 0.

## Back channel

Differential 8-PSK on a 350 Hz carrier at 200 baud, points at 22.5 +
45k degrees. The phase step for three bits 0 to 7 is 1 6 2 5 0 7 3 4
(times 45 degrees). In data mode a data symbol and a silent one (I = Q
= 0) alternate, so it carries 300 bit/s. Its scrambler is 1 + x^-14 +
x^-17, seeded with 0x0001fd28. Its training is 17 symbols of scrambled
ones, ending with two data symbols in a row.

The Courier's silent symbols still carry about a fifth of a data
symbol's energy at the receiver's sampling instant (its pulse is
narrower in frequency than D-Modem's), so the receiver tells them apart
by comparison: a silent symbol never follows another and is clearly
weaker than the data symbol before it.

The firmware also has a 450 bit/s back channel (375 Hz at 300 baud);
D-Modem does not.

## The Courier's start-up

What the recording shows, in order:

1. The answerer's 2100 Hz answer tone, then V.8 (this Courier, a
   V.34-era model, answers the caller's CM with a JM; the caller offered
   V.22bis alone).
2. V.22bis at 2400 bit/s both ways.
3. V.42's detection phase: the caller's ODP (DC1 with even and odd
   parity), the answerer's ADP ("EC"), then flags.
4. The caller's XID command and the answerer's XID response. Both end
   with V.42's user data subfield, in which a Courier puts:

   | Octets | Meaning |
   |---|---|
   | `ff 03 'HST'` | PI 0xFF, the set's name |
   | `c0 02 07 fc` | PI 0xC0: options (bit 0 HST, bit 1 online fallback, bit 2 the 450 bit/s back channel; the bits are cleared by S15's "disable" bits 1 and 2), then the forward rates (bit 2 4800 to bit 7 16800) |
   | `f1 01 06` | always there; meaning unknown |

5. Instead of an SABME the caller sends a DM with F = 0 (`01 0f`), and
   both carriers stop. That is where the recording ends.

The supervisor's code agrees and fills in the rest:

- The XID is built at 0xc479 (command) and 0xc4de (response), the
  capability at 0x5f70; the parser at 0xd28c stores the remote's in
  0x4ed/0x4ee. The same PI 0xC0 goes into an MNP LR (builder at
  0x5b9e) when the call uses MNP's synchronous service class instead of
  V.42.
- When both offered HST, the caller (0xd125, on the XID response) and
  the answerer (0xcfbe, on the caller's DM, which would otherwise ask
  it for an SABME) leave V.42 for HST.
- The starting rate (0x60d0) is the first of 9600, 7200, 4800, 12000,
  14400 and 16800 that both offered. Afterwards a Courier moves up or
  down by retraining ("Adaptive Speed Leveling", DSP tag 0x36).
- The caller (0x4037) waits 30 ms, configures its DSP and starts the
  back channel at once (DSP tag 0x32). The answerer (0x40fb, then
  0x10e70) waits about 105 ms (ticks of 5 ms and of 1/2400 s) before
  it starts the forward channel's script (DSP tag 0x33), whose first
  180 symbols are silent; it goes online once its back channel receiver
  has trained.

D-Modem's start-up for `+MS=HST` does the same (`link/lapm.cpp`,
`Modem::hst_switch()`, and `HstPump` with `MDMPRM_HST_START`): V.22bis
first, the same XID field (options 01: no online fallback or 450 bit/s
back channel), the DM, 50 ms of flags, then the HST pump at the agreed
rate with no answer tone and no rate request: the answerer's training
after 180 ms of silence, the caller's back channel at once. After the
switch both run HST's own link, below.

## HST's link layer

After the switch a Courier does not go on with V.42: it has a link
protocol of its own. It was read from two emulated Couriers calling
each other, the frames each DSP received (its receive lanes, every FCS
checked), with data both ways, idle, a hangup and a 20 kB transfer.
The frames are HDLC frames with an FCS-16 and no address; the first
octet is the frame's type:

| Frame | Meaning |
|---|---|
| `04 ns data` | data, N(S) `ns`: 64 octets at first, up to 224 once the frames are acknowledged |
| `08 ns nr 08 data` | data with an acknowledgement (the first frame after a turn, often) |
| `s<<4 \| 8 \| l, data` | short data: the low 4 bits of N(S), 1 to 7 octets |
| `05 nr 08` | RR: the other's frames up to `nr` received |
| `03 nr 08` | REJ: from `nr` + 1 again |
| `06 k 04` | the back channel's side asks to turn the channels around; `k` counts its asks, and it asks again with the same `k` until granted |
| `06 k 3x` | asks for the forward rate of index x (ASL; 2 is 9600, 5 is 16800) |
| `07 k` | ask `k` granted |

What the frames show:

- Each side numbers its own data frames from 1, modulo 256 (00 follows
  ff). `nr` is the last frame received in order, 0 for none. A frame
  ahead of its turn gets one REJ; T1 sends the outstanding ones again.
- Up to 8 frames are outstanding (a sender goes back to the oldest
  after 8).
- Both sides start with `05 00 08` and send it again every 1.5 s or so
  while idle.
- The side on the back channel with data to send asks (`06 k 04`). It
  sends its data on the back channel meanwhile, but a Courier itself
  holds its own until it is granted. The forward side grants (`07 k`)
  once its frames are out. Then the granting side starts the back
  channel and trains its receiver (DSP tag 0x32) as soon as the grant
  is out; the asking side starts the forward channel's training (tag
  0x33) as after V.22bis, at the same rate. A Courier with data both
  ways turns the channels every 2 to 3 s.
- What the last octet of an RR (08) means is not known; it may be the
  window.
- A hangup sends no frame: the carrier stops.
- In its first second a Courier also sends runs of 1-octet frames
  with N(S) from 0xb0 (`04 b0 74`, ...), out of order, which the
  receiving Courier drops. Why is not known (it may be the emulation);
  D-Modem drops them too.

D-Modem's link (`link/hst_link.cpp`, `HstLink`) does all of this but
ASL: it never asks for another rate and does not grant the remote's
asks for one (it offers no online fallback in XID, so a Courier should
not ask). It sends 64-octet frames, grants a turn once its frames are
out and either its DTE has nothing more or it has sent 32 frames since
it got the forward channel, and asks for one before each of its data
frames while it waits. The pump turns at the link's word
(`MDMPRM_HST_TURN`): its forward channel's transmitter and back
channel's receiver become a back channel's transmitter and forward
channel's receiver, or the other way round, each trained afresh as
after the start-up.

## D-Modem's own start-up

Without V.42 there is no XID to agree on HST in, so the pump falls back
to a start-up of its own, which a Courier does not understand:

- The answering modem sends 2100 Hz answer tone, a gap, then the
  forward channel's training at 4800 bit/s, and keeps sending scrambled
  4800 bit/s symbols.
- The calling modem trains on it, starts its back channel when it sees
  the alternating symbols, and after 1200 scrambled symbols asks for a
  rate from its measured signal to noise ratio: 7200 at 15.5 dB, 9600
  at 18.5, 12000 at 21.5, 14400 at 24.5, 16800 at 28 (and no higher than
  `AT+MS` allows). The request is the 16-bit word
  0x7e | index << 8 | (~index & 0xf) << 12, first bit first, sent over
  and over on the back channel.
- The answerer takes two identical words, uses the lower of the
  requested rate and its own limit, and sends the rate training (step 3
  above) at that rate.
- The caller finds the rate and where data starts by matching what it
  receives against the rate training each rate would send, since that
  sequence is known in advance.

Then both sides `CONNECT` with the forward rate one way and 300 the
other, and the link layer (V.42 by default) runs over both channels as
synchronous bits.

## Not done

- A call to a real Courier.
- The Courier's speed changes during the call (Adaptive Speed Leveling).
- HST negotiated in MNP's LR (PI 0xC0) instead of V.42's XID, as older
  Couriers without V.42 would.
- The 450 bit/s back channel.
- 16800 bit/s from a Courier to a D-Modem caller has bursts of errors
  in its first second of data (see below).

## Checked against the Courier's firmware

The Courier's DSP firmware (IDSDL302's resident bank and the C5x's mask
ROM) was run in an emulator of my own, outside this repository, with
the supervisor's DSP commands scripted the way the HST code at 0x4037
and 0x10e70 sends them (tags 0x1e, 0x39, then 0x32 on the caller and
0x33 on the answerer). Two emulated Couriers connect to each other;
then each side was replaced by D-Modem's pump at 7200 Hz, the
firmware's sample rate:

| D-Modem | Rate | D-Modem receives | the Courier receives |
|---|---|---|---|
| caller | 4800, 9600 | no errors | no errors |
| caller | 16800 | errors in bursts for about a second, then none | no errors |
| answerer | 4800, 9600, 16800 | no errors | no errors |

This found the trainings' scrambler input (ones, above) and the back
channel's silent symbols, and that the caller has to tolerate a wrong
decision as the alternation ends (it looks for 8 symbols of the
scrambled part with one wrong at most, from its first 24).
