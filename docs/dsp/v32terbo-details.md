# V.32terbo: technical details

V.32terbo's specification was never published, so this page records
what D-Modem's version (in `dsp/dp_v32.cpp`) is based on. For an
overview, see [V.32terbo](v32terbo.md).

## Where it comes from

Public sources say little beyond the speeds: AT&T, 1993, 16800 and
19200 bit/s, 2400 baud, an 1800 Hz carrier, trellis coding. No patent,
datasheet, Usenet post or open-source modem found gives the rate bits,
the constellations or the coding.

The Agere soft modem in the interop tests (`dsp/test/agere/agrsm_core.o`,
Agere SoftModem 2.1.40) has all of it. Agere came out of AT&T
Microelectronics through Lucent, so its V.32 code is AT&T's line of
V.32terbo. The object file is not stripped, and its symbols name each
piece:

| Symbol | What it gave |
|---|---|
| `V32ControlDetermineRate`, `V32ControlDefineE`, `V32ControlRefineR` | the rate bits of R and E, and the rate indexes 12 (16800) and 13 (19200) |
| `V32EncoderSetRate` (its two jump tables) | 7 and 8 data bits a symbol, the maps `cCoded16800Map` and `cCoded19200Map`, and that only these two rates go through `V32NonlinearEncoder` |
| `cCoded16800Map`, `cCoded19200Map` | the 256 and 512 points, as signed bytes |
| `V32NonlinearEncoder`, `V32NonlinearDecoder`, `V32EncoderEnableHighScaleFactor` | the non-linear encoder and its scale |
| `V32TrellisEncodeData` | the same 8 state code and point indexing as V.32bis |

Its V.32bis maps (`cCoded7200Map` to `cCoded14400Map`) are exactly
D-Modem's V.32bis constellations times 14, point for point, which
confirms how its tables are indexed and scaled. The modem's own `+MS`
takes `V32T`, and its connection type list has `v.32/v.32bis/v.32terbo`.

The interop tests then check the result against the modem itself
(below).

## The rate signals

The R and E words are V.32bis's. V.32bis leaves bits B13 and B14 of
them reserved (sent as 0); V.32terbo uses them:

| Bit | Rate |
|---|---|
| B12 | 14400 (V.32bis) |
| B13 | 16800 |
| B14 | 19200 |

Both need B8 (trellis coding) as well, like V.32bis's trellis rates.
Agere's E for 19200 is `F993` (B0 first: B0 to B4, B7, B8, B11, B14,
B15), for 16800 `F995`. A modem offering V.32terbo sends its R with
B13 and B14 set; a V.32bis modem ignores them and answers with a
V.32bis rate, so the start-up needs nothing else.

## Coding and constellations

The 8 state trellis code of V.32bis, unchanged. 16800 bit/s carries 7
data bits a symbol (Q1 Q2 through the coder, Q3 to Q7 uncoded), 19200
carries 8 (Q3 to Q8 uncoded): 256 and 512 point constellations, indexed
as V.32bis's are (Y0, then Y1 Y2, then the uncoded bits). The points lie
on the usual odd-sum integer grid: steps of 10/14 of V.32bis's grid for
16800 (coordinates up to 12), half steps for 19200 (up to 18). Both have
about V.32bis's mean power. Within each of the 8 subsets the points are
4 grid steps apart (squared distance 16), as in V.32bis.

The tables are in `dsp/qam_tables.cpp`.

## The non-linear encoder

At 16800 and 19200 every point is warped before it is sent: points far
from the centre are pushed further out, those near it pulled in a
little. This spreads the outer points, which suffer most from the
companding noise of the digital phone network's G.711 coding. V.34
later standardized the same idea (its non-linear encoder, 9.7/V.34).

Agere's fixed-point code, at its transmit scale (one V.32bis grid unit
is 2048), does, for a point Y:

    s  = ((|Y|^2 >> 14) * 20649) >> 14
    c  = (s * s) >> 15
    b  = (c * s) >> 15
    e  = (2731 s + 137 c + 3 b) >> 12
    g  = ((e * 15027) >> 17) + 15027
    Y' = (Y * g) >> 14

In other words Y' = 0.917 Y (1 + z/6 + z^2/120 + z^3/5040), with z =
0.0098 |Y|^2 in V.32bis grid units: the series of sinh(sqrt z)/sqrt z.
Its decoder applies the inverse series. D-Modem computes the warped
points once (in `dsp/qam_tables.cpp`, with the same integer steps) and
uses them as the constellation: the transmitter sends them and the
Viterbi decoder measures distances to them directly.

Both modems must use the same transmit scale for the warp to match.
`V32EncoderEnableHighScaleFactor` (a scale 1.414 times larger) is turned
on by `V32ControlReset` early in the start-up and off again when the
rate signals begin, so data uses the normal scale. The interop test
confirms it: D-Modem decodes the Agere modem's 19200 bit/s with its own
warped table at a mean squared error of 0.0005 of the signal's power.

## Receiving 512 points

V.32bis's receiver needed two changes to take 19200 bit/s; both apply
to all the trellis rates:

- **The gain is held in data mode.** The AGC follows the signal's power
  over about 30 ms. On a dense constellation that estimate wanders by a
  few per cent, which moves the outer points by as much as the spacing
  of the 512 point constellation. In data mode the gain is now frozen
  and the equalizer follows slow changes. Back to back, data's SNR went
  from about 31 dB to 40 dB on a clean line.
- **B1 is used to train.** Data begins with 128 symbols of scrambled
  ones (B1). The receiver knows its descrambler's register, so it knows
  exactly what the remote sends then, except for the remote's trellis
  encoder state, which no standard fixes (Agere's starts in state 31,
  ours in 0). It follows all 32 states for 8 symbols, picks the one
  that matches, and trains on the true points for the rest of B1. With
  only decisions to go on, the equalizer at 19200 sometimes settled on
  a gain 1 to 2 per cent off, which the outer points cannot take.

The rate each modem asks for comes from its training's error. That
estimate tops out near 31 dB, so the thresholds were set from back to
back runs: 19200 from an estimate of 29.5 dB, 16800 from 26.5 dB. On
`pump_test`'s line that picks 19200 from 34 dB of SNR and 16800 from
26 dB; forced, 19200 runs clean from about 32 dB and 16800 from 24 dB.

## Not done

- **Rate renegotiation.** V.32bis's rate renegotiation is not
  implemented for any of the V.32 family; D-Modem retrains instead.
- **V.8.** V.8 has no code for V.32terbo; it is used only when chosen
  with `AT+MS=V32T`.

## Tests

- `pump_test`: `v32terbo*` (clean, 27 dB, no band limit, 8000 Hz,
  300 ppm clock offset, an echo), and `V32Terbo.Rates` (19200 at 40 dB,
  16800 at 27 dB, 14400 at 25 dB, each without a bit error).
- `agere_232_agere_calls`, `agere_232_open_calls`: the Agere modem
  and D-Modem, both with `AT+MS=V32T`, call each other and connect at
  19200 bit/s both ways (`ctest -L agere`).
