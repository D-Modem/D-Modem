# V.32terbo: technical details

AT&T did not publish the specification of V.32terbo. Thus, this page
records the sources of the V.32terbo of D-Modem (in `dsp/dp_v32.cpp`). For an
overview, refer to [V.32terbo](v32terbo.md).

## Where it comes from

Public sources give little information other than the speeds: AT&T,
1993, 16800 and 19200 bit/s, 2400 baud, an 1800 Hz carrier, trellis
coding. We found no patent, datasheet, Usenet post or open-source modem
that gives the rate bits, the constellations or the coding.

The Agere soft modem in the interop tests (`dsp/test/agere/agrsm_core.o`,
Agere SoftModem 2.1.40) has all of this information. Agere came from AT&T
Microelectronics through Lucent. Thus, its V.32 code comes from the
V.32terbo of AT&T.
The object file is not stripped, and its symbols give the name of each
part:

| Symbol | What it gave |
|---|---|
| `V32ControlDetermineRate`, `V32ControlDefineE`, `V32ControlRefineR` | the rate bits of R and E, and the rate indexes 12 (16800) and 13 (19200) |
| `V32EncoderSetRate` (its two jump tables) | 7 and 8 data bits for each symbol, the maps `cCoded16800Map` and `cCoded19200Map`, and the fact that only these two rates go through `V32NonlinearEncoder` |
| `cCoded16800Map`, `cCoded19200Map` | the 256 and 512 points, as signed bytes |
| `V32NonlinearEncoder`, `V32NonlinearDecoder`, `V32EncoderEnableHighScaleFactor` | the non-linear encoder and its scale |
| `V32TrellisEncodeData` | the same 8 state code and point indexing as V.32bis |

Its V.32bis maps (`cCoded7200Map` to `cCoded14400Map`) are exactly the
V.32bis constellations of D-Modem multiplied by 14, point for point. This
confirms the indexes and the scale of its tables. The `+MS` of the modem
accepts `V32T`, and its list of connection types has `v.32/v.32bis/v.32terbo`.

Then the interop tests compare the result with the modem itself
(refer to the end of this page).

## The rate signals

The R and E words are the words of V.32bis. In V.32bis, bits B13 and B14
of these words are reserved (sent as 0). V.32terbo uses them:

| Bit | Rate |
|---|---|
| B12 | 14400 (V.32bis) |
| B13 | 16800 |
| B14 | 19200 |

The two rates also must have B8 (trellis coding), as the trellis rates
of V.32bis do. The E of Agere for 19200 is `F993` (B0 first: B0 to B4,
B7, B8, B11, B14, B15). For 16800, it is `F995`. A modem that offers
V.32terbo sends its R with B13 and B14 set. A V.32bis modem ignores
these bits and answers with a V.32bis rate. Thus, the start-up needs no
other changes.

## Coding and constellations

The coding is the 8 state trellis code of V.32bis, without changes.
16800 bit/s carries 7 data bits for each symbol (Q1 Q2 through the coder,
Q3 to Q7 uncoded). 19200 carries 8 (Q3 to Q8 uncoded). The
constellations have 256 and 512 points. Their indexes are as in the
constellations of V.32bis (Y0, then Y1 Y2, then the uncoded bits).

The points are on the usual odd-sum integer grid. For 16800, the steps
are 10/14 of the grid of V.32bis (coordinates up to 12). For 19200, the
steps are half steps (up to 18). The two constellations have about the
mean power of V.32bis. In each of the 8 subsets, the distance between
the points is 4 grid steps (squared distance 16), as in V.32bis.

The tables are in `dsp/qam_tables.cpp`.

## The non-linear encoder

At 16800 and 19200, the modem warps each point before it sends it.
Points far from the centre move farther out, and points near the centre
move in a little. This spreads the outer points. The companding noise of
the G.711 coding of the digital telephone network has the largest effect
on these points. Later, V.34 standardized the same principle (its
non-linear encoder, 9.7/V.34).

The fixed-point code of Agere, at its transmit scale (one grid unit of
V.32bis is 2048), does this calculation for a point Y:

    s  = ((|Y|^2 >> 14) * 20649) >> 14
    c  = (s * s) >> 15
    b  = (c * s) >> 15
    e  = (2731 s + 137 c + 3 b) >> 12
    g  = ((e * 15027) >> 17) + 15027
    Y' = (Y * g) >> 14

That is, Y' = 0.917 Y (1 + z/6 + z^2/120 + z^3/5040), with z =
0.0098 |Y|^2 in grid units of V.32bis. This is the series of
sinh(sqrt z)/sqrt z. Its decoder applies the inverse series. D-Modem
calculates the warped points one time (in `dsp/qam_tables.cpp`, with the
same integer steps), and uses them as the constellation. The transmitter sends them, and the
Viterbi decoder measures distances to them directly.

The two modems must use the same transmit scale, so that the warp is the
same. `V32ControlReset` enables `V32EncoderEnableHighScaleFactor` (a
scale 1.414 times larger) early in the start-up. When the rate signals
start, the modem disables it again. Thus, data uses the normal scale. The
interop test confirms this. D-Modem decodes the 19200 bit/s of the Agere
modem with its own warped table at a mean squared error of 0.0005 of the
power of the signal.

## The receiver at 19200 bit/s

To accept 19200 bit/s, the receiver of V.32bis has two more functions.
The two functions apply to all the trellis rates:

