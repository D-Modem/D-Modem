# V.FC: Rockwell's V.Fast Class

**V.FC** ("V.Fast Class") was the 28800 bit/s modem protocol of Rockwell,
from 1993 to 1995. Modems with Rockwell chips used it, and the USRobotics
Courier also used it, before the ITU completed V.34. It was the first
protocol to get 28800 bit/s on a telephone line. It changes the symbol
rate for the line (2400 to 3429 baud), and does not only add points. V.34
used this idea and many other parts of V.FC. But the two protocols do not
understand each other. Rockwell never published V.FC.

The V.FC of D-Modem comes from the DSP firmware of the USRobotics Courier
V.Everything (refer to [the technical details](vfc-details.md)). It has the
start-up of V.FC and the data mode of V.FC:

- The start-up is the same as that of the Courier: the same tones,
  messages, trainings and rate words, in the same sequence and at the
  same times.
- The data mode is the same as that of the Courier: the same frames,
  constellations, trellis codes and scramblers. The data points of the
  Courier are the points of the encoder of D-Modem, symbol for symbol.

## How to use V.FC

    AT+MS=VFC        (or AT+MS=128)

1. The answerer sends the answer tone (2100 Hz). The caller replies with
   800 Hz. There is no V.8.
2. The two modems exchange their symbol rates at 800 baud. They use the
   fastest rate that the two modems have.
3. Each modem trains the receiver of the other modem.
4. The two modems exchange the data rates that each modem accepts. They
   use the fastest rate that the two modems accept.

`+MS` sets the limits of the speeds as usual, for example
`AT+MS=VFC,0,14400,19200`.

| Symbol rate | V.FC's speeds |
|---|---|
| 2400 | 14400 to 19200 bit/s |
| 2743, 2800 | 14400 to 24000 bit/s |
| 3000 | 14400 to 26400 bit/s |
| 3200, 3429 | 14400 to 26400 bit/s (V.FC goes to 28800) |

## What works

| Call | Result |
|---|---|
| D-Modem to D-Modem | CONNECT at 26400, data in the two directions. |
| D-Modem calls the Courier | The full start-up. CONNECT at 26400. D-Modem receives the data of the Courier. |
| The Courier calls D-Modem | Not yet. The Courier does not reply to the answer tone of D-Modem. |

## What is missing

- 28800 bit/s. The receiver of D-Modem has a noise floor of about 30 dB
  below the signal. This floor is too high for 28800. Thus, D-Modem
  offers a maximum of 26400.
- Data from D-Modem to the Courier. The Courier decodes the first frames
  of the data of D-Modem, and then it loses the data. We do not know the
  cause.
- Calls from the Courier.
- A fallback to V.34. `+MS=VFC` is only V.FC.
- The speeds below 14400 bit/s, precoding, the non-linear encoder and
  rate renegotiation.
