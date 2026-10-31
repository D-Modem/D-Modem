# V.32terbo: 19200 bit/s on V.32bis

**V.32terbo** is AT&T's extension of V.32bis from 1993. It keeps
everything about V.32bis (the 2400 baud signal on an 1800 Hz carrier,
the start-up, the trellis code) and adds two faster speeds, **16800**
and **19200 bit/s**, for clean lines. It came out while V.34 was still
being written. AT&T's own modems had it, and so did others built on
AT&T Microelectronics' modem chips (a business that became Lucent's and
then Agere's). V.34 soon made it obsolete.

It was never an ITU standard and its specification was never
published. D-Modem's version was rebuilt from a modem that has it:
Agere's soft modem, which D-Modem's tests already run (see
[Interop](../interop.md)). The two connect to each other at 19200 bit/s.
For how it was found and how it works, see
[the technical details](v32terbo-details.md).

(There is no "V.34terbo". The name sometimes turns up as a mix of
V.32terbo and V.34.)

## Using it

    AT+MS=V32T       (or AT+MS=232)

The modem then offers 19200 and 16800 bit/s on top of V.32bis's speeds.
Each end measures the line during the start-up and the two agree on the
fastest speed both can take; a modem that only does V.32bis just
connects at 14400 or less. V.8 (the negotiation `AT+MS=34` and up
start with) has no code for V.32terbo, so it is used only when chosen
with `AT+MS`.

## Speeds in simulation

Two D-Modems back to back on a simulated phone line (`pump_test`):

| Line's signal to noise | Speed |
|---|---|
| 34 dB and more | 19200 bit/s |
| 26 to 33 dB | 16800 bit/s |
| below 26 dB | V.32bis's speeds (14400 and down) |

Every one of these calls ran without a bit error.

Real phone lines in the 1990s seldom reached 19200 this way, which is
part of why V.34 replaced it.
