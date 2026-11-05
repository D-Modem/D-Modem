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
has been read. How a Courier starts a call and picks a speed has not
been read yet, so D-Modem has its own start-up for that, and two
D-Modems connect to each other with HST; a call to a real Courier HST is
not expected to work yet.

## Using it

    AT+MS=HST        (or AT+MS=168)

The calling modem measures the line during the answerer's training and
asks for the fastest speed the line takes: 16800, 14400, 12000, 9600,
7200 or 4800 bit/s. A top speed can be set the usual way,
`AT+MS=HST,0,4800,9600`. The answering modem always sends the forward
channel; the caller sends the 300 bit/s back channel. D-Modem does not
turn the channels around, so the calling side's sending stays at 300
bit/s for the whole call. V.42 (or MNP) runs on top as on any other
modem. V.8 has no code for HST, so it is used only when chosen with
`AT+MS`.

## Speeds in simulation

Two D-Modems back to back on a simulated phone line (`pump_test`):

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
