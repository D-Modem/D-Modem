# ZyXEL 16800 and 19200

**Status: write-up only.** D-Modem does not have this protocol yet. This
page and [the technical details](zyxel-details.md) record what we found
in the firmware of a ZyXEL U-1496S. We will use them to make the
protocol.

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

## What is not known yet

- The carrier frequency at 2742.857 baud.
- The signal that each modem uses to find a ZyXEL peer.
- The scrambler (we think that it is the scrambler of V.32bis).

For the evidence and the full list, refer to
[the technical details](zyxel-details.md).
