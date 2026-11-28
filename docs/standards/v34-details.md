# V.34: technical details

This page gives the technical details for [V.34](v34.md). The clause
numbers are the clause numbers of V.34 (02/98). This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on [V.8 and V.34: technical
details](../dsp/v34-details.md).

## The signal (5)

| Symbol rate (5.2) | Low carrier | High carrier | Required |
|---|---|---|---|
| 2400 | 1600 Hz | 1800 Hz | Yes |
| 2743 (8/7 × 2400) | 1646 Hz | 1829 Hz | No |
| 2800 (7/6 × 2400) | 1680 Hz | 1867 Hz | No |
| 3000 (5/4 × 2400) | 1800 Hz | 2000 Hz | Yes |
| 3200 (4/3 × 2400) | 1829 Hz | 1920 Hz | Yes |
| 3429 (10/7 × 2400) | 1959 Hz | 1959 Hz | No |

- The symbol rate is accurate to ± 0.01 %. The two directions can use
  different symbol rates and carriers, if the two modems have this
  capability.
- The data rates are 2400 bit/s to 33600 bit/s in steps of 2400 bit/s.
  The auxiliary channel adds 200 bit/s. Not all the rates are available
  at all the symbol rates.
- The transmitter can use one of 11 pre-emphasis filters (5.4). The
  receiver of the far modem selects it.

## The encoder (9)

- A mapping frame has 4 four-dimensional (4D) symbols, that is, 8 2D
  symbols. Shell mapping (9.4) puts the bits of a frame on rings of the
  superconstellation, so that the low-power points occur more often.
- The trellis code (9.6) is four-dimensional, with 16, 32 or 64 states.
  The receiver asks for the code.
- Precoding (9.6.2) removes the effect of the line from the receiver
  equalizer. The receiver sends the three coefficients in MP.
- Non-linear encoding (9.7) warps the points to make the signal immune
  to the companding of the network.
- The scrambler is self-synchronizing. The caller uses 1 +
  x<sup>-18</sup> + x<sup>-23</sup>, and the answerer uses 1 +
  x<sup>-5</sup> + x<sup>-23</sup>.

## The control channel (10.1)

The start-up signals of phase 2 use DPSK at 600 bit/s. The caller sends
on a 1200 Hz carrier, and the answerer sends on a 2400 Hz carrier.

| Signal | Content |
|---|---|
| Tone A | 2400 Hz, from the answerer |
| Tone B | 1200 Hz, from the caller |
| A-bar, B-bar | A phase reversal of the tone |
| INFO0a, INFO0c | 49 bits: the capabilities of the answerer and the caller |
| INFO1c | 109 bits: the caller's results of the line probe, for each symbol rate |
| INFO1a | 70 bits: the answerer's selection of the symbol rate, the carrier and the pre-emphasis |
| L1, L2 | The line probe: tones at multiples of 150 Hz. L1 for 160 ms at a higher level, then L2. |

Each INFO frame starts with four fill bits (1111) and a sync word, and
ends with a CRC and fill bits.

## Phase 2 (11.2, Figure 16)

| Step | Caller | Answerer |
|---|---|---|
| 1 | After 75 ms ± 5 ms of silence, sends INFO0c, then tone B | After 75 ms ± 5 ms of silence, sends INFO0a, then tone A |
| 2 | | When tone B comes and tone A has continued for 50 ms, sends A-bar |
| 3 | Sends B-bar 40 ms ± 1 ms after A-bar arrives. Sends tone B for 10 ms more, then silence. | Measures the round-trip delay: from A-bar to the arrival of B-bar, less 40 ms |
| 4 | Measures the round-trip delay: from B-bar to the arrival of the second A-bar, less 40 ms | Sends A-bar 40 ms ± 1 ms after B-bar arrives, 10 ms more of tone A, then L1 and L2 |
| 5 | Receives L1 (160 ms) and L2 (at most 500 ms), then sends tone B | Receives the echo of its L2, then sends tone A for 50 ms and A-bar |
| 6 | Sends B-bar 40 ms ± 1 ms after A-bar arrives, then L1 and L2 | Receives L1 and L2, then sends tone A |
| 7 | Sends INFO1c, then silence | After INFO1c, sends INFO1a |

The 40 ms are measured at the line terminals of the modem. Clause 11.2.2
gives the recovery when an INFO0 or a reversal does not arrive. For
example, a modem that hears the far tone without the far INFO0 sends its
INFO0 again.

