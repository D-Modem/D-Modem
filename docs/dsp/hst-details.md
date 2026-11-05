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
the turning around of the channels and HST's own link layer; none of
that firmware has been read yet.

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
   whose scrambler input is its own last output,
3. 16 symbols (24 for 12000 and up) at the data rate, whose input is
   the last point's index plus an offset (8 when a symbol carries 2
   bits, 2 << n for n bits), cut to n bits,
4. then data.

The firmware's script starts with 180 symbols of silence, which D-Modem
leaves out. The first rate symbol at 4800 is coded from the last
training symbol's scrambler output, and the trellis coder starts with
its differential state set from that output and its convolutional state
at 0.

## Back channel

Differential 8-PSK on a 350 Hz carrier at 200 baud, points at 22.5 +
45k degrees. The phase step for three bits 0 to 7 is 1 6 2 5 0 7 3 4
(times 45 degrees). In data mode a data symbol and a step of zero
alternate, so it carries 300 bit/s. Its scrambler is 1 + x^-14 + x^-17,
seeded with 0x0001fd28. Its training is 17 symbols whose scrambler input
is its own last output, ending with two data symbols in a row.

The firmware also has a 450 bit/s back channel (375 Hz at 300 baud);
D-Modem does not.

## D-Modem's own start-up

Since the Courier's start-up is unknown, D-Modem's is simple and
should be replaced once it is read:

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

- The Courier's real start-up, rate negotiation and channel reversal.
- HST's own link layer (the Courier's error control when it is not
  using V.42 or MNP).
- The 450 bit/s back channel.
- A test against the Courier's firmware itself (courier-emu runs it).
