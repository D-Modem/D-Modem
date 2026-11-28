# V.70: technical details

This page gives the technical details for [V.70](v70.md). The clause
numbers are the clause numbers of V.70 (08/96). This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on [DSVD: technical
details](../v70-details.md).

## The parts of a DSVD terminal (5)

| Part | Function |
|---|---|
| Supervisory and control function (SCF, 5.2) | Asks the control entity to open and close DLCs, and to exchange the capabilities. It resolves opens that cross. |
| Data processing (5.3) | The data of the DTE. Asynchronous data uses ERM or UNERM. Synchronous data uses UNERM. |
| Voice processing (5.4) | G.729 Annex A. A blocking factor of 1: one 10 ms block in each frame. |
| Control entity (5.5) | V.75 |
| Multiplexer (5.6) | V.76, with all the default values of Annex C/V.76 |
| Modem (5.7) | V.32 bis or V.34. V.8 bis selects the mode. |

## The mandatory features (5.8)

| Feature | Reference |
|---|---|
| G.729 Annex A | 5.4 |
| V.8 bis for the mode and the capabilities | 6.1 |
| The roles of initiator and of responder | 6.1.4 |
| At least one voice DLC and one data DLC | 5.3, 5.4 |
| Opening and closing of DLCs | 6.2 |
| ERM and UNERM | 5.6 |
| The default parameters of V.76 | 5.6 |

These features are optional:

- More than one voice channel or data channel, and an out-of-band
  control channel.
- An 8-bit or 32-bit FCS, and the audio header.
- A blocking factor of more than 1.
- Segmentation of UNERM data, and suspend/resume.

## The procedures (6)

1. **The start (6.1).** The call starts in analogue telephony mode, or
   goes to DSVD mode at once. V.8 bis selects DSVD. Then the modems
   train.
2. **The roles (6.1.4).** The V.8 bis initiator is the initiator of V.75
   and V.76. On a leased line, the call modem is the initiator.
3. **Capabilities (6.2.1).** This is optional. The terminal can send an
   H.245 TerminalCapabilitySet in XID (in-band), or on a control DLC
   (out-of-band). If there is no exchange, the default values apply.
4. **Opening a DLC (6.2.2).** The SCF sends CE-ESTABLISH with the
   parameters. If there was no capability exchange, it can try
   non-default values first, then the default values.
5. **Opens that cross (6.2.2).** If the SCF receives an open of the same
   H.245 data type before its own open is confirmed, the opens crossed.
   The initiator refuses the open of the responder. Thus the open of the
   initiator is the one that stays.
6. **Closing a DLC (6.2.2).** The SCF sends CE-RELEASE.

## V.70 in D-Modem

| Item | D-Modem |
|---|---|
| Modulations | V.34 and V.32 bis. V.8 bis offers DSVD when `+DSVD=1` and `+A8E` enables V.8 bis. |
| Data channel | One, ERM, opened by the initiator on DLCI 0 |
| Voice channel | One, G.729 Annex A, UNERM, opened by the initiator on DLCI 1 when the host has a voice path. The responder opens it on DLCI 63 if the initiator opens none in 1 s. |
| Audio header, Annex B | Offered by the initiator. Silence compression only with the audio header. |
| Capabilities | No exchange. A TerminalCapabilitySet gets TerminalCapabilitySetReject, and the defaults apply. |
| Out-of-band control channel, suspend/resume, synchronous data | Not implemented |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| Voice opens that cross (6.2.2) | The responder refused the open of the initiator, and the initiator refused the open of the responder. The call had no voice channel. | The responder gives up its own open and accepts the open of the initiator |
