# V.FC: Rockwell's V.Fast Class

**V.FC** ("V.Fast Class") was the 28800 bit/s modem protocol of Rockwell,
from 1993 to 1995. Modems with Rockwell chips and the Couriers of
USRobotics had it, before the ITU completed V.34. It was the first
protocol to get 28800 bit/s on a telephone line. It changed the symbol
rate for the line (2400 to 3429 baud), and did not only add points. V.34
used this idea and many other parts of it. But the two protocols do not
understand each other. V.FC went out of use when V.34 modems became
common. Rockwell never published it.

The V.FC of D-Modem comes from the DSP firmware of the USRobotics Courier
V.Everything (refer to [the technical details](vfc-details.md)). Its data
mode changes the bits into points on the line. This data mode is the same
as that of the Courier: the same frames, the same constellations and the
same trellis codes. We compared it point for point with the code of the
Courier at all speeds. Its start-up is not that of V.FC now. D-Modem uses
the start-up of V.34 and then changes to the data mode of V.FC. Thus, two
D-Modems use V.FC with each other, but a real V.FC modem does not
understand the call.

## How to use it

    AT+MS=VFC        (or AT+MS=128)

1. The call starts as a V.34 call: V.8, then the line probing and
   training of V.34. These select the symbol rate.
2. In the last step of V.34 (the MP messages), each D-Modem tells that it
   wants V.FC.
3. If the two modems want V.FC, the data goes in the data mode of V.FC at
   14400 to 28800 bit/s. The speed is the fastest speed that V.34
   selected and that V.FC has at that symbol rate.
4. If the other modem does not ask for V.FC (all usual V.34 modems), the
   call continues as plain V.34.

`+MS` sets the limits of the speeds as usual, for example
`AT+MS=VFC,0,14400,19200`.

| Symbol rate | V.FC's speeds |
|---|---|
| 2400 | 14400 to 19200 bit/s |
| 2743, 2800 | 14400 to 24000 bit/s |
| 3000 | 14400 to 26400 bit/s |
| 3200, 3429 | 14400 to 28800 bit/s |

## What is missing

- The start-up of V.FC, which is necessary for a real V.FC modem. The
  firmware of the Courier has the script of the answering modem (tones
  as in V.32 and training at 2400 baud, then a rate signal). But we did
  not read the line probing and rate negotiation after it.
- The speeds below 14400 bit/s.
- Precoding and the non-linear encoder of V.FC. The Courier has them,
  but D-Modem does not use them.
