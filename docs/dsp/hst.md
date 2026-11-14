# HST: USRobotics' High Speed Technology

**HST** was the fast modem protocol of USRobotics. The Courier HST had it
first in 1986, and people used it until the middle of the 1990s. It is
not symmetrical. In one direction, it has a fast **forward channel** (up
to 14400 bit/s, later 16800). In the other direction, it has a slow
**back channel** of 300 bit/s. The modems turn the channels around when
the direction of the larger traffic changes. This was good for bulletin
boards and FidoNet, where most data goes in one direction. HST was the
preferred protocol of BBS sysops until V.32bis and V.34 became equally
fast. USRobotics never published it.

The HST of D-Modem comes from the known parts of the DSP firmware of the
Courier (refer to [the technical details](hst-details.md)). Its forward
and back channels are those of a real Courier, bit for bit, for the parts
of the firmware that we read. Its start-up follows a recorded call to a
real Courier and the firmware of the Courier:

- The call connects at 2400 bit/s (V.22bis).
- The two modems offer HST in the set-up of V.42.
- Then they retrain in HST.

Two D-Modems connect to each other in this mode. The recording does not
include the HST part. We compared the HST part with the DSP firmware of
the Courier, which we ran in an emulator. D-Modem connects to it as caller
and as answerer at 4800 to 16800 bit/s. The full Courier, with its
emulated firmware, calls D-Modem and D-Modem calls it. These calls use
the link layer of HST, and the channels turn around. We did not try it
with a real Courier.

## How to use it

    AT+MS=HST        (or AT+MS=168)

A call starts in the same way as a call of a Courier:

1. The modems connect at 2400 bit/s (V.22bis) and start V.42.
2. In the set-up of V.42 (XID), each modem offers HST and its speeds. It
   uses the same field as a Courier.
3. If the two modems offer HST, they skip the remaining part of V.42 at
   2400. They stop the carrier and train in HST. The speed is 9600 bit/s
   if the two modems allow it. If not, the speed is the first of 7200,
   4800, 12000, 14400 or 16800 that the two modems have. On HST, the
   modems use the link layer of HST, not V.42. This link layer has error
   correction without compression.
4. If the other modem does not offer HST, the call stays at 2400 bit/s
   with V.42, as with a Courier.

The speeds that the modem offers follow `+MS`, for example
`AT+MS=HST,0,4800,7200`. The answering modem sends the forward channel
first. The caller sends the 300 bit/s back channel. When the side on the
back channel has data to send, it asks, and the modems turn the channels
around. The fast direction follows the data. When the two sides send,
this occurs every few seconds. The rates in `CONNECT` are those of the
start. D-Modem does not change speed during the call. (A Courier
increases its speed from 9600 when the line lets it.)

With V.42 disabled (`AT\N0`), there is no set-up in which to offer HST.
Thus, the modems start HST immediately with the start-up of D-Modem. In
this start-up, the caller measures the line and asks for the fastest
speed that the line lets (up to 16800). A Courier does not understand
that start-up. V.8 has no code for HST. Thus, the modem uses HST only if
you select it with `AT+MS`.

## Speeds in simulation

Two D-Modems back to back on a simulated telephone line (`pump_test`, with
the start-up of D-Modem, which selects the speed from the line):

| Line's signal to noise | Forward channel |
|---|---|
| 40 dB | 16800 bit/s |
| 27 dB | 14400 bit/s |
| 23 dB | 12000 bit/s |
| 17 dB | 7200 bit/s |
| 12 dB | 4800 bit/s |

All of these calls had no bit errors. Calls with these conditions also
had no bit errors: a clock difference of 300 ppm, a gain change of 6 dB,
an echo, and a sample rate of 8000 Hz. Below about 12 dB, the start-up
fails.
