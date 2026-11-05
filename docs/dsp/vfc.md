# V.FC: Rockwell's V.Fast Class

**V.FC** ("V.Fast Class") was Rockwell's 28800 bit/s modem protocol of
1993 to 1995, sold in modems with Rockwell chips and in USRobotics'
Couriers while the ITU was still writing V.34. It was the first to reach
28800 bit/s over a phone line, by changing the symbol rate to suit the
line (2400 to 3429 baud) instead of only adding points. V.34 took that
idea and much else from it, but the two do not understand each other,
and V.FC disappeared once V.34 modems were common. Rockwell never
published it.

D-Modem's V.FC is built from the USRobotics Courier V.Everything's DSP
firmware (see [the technical details](vfc-details.md)). Its data mode,
which is how the bits become points on the line, is the Courier's: the
same frames, the same constellations and the same trellis codes, checked
point for point against the Courier's own code at every speed. Its
start-up is not V.FC's yet. D-Modem uses V.34's start-up and then
switches to V.FC's data mode, so two D-Modems talk V.FC to each other,
but a real V.FC modem does not understand the call.

## Using it

    AT+MS=VFC        (or AT+MS=128)

1. The call starts like a V.34 call: V.8, then V.34's line probing and
   training, which choose the symbol rate.
2. In V.34's last step (the MP messages) each D-Modem says it wants V.FC.
3. If both do, the data goes in V.FC's data mode at 14400 to 28800
   bit/s, the fastest V.34 chose that V.FC has at that symbol rate.
4. If the other modem does not ask for V.FC (any ordinary V.34 modem),
   the call goes on as plain V.34.

`+MS` limits the speeds as usual, for example `AT+MS=VFC,0,14400,19200`.

| Symbol rate | V.FC's speeds |
|---|---|
| 2400 | 14400 to 19200 bit/s |
| 2743, 2800 | 14400 to 24000 bit/s |
| 3000 | 14400 to 26400 bit/s |
| 3200, 3429 | 14400 to 28800 bit/s |

## What is missing

- V.FC's own start-up, which a real V.FC modem needs. The Courier's
  firmware has the answering modem's script (V.32-like tones and
  training at 2400 baud, then a rate signal), but the line probing and
  rate negotiation after it are not read yet.
- The speeds below 14400 bit/s.
- Precoding and V.FC's non-linear encoder, which the Courier has and
  D-Modem does not use.