## Phases 3 and 4 (11.3, 11.4)

| Signal | Content |
|---|---|
| S, S-bar | 128 symbols of two alternating points, then 16 symbols with the phase turned by 180°. They give a time reference. |
| PP | 288 symbols: a periodic sequence for a quick estimate of the line |
| TRN | Scrambled ones, 4 or 16 points, at least 512 symbols. The receiver trains its equalizer and the far echo canceller learns. |
| J | A 16-bit word again and again: the number of points of TRN in phase 4 |
| J' | The end of J |
| MP, MP' | 88 bits (type 0) or 188 bits (type 1, with the precoding coefficients): the data rates and the options of the modem. MP' acknowledges the far MP. |
| E | 20 ones: the end of MP. The data follows. |
| B1 | One data frame of scrambled ones at the rate of the data |

In phase 3, each modem trains its equalizer on S, PP and TRN of the far
modem. It trains its echo canceller on its own TRN. In phase 4, the
modems send TRN again and exchange MP. Each MP gives the rates that the
modem can receive. The data rate in each direction is the fastest rate
of the two MPs.

## Retrain and renegotiation (11.5, 11.6)

- A retrain starts phase 2 again with tone A or tone B.
- A rate renegotiation changes the rate without a new phase 2. The
  initiating modem turns circuit 106 OFF and sends S (128 symbols) and
  S-bar (16 symbols). Optionally, TRN follows. Then it sends MP and E.
  The far modem answers in the same way. All these signals use 4
  points.
- A cleardown (11.7) is a renegotiation where MP asks for no data rates.

## Half-duplex mode (12)

The half-duplex mode is for fax (T.30 Annex F). The source modem sends
the primary channel. The recipient modem receives it. The control
channel, at 1200 bit/s (2400 bit/s optional), is duplex. It carries the
T.30 signals between the pages.

| Item | Rule |
|---|---|
| Phase 2 (12.2) | INFO0, tones A and B with their reversals (40 ms ± 1 ms), the line probe from the source, and INFOh (51 bits) from the recipient |
| Phase 3 (12.3) | S, S-bar, PP and TRN of the primary channel from the source |
| Control channel start-up (12.4) | PPh, ALT, MPh (88 or 188 bits) until the far MPh comes, then E |
| Resynchronization (12.6) | Sh, S-bar h, ALT and E, for a control channel that stopped for a page |

## V.34 in D-Modem

| Item | D-Modem (`dsp/dp_v34.cpp`) |
|---|---|
| Symbol rates and carriers | All the rates of Table 1, with the two carriers. 3429 baud only when the caller offers it. |
| Data rates | 2400 bit/s to 33600 bit/s, with and without the auxiliary channel |
| Trellis code | 16, 32 and 64 states. We ask for 64 states. |
| Precoding, non-linear encoding, pre-emphasis | Implemented. The receiver asks for each one only when it gives a gain. |
| Phase 2 answers | 40 ms ± 1 ms on the line, at 9600 Hz and at 8000 Hz (`V34.ToneReversalDelay`) |
| INFO0 recovery | As in 11.2.2, also with V.90 and V.92 |
| Half-duplex mode | Source and recipient, with the control channel at 1200 bit/s |
| Leased lines (11.8) | Not implemented |

The rest of the implementation is on [V.8 and V.34: technical
details](../dsp/v34-details.md).

### Corrections (2026-11)

| Error | Correction |
|---|---|
| The answers to a tone reversal in phase 2 came 28 ms to 29 ms after the far reversal at 9600 Hz, and 31 ms to 50 ms at 8000 Hz (40 ms ± 1 ms required). The code used a detector delay of 13 ms (the real delay is about one symbol, 1.7 ms) and did not include the 5 ms of each sample rate converter at 8000 Hz. The round-trip estimate was 13 ms short. | `CC_RX_DELAY` is one symbol. At 8000 Hz, `cc_tx_delay()` and `cc_rx_delay()` add `RS_PATH_DELAY`. |
| In the half-duplex control channel, the receiver took the far level from the first symbol of the signal. When that symbol was much stronger than the rest, the receiver held its timing and its gain for some hundred symbols, and lost MPh. The fax call then used V.17. | The level is the mean of the first 32 symbols (`V34CC_LEVEL_N`). |

The test `V34.ToneReversalDelay` (`dsp/test/pump_test.cpp`) measures the
three answers of phase 2 on the line. `Core.Fax2PollV34` makes a V.34
fax call with the half-duplex control channel.
