# What the modem does

The signal processing of the modem is open source, in `dsp/`. It is C++ code behind the C interface of the original binary-only library of Smart Link (`dsp/dsplibs.h`). Smart Link wrote its slmodemd for that library. The code builds on all architectures.

## Modulations and protocols

The modem has these functions:

- V.8 and V.34 (up to 33600 bps).
- V.8 bis before V.8 (`AT+A8E=1,1,,1`, [docs/dsp/v8bis.md](dsp/v8bis.md)). With V.8 bis, the modem also has V.70 voice and data together (`+DSVD=1`, [docs/v70.md](v70.md)). V.70 gives a data channel and a G.729 Annex A voice channel, with the silence compression of Annex B, through the multiplexer of V.76. The voice goes to the handset of a SmartUSB56 or to a `--dsvd-audio` command.
- V.32bis (14400 bps) and V.32.
- V.26 ter (2400 bps duplex, `AT+MS=V26T`) and V.26 bis (2400 bps in one direction with the 75 bps backward channel, `AT+MS=V26B`, [docs/dsp/v26.md](dsp/v26.md)).
- V.22bis, V.22 and Bell 212A, with the fast connect of point of sale terminals (`ATS49=1`).
- V.29 FastPOS (`AT+MS=V29FP`, [docs/dsp/fastpos.md](dsp/fastpos.md)). This is half duplex 9600 bit/s for card terminals.
- V.23, V.21 and Bell 103.
- V.18 text telephones. These are the 5-bit TTY mode, DTMF, EDT, Bell 103, the V.23 of Minitel, the V.21 text telephones, and the mode of V.18 between two V.18 modems. V.18 also has automoding, and 5-bit telex and RTTY with their own rates and tones.
- Contact ID alarm reports (SIA DC-05). `AT+MS=CONTACTID` answers as the receiver of a central station and sends each event as a line to the DTE. Or, it calls as an alarm panel.
- Fixed-line SMS (ETSI ES 201 912). `AT+MS=SMS` answers and sends each text message to the DTE. Or, it calls and sends the messages of the DTE.
- Class 1, 2 and 2.0 fax (V.17, V.29, V.27ter and V.21 HDLC). The built-in fax machine sends and receives these, with error correction:
  - T.85 JBIG pages.
  - T.42 JPEG colour and grey-scale pages.
  - T.43 lossless colour pages.
  - T.44 mixed raster content pages.
  - T.434 binary files.
- Call progress, caller ID and voice.

## PCM modes

The modem has V.90 only as the digital modem (on an 8000 Hz G.711 line). It has the quick connect of V.92, PCM upstream (up to 36000 bps upstream in the tests) and its rate renegotiation. It also has V.91 between two digital modems (up to 62667 bps in the two directions).

## Copper pairs, leased lines and digital channels

On a bare copper pair (not a telephone line), the modem has its own wideband mode V.wb (`AT+MS=VWB`, [docs/dsp/vwb.md](dsp/vwb.md)). V.wb is DMT at 96 kHz for the WB stick of pico-serial, which `dmodemd --usb` operates. It sends up to about 149000 bps in each direction.

On a leased line or a dry pair in the voice band, use `AT&L1` for 2-wire and `AT&L2` for 4-wire ([docs/leased.md](leased.md)). In these modes:

- All modulations operate with no dial tone or ring signal.
- V.29, V.33, V.27ter, V.26 and Bell 201, 202 and 208 operate as 4-wire leased-line modems on a continuous carrier.
- V.91 operates symmetric PCM (up to 62667 bps in the two directions) on a 4-wire digital circuit with no V.8 (`AT&L2+MS=V91`).
- The modem has the loop tests of V.54, with the self-test of V.52 (`AT&T`) and the simulated carrier of V.13 (S48).

On a clear 64 kbit/s digital channel (the data call of an ISDN B-channel), `AT+MS=CLEAR` operates at 64000 bps in the two directions with no modulation ([docs/isdn.md](isdn.md)). dmodemd receives these calls through SIP in CLEARMODE (RFC 4040). The Asterisk module receives ISDN data calls (a digital bearer, from chan_dahdi) in the same way.

For more data, refer to [docs/dsp/](dsp/README.md). For the tests, refer to [docs/testing.md](testing.md).

## The data link layer and the modem core

`link/` is the data link layer between the data pumps and the DTE ([docs/link.md](link.md)). It has these functions:

- V.42 error correction (LAPM, with its detection phase).
- MNP 2 to 4 (the alternative procedure of V.42) for modems without LAPM.
- V.44, V.42bis and MNP 5 compression (and zstd between two D-Modems).
- Plain asynchronous characters. Or, as an alternative, the synchronous access mode of V.80 for DTEs that make their own frames of data, for example H.324 terminals.

`core/` is the modem core in C++ ([docs/core.md](core.md)). It contains the AT commands, the S-registers, the result codes, the state machine of the call and its timers. It has the same behaviour as the slmodemd core of Smart Link. slmodemd, libdmodem and modem_test use it.
