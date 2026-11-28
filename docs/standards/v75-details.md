# V.75: technical details

This page gives the technical details for [V.75](v75.md). The clause
numbers are the clause numbers of V.75 (08/96). This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on [DSVD: technical
details](../v70-details.md).

## The primitives and the frames (6, 8)

| Function | Request | V.76 frame | Answer |
|---|---|---|---|
| Open a channel (6.1) | CE-ESTABLISH | SABME with OpenLogicalChannel | UA with OpenLogicalChannelAck, or DM with OpenLogicalChannelReject |
| Close a channel (6.2) | CE-RELEASE | DISC with CloseLogicalChannel | UA with CloseLogicalChannelAck |
| Capabilities, in-band (6.4.4.1) | CE-SETPARM | XID command with TerminalCapabilitySet | XID response with TerminalCapabilitySetAck or TerminalCapabilitySetReject |
| Capabilities, out-of-band (6.4.4.2) | CE-SETPARM | The same messages in I frames on the control DLC | |
| Data (6.5) | CE-DATA | I frames (ERM), UI frames (UNERM) | |

- The user data of each frame is the H.245 message after the format
  identifier (FI) 133 (8.1, 8.2).
- OpenLogicalChannel has the forward and the reverse parameters. The
  forward parameters have the port number and the V.76 parameters of
  the multiplexer. The V.76 parameters include the V.75 parameters
  (Table 3).
- An in-band TerminalCapabilitySet has one AlternativeCapabilitySet.
  Only the out-of-band channel can send simultaneousCapabilities.
- A terminal ignores the capabilities that it does not know (6.4.1).

## The H.245 parameters (Tables 3 to 6)

| Parameter | Content |
|---|---|
| forwardLogicalChannelNumber | The channel number |
| portNumber | The user of the channel (for example the data port or the voice port) |
| dataType | Data (DataApplicationCapability), audio (for example g729AnnexA with the blocking factor) or the DSVD control channel |
| v76LogicalChannelParameters | ERM or UNERM, the recovery (REJ or SREJ), the window k, N401, the CRC length, suspend/resume, UIH |
| v75Parameters | audioHeaderPresent |
| OpenLogicalChannelReject cause | For example unspecified, unsuitableReverseParameters, dataTypeNotSupported, dataTypeNotAvailable, unknownDataType |

## The audio header (9, Table 7)

| Bit | Meaning |
|---|---|
| 0 | 1: a silence frame |
| 1 | 1: a silence insertion descriptor (SID) |
| 2 to 6 | The sequence number of the UI frame (bit 2 is the LSB), modulo 32 |
| 7 | Reserved |

The open of the channel tells if the header is present. Audio control
information in the voice data has priority over the header.

## Break (10)

Only on ERM channels. The break of the DTE goes as a V.76 user-control
message (Annex B/V.76). The receiver gives the break to the DTE.

## Synchronous data (6.5, 11)

A UNERM channel can carry the HDLC frames of a synchronous DTE. A
header octet segments a frame that is longer than N401, and the
receiver joins the segments again.

## V.75 in D-Modem

| Item | D-Modem (`link/v76.cpp`, `link/h245.cpp`) |
|---|---|
| Open, close | SABME, UA, DM and DISC with the H.245 messages. FI 133. |
| Channels accepted | One data channel (ERM, 16-bit FCS) and one audio channel (G.729 Annex A, UNERM, 16-bit FCS). Other channels get DM with OpenLogicalChannelReject. |
| Capabilities | An XID with TerminalCapabilitySet gets TerminalCapabilitySetReject. We send no capabilities. |
| Audio header | Sent and read as Table 7. A gap of up to 3 in the sequence numbers is concealed. |
| Break | Not sent: the DTE interface has no break signal |
| Out-of-band control channel, synchronous data, segmentation | Not implemented |
| H.245 decoder | Aligned PER. The extension additions of later editions are ignored. |
