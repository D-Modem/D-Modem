# HST: technical details

Nobody published the specification of HST. This page records the sources of the
version of D-Modem (`dsp/dp_hst.cpp`), and the items that we had to
invent. For an overview, refer to [HST](hst.md).

## Where it comes from

The physical layer comes from the DSP firmware of the Courier V.Everything (a
TMS320C5x). The
[courier-emu](https://github.com/Kitsok/courier-emu) project analysed it
(`docs/hst.md`), and for this work we read the code of the DSP and its mask ROM dump
again. Most of the files of courier-emu have no licence.
Thus, D-Modem uses its findings as facts, and copies none of its code.
We read the tables of D-Modem from the ROM dump with our own scripts.
Each value below that does not have the mark of D-Modem comes from the
firmware.

The 80186 supervisor of the Courier runs the start-up, the selection of speed,
the turnaround of the channels and the link layer of HST. Its
start-up comes from two sources:

- A recording of a Courier that answers a
  call (the two directions demodulated, with a good FCS on each HDLC frame).
- The code of the supervisor in the same firmware image (IDSDL302, file
  offsets below).

Its link layer and its channel reversal come from
two emulated Couriers that call each other (refer to the subsequent sections). We did not read its speed changes.

## Forward channel

2400 baud on an 1800 Hz carrier.

| Speed | Bits a symbol | Constellation | Coding |
|---|---|---|---|
| 4800 | 2 | V.32's 4 points | differential, as V.32's 4800 |
| 7200 to 14400 | 3 to 6 | V.32bis's | V.32's 8 state trellis code |
| 16800 | 7 | HST's own 256 points (mask ROM 0x0340) | the same trellis code |

The trellis encoder and the 7200 to 14400 bit/s constellations are
exactly the same as those of V.32bis (`v32_points_*`). Thus, D-Modem uses its own. The 16800
table is `hst_points_16800` in `qam_tables.cpp`: 256 little-endian ROM
words, with the high byte as the real part and the low byte as the imaginary part,
divided by 8.

The scrambler is 1 + x^-18 + x^-23 (`HstScr`), seeded with 0x001f7310.

Training, in symbols:

1. 72 symbols that alternate between the points 0 and 2 of 4800,
2. 200 scrambled 4800 bit/s symbols (3096 for 12000 bit/s and higher).
   The input of the scrambler is ones,
3. 16 symbols (24 for 12000 and higher) of scrambled ones at the data rate,
4. then data.

The input is ones because the symbol engine of the DSP sets it to 0x7f
before each symbol (resident 0xca3f), and the handlers of the training keep
the n bits of it that they must have. (The same handlers put the table index of the point
in that word, but this value is overwritten before the next symbol.)

The script of the firmware starts with 180 symbols of silence. D-Modem
does not send them. The first rate symbol at 4800 is coded from the scrambler output of the last
training symbol. The trellis coder starts with
its differential state set from that output, and its convolutional state
at 0.

## Back channel

Differential 8-PSK on a 350 Hz carrier at 200 baud, with points at 22.5 +
45k degrees. The phase step for three bits 0 to 7 is 1 6 2 5 0 7 3 4
(multiplied by 45 degrees). In data mode, a data symbol and a silent symbol (I = Q
= 0) alternate. Thus, it carries 300 bit/s. Its scrambler is 1 + x^-14 +
x^-17, seeded with 0x0001fd28. Its training is 17 symbols of scrambled
ones, with two data symbols in sequence at the end.

At the sampling instant of the receiver, the silent symbols of the Courier still carry about a fifth of the energy of a data
symbol (its pulse is
narrower in frequency than the pulse of D-Modem). Thus, the receiver finds the silent symbols
by comparison. A silent symbol never follows a different silent symbol, and it is clearly
weaker than the data symbol before it.

The firmware also has a 450 bit/s back channel (375 Hz at 300 baud).
D-Modem does not have it.

## The Courier's start-up

The recording shows these steps, in sequence:

1. The 2100 Hz answer tone of the answerer, then V.8. (This Courier is a
   model of the V.34 period. It answers the CM of the caller with a JM. The caller offered
   only V.22bis.)
2. V.22bis at 2400 bit/s in the two directions.
3. The detection phase of V.42: the ODP of the caller (DC1 with even and odd
   parity), the ADP of the answerer ("EC"), then flags.
4. The XID command of the caller and the XID response of the answerer. The two have
   the user data subfield of V.42 at the end. In this subfield, a Courier puts:

   | Octets | Meaning |
   |---|---|
   | `ff 03 'HST'` | PI 0xFF, the set's name |
   | `c0 02 07 fc` | PI 0xC0: options (bit 0 HST, bit 1 online fallback, bit 2 the 450 bit/s back channel; the bits are cleared by S15's "disable" bits 1 and 2), then the forward rates (bit 2 4800 to bit 7 16800) |
   | `f1 01 06` | always there; meaning unknown |

5. The caller does not send an SABME. It sends a DM with F = 0 (`01 0f`), and
   the two carriers stop. The recording stops at that point.

The code of the supervisor agrees, and gives the remaining steps:

- The XID is built at 0xc479 (command) and 0xc4de (response), and the
  capability at 0x5f70. The parser at 0xd28c stores the capability of the remote in
  0x4ed/0x4ee. The same PI 0xC0 goes into an MNP LR (builder at
  0x5b9e) when the call uses the synchronous service class of MNP instead of
  V.42.
- When the two sides offered HST, the caller (0xd125, on the XID response) and
  the answerer (0xcfbe, on the DM of the caller, which would otherwise ask
  it for an SABME) stop V.42 and change to HST.
- The starting rate (0x60d0) is the first rate of 9600, 7200, 4800, 12000,
  14400 and 16800 that the two sides offered. After that, a Courier increases or
  decreases the rate when it retrains ("Adaptive Speed Leveling", DSP tag 0x36).
- The caller (0x4037) waits 30 ms, configures its DSP and starts the
  back channel immediately (DSP tag 0x32). The answerer (0x40fb, then
  0x10e70) waits about 105 ms (ticks of 5 ms and of 1/2400 s). Then
  it starts the script of the forward channel (DSP tag 0x33). The first
  180 symbols of this script are silent. The answerer goes online after its back channel receiver
  has trained.

The start-up of D-Modem for `+MS=HST` does the same (`link/lapm.cpp`,
`Modem::hst_switch()`, and `HstPump` with `MDMPRM_HST_START`):

1. V.22bis first.
2. The same XID field (options 01: no online fallback or 450 bit/s
   back channel).
3. The DM, then 50 ms of flags.
4. The HST pump at the agreed rate, with no answer tone and no rate request.
   The answerer trains after 180 ms of silence, and the caller starts its back channel immediately.

After the change, the two sides use the link of HST (refer to the subsequent section).

## HST's link layer

After the change, a Courier does not continue with V.42. It has its own link
protocol. We read it from two emulated Couriers that call
each other. We monitored the frames that each DSP received (its receive lanes, with each FCS
checked), with data in the two directions, idle, a hangup and a 20 kB transfer.
The frames are HDLC frames with an FCS-16 and no address. The first
octet is the type of the frame:

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

The frames show these items:

- Each side numbers its data frames from 1, modulo 256 (00 follows
  ff). `nr` is the last frame received in sequence, or 0 for none. A frame
  that comes before its turn gets one REJ. T1 sends the outstanding frames again.
- A maximum of 8 frames are outstanding (after 8, a sender goes back to the oldest frame).
- The two sides start with `05 00 08`. They send it again approximately each 1.5 s
  while idle.
- The side on the back channel asks (`06 k 04`) when it has data to send. In
  the meantime, it sends its data on the back channel, but a Courier itself
  holds its data until the request is granted. The forward side grants (`07 k`)
  after its frames are sent. Then the side that grants starts the back
  channel and trains its receiver (DSP tag 0x32) immediately after the grant
  is sent. The side that asks starts the training of the forward channel (tag
  0x33), as after V.22bis, at the same rate. A Courier with data in the two
  directions turns the channels each 2 to 3 s.
- We do not know the meaning of the last octet of an RR (08). Possibly it is the
  window.
- A hangup sends no frame: the carrier stops.
- In its first second, a Courier also sends series of 1-octet frames
  with N(S) from 0xb0 (`04 b0 74`, ...), out of sequence. The
  receiving Courier ignores them. We do not know why (possibly the cause is the emulation).
  D-Modem also ignores them.

The link of D-Modem (`link/hst_link.cpp`, `HstLink`) does all of this, but not
ASL. It never asks for a different rate, and it does not grant the requests of the remote
for a different rate. (It offers no online fallback in XID, thus a Courier must
not ask.) It sends 64-octet frames. It grants a turn when its frames are
sent and one of these conditions is true: its DTE has no more data, or it has sent 32 frames since
it got the forward channel. While it waits, it asks for a turn before each of its data
frames. The pump turns when the link tells it to
(`MDMPRM_HST_TURN`). The transmitter of its forward channel and the receiver of its back
channel become the transmitter of a back channel and the receiver of a forward
channel, or the opposite. Each one trains again,
as after the start-up.

## D-Modem's own start-up

Without V.42, there is no XID in which the modems can agree on HST. Thus, the pump uses
its own start-up. A Courier does not understand this start-up:

- The answering modem sends the 2100 Hz answer tone, a gap, and then the
  training of the forward channel at 4800 bit/s. Then it continues to send scrambled
  4800 bit/s symbols.
- The calling modem trains on it. It starts its back channel when it sees
  the alternating symbols. After 1200 scrambled symbols, it asks for a
  rate from its measured signal to noise ratio: 7200 at 15.5 dB, 9600
  at 18.5, 12000 at 21.5, 14400 at 24.5, 16800 at 28 (and not higher than
  `AT+MS` permits). The request is the 16-bit word
  0x7e | index << 8 | (~index & 0xf) << 12, first bit first. The caller sends it
  again and again on the back channel.
- The answerer accepts two identical words. It uses the lower of the
  requested rate and its own limit, and sends the rate training (step 3
  above) at that rate.
- The caller finds the rate and the start of the data. It compares what it
  receives with the rate training that each rate would send, because that
  sequence is known before.

Then the two sides send `CONNECT` with the forward rate in one direction and 300 in the other
direction. The link layer (V.42 by default) runs over the two channels as
synchronous bits.

## Not done

- A call to a real Courier.
- The speed changes of the Courier during the call (Adaptive Speed Leveling).
- HST negotiated in the LR of MNP (PI 0xC0) instead of the XID of V.42, as older
  Couriers without V.42 would do.
- The 450 bit/s back channel.
- 16800 bit/s from a Courier to a D-Modem caller has bursts of errors
  in its first second of data (refer to the subsequent section).

## Checked against the Courier's firmware

The author ran the DSP firmware of the Courier (the resident bank of IDSDL302 and the mask
ROM of the C5x) in an emulator that the author wrote, outside this repository. The
DSP commands of the supervisor were scripted in the same method as the HST code at 0x4037
and 0x10e70 sends them (tags 0x1e, 0x39, then 0x32 on the caller and
0x33 on the answerer). Two emulated Couriers connect to each other.
Then we replaced each side with the pump of D-Modem at 7200 Hz, the
sample rate of the firmware:

| D-Modem | Rate | D-Modem receives | the Courier receives |
|---|---|---|---|
| caller | 4800, 9600 | no errors | no errors |
| caller | 16800 | errors in bursts for about a second, then none | no errors |
| answerer | 4800, 9600, 16800 | no errors | no errors |

This work found the scrambler input of the trainings (ones, above) and the silent symbols of the back
channel. It also found that the caller must accept one incorrect
decision when the alternation stops. (The caller looks for 8 symbols of the
scrambled part with a maximum of one incorrect symbol, from its first 24 symbols.)
