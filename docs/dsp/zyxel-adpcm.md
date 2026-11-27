# ZyXEL ADPCM

Status: in D-Modem (`AT+VSM=134`, `135` and `136`).

## What it is

ZyXEL voice modems (the U-1496 family and the Elite 2864) record and
play voice in their own ADPCM. The coder has 2, 3 or 4 bits for each
sample. The sample rate is 9600 samples a second. D-Modem also accepts
8000 and 7200.

The files of ZyXEL's `vcnvt` program and the `zyxel` formats of
`vgetty` (mgetty) use this coder. D-Modem reads and writes the same
data.

## How to use it

| Command | Coder |
|---|---|
| `AT+VSM=134,9600` | ZyXEL ADPCM, 2 bits a sample |
| `AT+VSM=135,9600` | ZyXEL ADPCM, 3 bits a sample |
| `AT+VSM=136,9600` | ZyXEL ADPCM, 4 bits a sample |

The numbers 134 to 136 are numbers of D-Modem. The firmware of ZyXEL uses
`+VSM=2`, `3` (or `30`) and `4`. In V.253, number 4 is G.711 u-law. Thus,
D-Modem cannot use the numbers of ZyXEL.

## The coder

The coder keeps an accumulator `a` and a step `d` (start: `a = 0`,
`d = 5`). For each code `c` of `n` bits:

1. The sign is the most significant bit of `c`. The magnitude `m` is the
   other bits.
2. The accumulator leaks: `a = (a * 4093 + 2048) >> 12`.
3. `v = (2m + 1) * d`. The accumulator increases or decreases by
   `v >> 1`, as the sign tells. If `d` is odd, `a` increases by 1.
4. The step changes: `d = (d * M[n][m] + 0x2000) >> 14`. The minimum is 5.
5. The output sample is `4a`, clipped to 16 bits.

The tables `M` (one row for each width) are the tables of `vcnvt`. The
firmware has the same tables:

- The 4-bit row is in the Elite 2864I 1.11 image at 0x10BD88 and 0x140EC4.
- The 3-bit row is in the voice overlay of the U-1496 DSP at 0xA6A2C.

With 4 bits, code 0 sets `d` to 4. The encoder never sends code 0. It
sends code 8 in its place.

The encoder takes the difference `e = (x >> 2) - a`. The sign of `e`
gives the sign bit. The magnitude is the number of steps `d` in `|e|`
(the remainder above `d`, subtracted step by step), at most the largest
magnitude.
Then the encoder runs the decoder on the code, so that the two keep the
same `a` and `d`.

## Packing

The first sample is in the most significant bits of an octet. A 3-bit
sample can go across two octets.

## Tests

`dsp/test/voice_codec_test.cpp` (`VoiceCodec.Zyxel`):

- The packing of each width.
- The signal-to-noise ratio of a tone: more than 16, 22 and 27 dB for 2,
  3 and 4 bits (measured: 19.3, 25.9 and 31.4 dB).
- A vector of 24 codes for each width, against the algorithm of mgetty's
  `zyxel.c`.
- The 4-bit encoder never sends code 0.
