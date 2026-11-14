# V.32terbo: 19200 bit/s on V.32bis

**V.32terbo** is the extension of V.32bis that AT&T made in 1993. It keeps
all of V.32bis (the 2400 baud signal on an 1800 Hz carrier, the start-up,
the trellis code). It adds two faster speeds, **16800** and
**19200 bit/s**, for clean lines. It came into use before the ITU
completed V.34. The modems of AT&T had it. Other modems that used the
modem chips of AT&T Microelectronics also had it (this business went to
Lucent and then to Agere). Soon after, V.34 made it obsolete.

It was never an ITU standard, and nobody published its specification.
We made the version of D-Modem again from a modem that has it. This modem
is the soft modem of Agere, which the tests of D-Modem already use (refer
to [Interop](../interop.md)). The two modems connect to each other at
19200 bit/s. For how we found it and how it operates, refer to
[the technical details](v32terbo-details.md).

(There is no "V.34terbo". Sometimes, people use this name as a mix of
V.32terbo and V.34.)

## How to use it

    AT+MS=V32T       (or AT+MS=232)

Then the modem gives 19200 and 16800 bit/s in addition to the speeds of
V.32bis. Each end measures the line during the start-up. The two ends
agree on the fastest speed that the two can use. A modem that only does
V.32bis connects at 14400 or less. V.8 is the negotiation at the start of
`AT+MS=34` and higher. V.8 has no code for V.32terbo. Thus, the modem uses
V.32terbo only if you select it with `AT+MS`.

## Speeds in simulation

Two D-Modems back to back on a simulated telephone line (`pump_test`):

| Line's signal to noise | Speed |
|---|---|
| 34 dB and more | 19200 bit/s |
| 26 to 33 dB | 16800 bit/s |
| below 26 dB | V.32bis's speeds (14400 and down) |

All of these calls had no bit errors.

In the 1990s, real telephone lines did not frequently get 19200 in this
mode. This is one of the reasons that V.34 replaced it.
