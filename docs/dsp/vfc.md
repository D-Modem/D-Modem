# V.FC: Rockwell's V.Fast Class

**V.FC** ("V.Fast Class") was Rockwell's 28800 bit/s modem protocol of
1993 to 1995. Modems with Rockwell chips used it, and so did the
USRobotics Courier, before the ITU completed V.34. It was the
first protocol to reach 28800 bit/s on a phone line. It changes the
symbol rate to suit the line (2400 to 3429 baud), and it does not only
add points. V.34 took that idea and much more from V.FC, but the two
protocols do not understand each other. Rockwell never published V.FC.

D-Modem's V.FC comes from the DSP firmware of the USRobotics Courier
V.Everything (see [the technical details](vfc-details.md)). It has V.FC's
own start-up and V.FC's data mode:

- The start-up is the Courier's: the same tones, messages, trainings and
  rate words, in the same order and at the same times.
- The data mode is the Courier's: the same frames, constellations,
  trellis codes and scramblers. The Courier's data points are the points
  of D-Modem's encoder, symbol for symbol.

## How to use V.FC

    AT+MS=VFC        (or AT+MS=128)

1. The answerer sends the answer tone (2100 Hz). The caller replies with
   800 Hz. There is no V.8.
2. The two modems exchange their symbol rates at 800 baud and use the
   fastest rate that both have.
3. Each modem trains the receiver of the other modem.
4. The two modems exchange the data rates that each allows, and use the
   fastest rate that both allow.

`+MS` limits the speeds as usual, for example `AT+MS=VFC,0,14400,19200`.

| Symbol rate | V.FC's speeds |
|---|---|
| 2400 | 14400 to 19200 bit/s |
| 2743, 2800 | 14400 to 24000 bit/s |
| 3000 | 14400 to 26400 bit/s |
| 3200, 3429 | 14400 to 26400 bit/s (V.FC goes to 28800) |

## What works

| Call | Result |
|---|---|
| D-Modem to D-Modem | CONNECT at 26400, data both ways. |
| D-Modem calls the Courier | The whole start-up. CONNECT at 26400. D-Modem receives the Courier's data. |
| The Courier calls D-Modem | Not yet. The Courier does not reply to D-Modem's answer tone. |

## What is missing

- 28800 bit/s. D-Modem's receiver has a noise floor of about 30 dB below
  the signal. This floor is too high for 28800, so D-Modem offers 26400
  at most.
- Data from D-Modem to the Courier. The Courier decodes the first few
  frames of D-Modem's data and then loses it. The cause is not known.
- Calls from the Courier.
- A fallback to V.34. `+MS=VFC` is V.FC only.
- The speeds below 14400 bit/s, precoding, the non-linear encoder and
  rate renegotiation.
