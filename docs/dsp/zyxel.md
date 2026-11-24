# ZyXEL 16800 and 19200

**Status: D-Modem to D-Modem only.** D-Modem has the ZyX modes
(`AT+MS=ZYX`). Two parts of the signal are not known (refer to
[What is not known yet](#what-is-not-known-yet)). Thus, D-Modem uses
best guesses for them. Two D-Modems connect with ZyX. A real ZyXEL modem
probably does not connect to D-Modem. This page and
[the technical details](zyxel-details.md) record what we found in the
firmware of a ZyXEL U-1496S.

## What it is

ZyXEL Communications made its own modems in the early 1990s. The U-1496
family (U-1496E, U-1496S, U-1496 Plus and others) has V.32bis and its own
modes, **ZyX 7200** to **ZyX 19200**. Later ZyXEL modems (for example, the
Elite 2864) also have these modes. Two ZyXEL modems use them when the
two ends permit them (`AT&N0`, or `AT&N34` to `AT&N39`).
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

With `AT+MS=ZYX`, D-Modem always uses the ZyX modes. This is the same as
`AT&N34` to `AT&N39` on a ZyXEL, which also connect only to a ZyXEL.

### The peer marker (S115)

With `ATS115=1`, D-Modem sends a marker in the V.32bis start-up, and a
modem that gets the marker of the other end changes to ZyX. Thus, two
D-Modems with `ATS115=1` and `AT+MS=V32B` (or `AT+MS=ZYX`) find each
other and connect with ZyX. If the other modem does not send the
marker, the call continues as V.32bis. The default is 0.

- The caller sends its marker after CC (80 ms). The answerer listens for
  it in the silence before S.
- The answerer replies with its marker (80 ms). Then it changes to ZyX.
  The caller changes to ZyX when it gets the reply.
- With `S115=0`, the modem sends no marker and keeps the V.32bis
  timing. The answerer still listens for the marker of the caller, and
  writes it in the debug log. It does not change to ZyX.

Use the same `S115` on the two ends. The marker of the caller is the
probable ZyXEL signal (refer to the technical details). The marker of
the answerer is not known: D-Modem uses its own signal. Thus, a real
ZyXEL modem does not find D-Modem, and D-Modem does not find a real ZyXEL
modem as the answerer.

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

- The signal that each modem uses to find a ZyXEL peer. D-Modem sends
  a probable signal only with `S115=1` (refer to
  [The peer marker](#the-peer-marker-s115)).
- The scrambler. D-Modem uses the scrambler of V.32bis.

The carrier at 2742.857 baud is known: 1828.571 Hz, from the ZyX DSP
program in the Elite 2864I V.90 images. The same images have the
detector of a ZyXEL peer and give a probable signal for it. They show
that the scrambler is in the serial hardware. Thus, it is probably the
V.32 scrambler that D-Modem uses. These two items are not proven. A
recording of a call between two ZyXEL modems can give these parts. For
the evidence and the full list, refer to
[the technical details](zyxel-details.md).
