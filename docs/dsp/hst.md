# HST: USRobotics' High Speed Technology

**HST** was USRobotics' own fast modem protocol, first sold in 1986 in
the Courier HST and used into the mid 1990s. It is lopsided: one way a
fast **forward channel** (up to 14400 bit/s, later 16800), the other way
a slow **back channel** of 300 bit/s, and the modems turn the channels
around when the heavier traffic changes direction. That suited bulletin
boards and FidoNet, where most data goes one way, and HST was the
favourite of BBS sysops until V.32bis and V.34 caught up. USRobotics
never published it.

D-Modem's HST is built from what is known of the Courier's DSP firmware
(see [the technical details](hst-details.md)). Its forward and back
channels are those of a real Courier, bit for bit as far as the firmware
has been read. Its start-up follows a recorded call to a real Courier
and the Courier's own firmware: the call connects at 2400 bit/s
(V.22bis), both modems offer HST in V.42's set-up, and they then
retrain in HST. Two D-Modems connect to each other this way. The HST
part, which the recording does not reach, was checked against the
Courier's DSP firmware itself, run in an emulator: D-Modem connects to
it as caller and as answerer at 4800 to 16800 bit/s. The whole Courier,
its own firmware emulated, calls D-Modem and is called by it, with
HST's own link layer and the channels turning around. It has not been
tried against a real Courier yet.

## Using it

    AT+MS=HST        (or AT+MS=168)

A call starts the way a Courier's does:

1. The modems connect at 2400 bit/s (V.22bis) and begin V.42.
2. In V.42's set-up (XID) each one offers HST and its speeds, in the
   same field a Courier uses.
3. If both offer it, they skip the rest of V.42 at 2400, drop the
   carrier and train in HST: 9600 bit/s if both allow it, else 7200,
   4800, 12000, 14400 or 16800, whichever both have first. Over HST
   the modems run HST's own link layer, not V.42: error correction
   without compression.
4. If the other modem does not offer HST, the call stays at 2400 bit/s
   with V.42, as with a Courier.

The speeds offered follow `+MS`, for example `AT+MS=HST,0,4800,7200`.
The answering modem sends the forward channel first; the caller sends
the 300 bit/s back channel. When the side on the back channel has data
to send, it asks, and the modems turn the channels around: the fast
direction follows the data, every few seconds when both sides send.
The rates `CONNECT` gives are those of the start. D-Modem does not
change speed during the call (a Courier moves up from 9600 when the
line allows).

With V.42 off (`AT\N0`), there is no set-up to offer HST in, so the
modems start HST at once with D-Modem's own start-up, in which the
caller measures the line and asks for the fastest speed it takes (up to
16800). A Courier does not understand that start-up. V.8 has no code
for HST, so it is used only when chosen with `AT+MS`.

## Speeds in simulation

Two D-Modems back to back on a simulated phone line (`pump_test`, with
D-Modem's own start-up, which picks the speed from the line):

| Line's signal to noise | Forward channel |
|---|---|
| 40 dB | 16800 bit/s |
| 27 dB | 14400 bit/s |
| 23 dB | 12000 bit/s |
| 17 dB | 7200 bit/s |
| 12 dB | 4800 bit/s |

Every one of these calls ran without a bit error, as did calls with a
300 ppm clock difference, a 6 dB gain change, an echo, and an 8000 Hz
sample rate. Below about 12 dB the start-up fails.
