# H.245: technical details

This page gives the technical details for [H.245](h245.md). Only the
messages that V.75 uses are on this page. This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on [DSVD: technical
details](../v70-details.md#h245-in-aligned-per).

## The message types

| Type | Messages that V.75 uses |
|---|---|
| RequestMessage | TerminalCapabilitySet, OpenLogicalChannel, CloseLogicalChannel |
| ResponseMessage | TerminalCapabilitySetAck, TerminalCapabilitySetReject, OpenLogicalChannelAck, OpenLogicalChannelReject, CloseLogicalChannelAck |

## OpenLogicalChannel

| Component | Content |
|---|---|
| forwardLogicalChannelNumber | 1 to 65535 |
| forwardLogicalChannelParameters | portNumber (optional), dataType, multiplexParameters (v76LogicalChannelParameters) |
| reverseLogicalChannelParameters | The parameters of the other direction (optional) |

| v76LogicalChannelParameters | Content |
|---|---|
| hdlcParameters | crcLength (8, 16 or 32 bits), n401, loopbackTestProcedure |
| suspendResume | noSuspendResume, suspendResumewAddress or suspendResumewoAddress |
| uIH | UIH frames instead of UI frames |
| mode | eRM (with windowSize and recovery: rej, sREJ or mSREJ) or uNERM |
| v75Parameters | audioHeaderPresent |

## The aligned PER (X.691)

| ASN.1 | Encoding |
|---|---|
| SEQUENCE | An extension bit if the type is extensible. One bit for each OPTIONAL component of the root. Then the components. |
| CHOICE | An extension bit, then the index of the alternative in the fewest bits that the root needs |
| INTEGER with a range | The value minus the lower bound, in the fewest bits. A range of more than 255 values is aligned on an octet. |
| BOOLEAN | One bit |
| NULL | No bits |
| Extension additions | A length and the encoding of each addition (an open type). A decoder that does not know them skips them. |

## OpenLogicalChannelReject causes

| Cause | Meaning |
|---|---|
| unspecified | No reason given |
| unsuitableReverseParameters | The reverse parameters are not acceptable |
| dataTypeNotSupported | The terminal cannot use this data type |
| dataTypeNotAvailable | The data type is not available now |
| unknownDataType | The data type is not known |
| dataTypeALCombinationNotSupported | The data type and the adaptation layer cannot go together |

## H.245 in D-Modem

| Item | D-Modem (`link/h245.cpp`) |
|---|---|
| Messages | The messages of the table above. Other messages are not known, and the modem refuses or ignores them. |
| Data types | data (V.14 buffered, the userData of DataApplicationCapability) and audio (g729AnnexA with the blocking factor) |
| TerminalCapabilitySet | Only its sequence number is read |
| Extension additions | Skipped, so that a later edition decodes |
| Messages cut short | Not valid |
| Tests | `Link.H245Messages` and `Link.H245Extensions` compare each message with the octets of an ASN.1 compiler |
