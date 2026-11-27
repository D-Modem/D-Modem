# V.8 bis: mode selection before V.8

This page describes ITU-T Recommendation V.8 bis (11/2000), "Procedures
for the identification and selection of common modes of operation
between data circuit-terminating equipments (DCEs) and between data
terminal equipments (DTEs) over the public switched telephone network
and on leased point-to-point telephone-type circuits". It is our own
description. It is not a copy of the Recommendation. For the normative
text, refer to the Recommendation (`docs/specs/fetch.sh V.8bis`). For
the signals, the messages and the codes, refer to
[the technical details](v8bis-details.md).

## Summary

V.8 bis lets two stations agree on the type of call before a modem
starts. A station is a modem with its terminal. The two stations
can agree on these types of call, and others:

- Data.
- Simultaneous voice and data (V.61, V.70).
- A multimedia terminal (H.324).
- A text telephone (V.18).
- Fax (T.30).
- A usual telephone call (telephony mode).

V.8 bis can operate at the start of a call, or later, during a
telephone conversation. Its signals are short and quiet. Thus, a person
who talks on the line is not disturbed. A station that does not know
V.8 bis ignores the signals.

## Signals and messages

V.8 bis has two types of transmission:

- **Signals.** A signal is a pair of tones, then a single tone. A
  station can detect a signal while a person talks. The signals start a
  transaction: MR (mode request), CR (capabilities request) and ES
  (escape).
- **Messages.** A message is an HDLC frame at 300 bit/s on a V.21
  channel. The messages carry the information: MS (mode select), CL
  (capabilities list), CLR (capabilities list request), ACK and NAK.

## A typical transaction

This example is a call that an automatic station answers:

1. The answer station waits 400 ms. Then it sends MRe or CRe, a quiet
   signal.
2. The call station replies with a signal (for example, CRd).
3. The answer station sends CL, the list of its capabilities.
4. The call station selects a mode. It sends MS.
5. The answer station sends ACK(1).
6. If the mode is a modem mode, the station that received MS becomes the
   answer modem. The modems start with V.8, a short V.8 or V.25.

If the call station does not reply, the answer station sends the usual
answer tone after about 3 seconds.

## Related standards

- [V.8](v8.md): the start-up after V.8 bis.
- [V.21](v21.md): the modulation of the messages.
- [V.25](v25.md): the answer tone.
- V.251: the AT commands that control V.8 bis (`+A8E` and others).
- V.61 and V.70: simultaneous voice and data. They start with V.8 bis.

## V.8 bis in D-Modem

`dsp/v8bis.cpp` codes the signals and the messages. `dsp/dp_v8bis.cpp`
does the transactions. To enable V.8 bis, send `AT+A8E=1,1,,1` to the two
modems. For more information, refer to [V.8bis](../dsp/v8bis.md).
