# ZyXEL 16800 and 19200

**Status: D-Modem to D-Modem only.** D-Modem has the ZyX modes
(`AT+MS=ZYX`). Three parts of the signal are not known yet (refer to
[What is not known yet](#what-is-not-known-yet)). Thus, D-Modem uses
best guesses for them. Two D-Modems connect with ZyX. A real ZyXEL modem
probably does not connect yet. This page and
[the technical details](zyxel-details.md) record what we found in the
firmware of a ZyXEL U-1496S.

## What it is

ZyXEL Communications made its own modems in the early 1990s. The U-1496
family (U-1496E, U-1496S, U-1496 Plus and others) has V.32bis and its own
modes, **ZyX 7200** to **ZyX 19200**. Later ZyXEL modems (for example, the
Elite 2864) also have these modes. Two ZyXEL modems
use them when the two ends permit them (`AT&N0`, or `AT&N34` to `AT&N39`).
The result code and the link status report show `ZyX 19200`,
`ZyX 16800` and the lower speeds.

ZyXEL did not publish the protocol. The firmware of the U-1496S (version
5.06B) gave these items:

- The rate signals are the rate signals of V.32bis. Bits B13 and B14 give
  16800 and 19200, as in [V.32terbo](v32terbo.md). The E words are the
  same (`F995`, `F993`).
- When the two modems find that the other modem is a ZyXEL, they change
  the symbol rate from 2400 baud to **2742.857 baud** (2400 x 8/7).
- **ZyX 19200** sends 7 data bits in each symbol (7 x 2742.857 = 19200),
  with the trellis code of V.32bis on 256 points.
- **ZyX 16800 to ZyX 7200** use a frame of 8 symbols. Each frame carries
  7 groups of 7, 6, 5, 4 or 3 bits (thus, 16800 to 7200 bit/s). A part of
  the bits in each frame selects one of four rings for each symbol. Inner
  rings are more frequent than outer rings. This decreases the mean power
  for the same distance between the points (shell mapping). V.34 later
  standardized the same method.
- The trellis code is the 8 state code of V.32bis, without changes.

## How to use it

    AT+MS=ZYX        (or AT+MS=234)

Use this command on the two ends. The two modems do the start-up of
V.32bis at 2400 baud until the round trip delay is measured. Then they
change to 2742.857 baud and continue with the training and the rate
signals. The two ends agree on the fastest speed that the two can use.
The result code and the call statistics show the speed (ZyX 7200 to
ZyX 19200).

D-Modem does not detect a ZyXEL peer. With `AT+MS=ZYX`, it always uses
the ZyX modes. This is the same as `AT&N34` to `AT&N39` on a ZyXEL, which
also connect only to a ZyXEL.

## Speeds in simulation

Two D-Modems back to back on a simulated telephone line (`pump_test`):

| Line's signal to noise | Speed |
|---|---|
| about 31 dB and more | ZyX 19200 |
| about 28 to 30 dB | ZyX 16800 |
| about 24 to 27 dB | ZyX 14400 |
| about 20 to 23 dB | ZyX 12000 |
| about 17 to 19 dB | ZyX 9600 |
| less | ZyX 7200 |

ZyX 16800 to ZyX 7200 need about 3 dB less signal to noise than the
speeds of V.32bis with the same number of bits in each symbol. This is
the gain of the ring table (shell mapping).

## What is not known yet

- The carrier frequency at 2742.857 baud. D-Modem uses 1800 Hz.
- The signal that each modem uses to find a ZyXEL peer. D-Modem sends
  no signal for this.
- The scrambler. D-Modem uses the scrambler of V.32bis.

The code of these parts is in the mask ROM of the DSPs of the ZyXEL
modems. It is not in the EPROMs of the U-1496S or of the Elite 2864I. A
recording of a call between two ZyXEL modems can give these parts. For
the evidence and the full list, refer to
[the technical details](zyxel-details.md).