- **The gain is fixed in data mode.** The AGC follows the power of the
  signal over about 30 ms. On a dense constellation, that estimate
  changes by some per cent. This moves the outer points by as much as
  the distance between the points of the 512 point constellation. Thus,
  in data mode, the gain is fixed, and the equalizer follows slow
  changes. Back to back on a clean line, this increased the SNR of data
  from about 31 dB to 40 dB.
- **The receiver trains on B1.** Data starts with 128 symbols of
  scrambled ones (B1). The receiver knows the register of its
  descrambler. Thus, it knows exactly what the remote sends at that time,
  but not the state of the trellis encoder of the remote. No standard
  sets this state (the encoder of Agere starts in state 31, our encoder
  in 0). The receiver follows all 32 states for 8 symbols and selects the
  state that agrees. Then it trains on the true points for the remaining
  part of B1. With decisions only, the equalizer at 19200 sometimes
  stopped at a gain that was 1 to 2 per cent incorrect. The outer points
  cannot accept this error.

Each modem asks for a rate that comes from the error of its training. That
estimate has a maximum near 31 dB. Thus, we set the thresholds from back to
back runs: 19200 from an estimate of 29.5 dB, 16800 from 26.5 dB
(`v32_min_snr` in `dsp/dp_v32.cpp`). On the line of `pump_test`, this
selects 19200 from 34 dB of SNR and 16800 from 26 dB. When we force the
rate, 19200 runs without errors from about 32 dB, and 16800 from 24 dB.

## Rate renegotiation

V.32bis and V.32terbo can change their rate during data without a
retrain (8/V.32bis). The equalizers and the echo canceller keep the
values that they learned. Thus, a renegotiation takes about 150 ms, but a
retrain takes about three seconds.

1. The modem that starts the renegotiation (the initiator) sends a
   preamble. The caller sends AA for 56 symbols and CC for 8 symbols. The
   answerer sends AC for 56 symbols and CA for 8 symbols. Then it sends
   R4: the rates that it can use now. The scrambler starts again from
   zero for R4.
2. The other modem (the responder) finds the preamble in its data: 24
   symbols in a row on one training point (AA), or on two opposite points
   in turn (AC). It continues to send data until it receives R4. Then it
   sends its own preamble and R5 for 64 symbols. R5 gives the rates that
   its receiver can use now, whatever R4 contains.
3. Each modem sends E with the fastest rate of R4 and R5, then 24
   symbols of B1, then data. The initiator sends E after R5 arrives,
   when it has sent R4 for 64 symbols at least.

If the two modems start at the same time, each modem uses the R4 of the
other modem as an R5. Then the two modems use the fastest rate that the
two words have in common.

A preamble is shorter than a retrain. A modem answers a tone of AA or AC
as a retrain only after 128 symbols (5.5/V.32).

Before 2026-11, the pump used S, S-bar and a short TRN as its preamble,
because we did not have the text of V.32bis then. Agere, Broadcom, ESS
and the Courier answered that signal with a retrain. If a modem does not
renegotiate, its AA or AC continues for more than 128 symbols, and the
pump answers that retrain. If neither shows, the pump retrains after 3 s.

These items start a renegotiation:

- `ATO3`.
- `<EM><rrn>` of V.80 (`MDMPRM_LINK_REQ` 2).
- The rate shifts of MNP 10 in V.32bis and V.32terbo, to a higher or a
  lower rate. The new rate is the maximum.

Plain V.32 has no renegotiation. Thus, in V.32, `ATO3` starts a retrain.
`ATI11` counts each renegotiation as a renegotiation of the local modem
or of the remote modem.

Back-to-back tests showed one detail: **the modem clears the tone
trackers after a renegotiation.** The preamble starts the 1800 Hz
tracker or the 600 Hz tracker. The spectrum of the data near 1800 Hz then
goes above the threshold frequently, and this keeps the tracker on.

The rate words are the words of the start-up. [V.32 bis](../standards/v32bis-details.md)
has the details of the procedure.

## Not done

- **V.8.** V.8 has no code for V.32terbo. The modem uses V.32terbo only
  when you select it with `AT+MS=V32T`.

## Tests

- `pump_test`: `v32terbo*` (clean, 27 dB, no band limit, 8000 Hz,
  300 ppm clock offset, an echo), and `V32Terbo.Rates` (19200 at 40 dB,
  16800 at 29 dB, 14400 at 25 dB, each without a bit error).
- `V32bis.Renegotiation` of `pump_test`: a renegotiation of the caller
  and then of the answerer, to 9600 bit/s
  (`DSP_V32_RENEG=<ms>[:<bit/s>][a|c]`). The other modem answers without
  a retrain. All the errors are in the one burst of the change.
- `v32_reneg_both`, `v32_reneg_a`, `v32_reneg_c` of `dmodem_test`: the
  same tests through the full modem, with V.42 data.
- `Core.Mnp10Climbs` of `core_test`: MNP 10 increases the rate from
  V.32bis 4800 to 14400, and from V.32terbo 16800 to 19200, with
  renegotiations.
- `<peer>_132_dte_reneg_<peer>_calls` (Agere, Broadcom, ESS) and
  `courier_v32_dte_reneg`: our `ATO3` in V.32bis against a modem that
  answers with a retrain. All the data arrives.
- `agere_232_agere_calls`, `agere_232_open_calls`: the Agere modem
  and D-Modem, both with `AT+MS=V32T`, call each other and connect at
  19200 bit/s in the two directions (`ctest -L agere`).
