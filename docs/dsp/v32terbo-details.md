# V.32terbo: technical details

Nobody published the specification of V.32terbo. Thus, this page records
the sources of the version of D-Modem (in `dsp/dp_v32.cpp`). For an
overview, refer to [V.32terbo](v32terbo.md).

## Where it comes from

Public sources give little information other than the speeds: AT&T, 1993, 16800 and
19200 bit/s, 2400 baud, an 1800 Hz carrier, trellis coding. We found no patent,
datasheet, Usenet post or open-source modem that gives the rate bits,
the constellations or the coding.

The Agere soft modem in the interop tests (`dsp/test/agere/agrsm_core.o`,
Agere SoftModem 2.1.40) has all of this information. Agere came from AT&T
Microelectronics through Lucent. Thus, its V.32 code comes from the V.32terbo of AT&T.
The object file is not stripped, and its symbols give the name of each
part:

| Symbol | What it gave |
|---|---|
| `V32ControlDetermineRate`, `V32ControlDefineE`, `V32ControlRefineR` | the rate bits of R and E, and the rate indexes 12 (16800) and 13 (19200) |
| `V32EncoderSetRate` (its two jump tables) | 7 and 8 data bits for each symbol, the maps `cCoded16800Map` and `cCoded19200Map`, and the fact that only these two rates go through `V32NonlinearEncoder` |
| `cCoded16800Map`, `cCoded19200Map` | the 256 and 512 points, as signed bytes |
| `V32NonlinearEncoder`, `V32NonlinearDecoder`, `V32EncoderEnableHighScaleFactor` | the non-linear encoder and its scale |
| `V32TrellisEncodeData` | the same 8 state code and point indexing as V.32bis |

Its V.32bis maps (`cCoded7200Map` to `cCoded14400Map`) are exactly
the V.32bis constellations of D-Modem multiplied by 14, point for point. This
confirms how its tables are indexed and scaled. The `+MS` of the modem
accepts `V32T`, and its list of connection types has `v.32/v.32bis/v.32terbo`.

Then the interop tests compare the result with the modem itself
(refer to the end of this page).

## The rate signals

The R and E words are the words of V.32bis. In V.32bis, bits B13 and B14 of
these words are reserved (sent as 0). V.32terbo uses them:

| Bit | Rate |
|---|---|
| B12 | 14400 (V.32bis) |
| B13 | 16800 |
| B14 | 19200 |

The two rates also must have B8 (trellis coding), as the trellis rates of V.32bis do.
The E of Agere for 19200 is `F993` (B0 first: B0 to B4, B7, B8, B11, B14,
B15). For 16800, it is `F995`. A modem that offers V.32terbo sends its R with
B13 and B14 set. A V.32bis modem ignores these bits and answers with a
V.32bis rate. Thus, the start-up does not need other changes.

## Coding and constellations

The coding is the 8 state trellis code of V.32bis, without changes. 16800 bit/s carries 7
data bits for each symbol (Q1 Q2 through the coder, Q3 to Q7 uncoded). 19200
carries 8 (Q3 to Q8 uncoded). The constellations have 256 and 512 points, indexed
as the constellations of V.32bis (Y0, then Y1 Y2, then the uncoded bits). The points are
on the usual odd-sum integer grid. For 16800, the steps are 10/14 of the grid of V.32bis
(coordinates up to 12). For 19200, the steps are half steps (up to 18). The two constellations have
about the mean power of V.32bis. In each of the 8 subsets, the distance between the points is
4 grid steps (squared distance 16), as in V.32bis.

The tables are in `dsp/qam_tables.cpp`.

## The non-linear encoder

At 16800 and 19200, the modem warps each point before it sends it. Points far
from the centre move further out, and points near the centre move in a
little. This spreads the outer points. The companding noise of the G.711 coding
of the digital phone network has the largest effect on these points. Later, V.34
standardized the same principle (its non-linear encoder, 9.7/V.34).

The fixed-point code of Agere, at its transmit scale (one grid unit of V.32bis
is 2048), does this calculation for a point Y:

    s  = ((|Y|^2 >> 14) * 20649) >> 14
    c  = (s * s) >> 15
    b  = (c * s) >> 15
    e  = (2731 s + 137 c + 3 b) >> 12
    g  = ((e * 15027) >> 17) + 15027
    Y' = (Y * g) >> 14

That is, Y' = 0.917 Y (1 + z/6 + z^2/120 + z^3/5040), with z =
0.0098 |Y|^2 in grid units of V.32bis. This is the series of sinh(sqrt z)/sqrt z.
Its decoder applies the inverse series. D-Modem calculates the warped
points one time (in `dsp/qam_tables.cpp`, with the same integer steps), and
uses them as the constellation. The transmitter sends them, and the
Viterbi decoder measures distances to them directly.

The two modems must use the same transmit scale, so that the warp is the same.
`V32ControlReset` enables `V32EncoderEnableHighScaleFactor` (a scale 1.414 times larger)
early in the start-up. When the
rate signals start, the modem disables it again. Thus, data uses the normal scale. The interop test
confirms this: D-Modem decodes the 19200 bit/s of the Agere modem with its own
warped table at a mean squared error of 0.0005 of the power of the signal.

## Receiving 512 points

The receiver of V.32bis needed two changes to accept 19200 bit/s. The two changes apply
to all the trellis rates:

- **The gain is held in data mode.** The AGC follows the power of the signal
  over about 30 ms. On a dense constellation, that estimate changes by some
  per cent. This moves the outer points by as much as the spacing
  of the 512 point constellation. Now, in data mode, the gain is frozen,
  and the equalizer follows slow changes. Back to back, the SNR of data increased
  from about 31 dB to 40 dB on a clean line.
- **B1 is used to train.** Data starts with 128 symbols of scrambled
  ones (B1). The receiver knows the register of its descrambler. Thus, it knows
  exactly what the remote sends at that time, but not the state of the trellis
  encoder of the remote. No standard sets this state (the encoder of Agere starts in state 31,
  our encoder in 0). The receiver follows all 32 states for 8 symbols, selects the state
  that agrees, and trains on the true points for the remaining part of B1. When the equalizer
  had only decisions, at 19200 it sometimes stopped at
  a gain 1 to 2 per cent incorrect. The outer points cannot accept this error.

Each modem asks for a rate that comes from the error of its training. That
estimate has a maximum near 31 dB. Thus, we set the thresholds from back to
back runs: 19200 from an estimate of 29.5 dB, 16800 from 26.5 dB. On
the line of `pump_test`, this selects 19200 from 34 dB of SNR and 16800 from
26 dB. When we force the rate, 19200 runs without errors from about 32 dB, and 16800 from 24 dB.

## Not done

- **Rate renegotiation.** The rate renegotiation of V.32bis is not
  implemented for any member of the V.32 family. D-Modem retrains instead.
- **V.8.** V.8 has no code for V.32terbo. The modem uses V.32terbo only when you select it
  with `AT+MS=V32T`.

## Tests

- `pump_test`: `v32terbo*` (clean, 27 dB, no band limit, 8000 Hz,
  300 ppm clock offset, an echo), and `V32Terbo.Rates` (19200 at 40 dB,
  16800 at 27 dB, 14400 at 25 dB, each without a bit error).
- `agere_232_agere_calls`, `agere_232_open_calls`: the Agere modem
  and D-Modem, both with `AT+MS=V32T`, call each other and connect at
  19200 bit/s in the two directions (`ctest -L agere`).
