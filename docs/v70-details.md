# V.70 (DSVD): technical details

The plain-English overview is [v70.md](v70.md). The references are to
ITU-T V.70 (08/96), V.75 (08/96), V.76 (08/96), V.8 bis (11/2000) and
H.245 (its ASN.1 module, which later editions extend compatibly).

## The parts

| Part | Where | What it does |
|---|---|---|
| `+DSVD`, `Modem::dsvd_offered()` | `core/at.cpp`, `core/modem.cpp` | D-Modem's own `+DSVD=<on>` (0, 1), kept with the extended parameters: with V.8 bis on (`+A8E`'s `<v8b>` 1, `v8bis_call()`), DSVD is offered and chosen. V.251 leaves the modes a modem runs V.8 bis for to "manufacturer-determined procedures"; V.70's own DTE control (7.2/V.70) is V.251's `+A8M`, which D-Modem does not have |
| `MDMPRM_V8BIS` (76) | `dsp/modem_param.h`, `core/interface.cpp` | Read by the pumps: bit 0 V.8 bis, bit 1 DSVD offered. Set by `DP_V8BIS`: what it chose, 1 data, 2 DSVD (`Modem::svd_`) |
| V.8 bis's SVD mode | `dsp/dp_v8bis.cpp` | With DSVD offered, our CL has the SVD mode beside data, with V.70, V.34, V.32 bis, V.14 and V.76 (8.3.2/V.8 bis). The caller's MS chooses SVD when both have V.70, V.76 and V.34 or V.32 bis: V.70, V.76, the faster modulation, V.14 if both have it. The answerer takes an MS with the SVD mode only if it offered DSVD and the MS has what DSVD needs, else NAK(3) and V.8 without V.8 bis. A NAK to the caller's MS undoes its choice |
| `Link::start_v76()` | `link/link.cpp` | At the pump's connect (`ST_DP_LINK`) with DSVD chosen: no detection phase, no XID and no compression; the link's HDLC frames go to the multiplexer; V.76's initiator is V.8 bis's (6.1.4/V.70), the answerer here |
| `Mux` | `link/v76.h`, `link/v76.cpp` | V.76's multiplexer and V.75's control entity for the data channel, below |
| `Lapm` as a DLC | `link/lapm.cpp` | `set_dlc()`: the DLCI and the role in the address octet; `open()`: L-ESTABLISH (a SABME with no XID before it); `set_user_data()`: V.75's messages in our SABME and DISC, or in the UA and DM answering the remote's. The rest is LAPM's data phase, as V.76's ERM with Annex C's defaults is (REJ recovery, k 15, N401 128, the 16 bit FCS) |
| `h245::encode()`, `decode()` | `link/h245.h`, `link/h245.cpp` | H.245's messages in aligned PER (X.691): OpenLogicalChannel (a data or G.729 Annex A channel, V76LogicalChannelParameters both ways), its Ack and Reject, CloseLogicalChannel and its Ack, TerminalCapabilitySet's number, its Ack and Reject. A later encoder's extension additions are passed over; a message cut short is not one |

## The data channel

| Step | Initiator (the answerer) | Responder (the caller) |
|---|---|---|
| 1 | 384 bits of flags (at least 16, clause 7/V.76) | Flags |
| 2 | SABME on DLCI 0 (the initiator's DLCIs count up from 0, Annex C), P 1, its information field V.75's: FI 133, then OpenLogicalChannel: channel 1, port 0, data (userData, V.14 buffered), maxBitRate the line's rate in 100 bit/s, V.76's parameters both ways (ERM, REJ, k 15, N401 128, CRC 16, no suspend/resume, no UIH, no audio header) | |
| 3 | | The first data channel opened, ERM both ways with a 16 bit FCS: its DLCI is the data channel's, our windows and N401 from the forward (what we receive) and reverse (what we send) parameters; UA, F 1, with OpenLogicalChannelAck; Up |
| 4 | UA: Up | |
| 5 | I and S frames on DLCI 0: LAPM's data phase, the DTE's characters | the same |
| 6 | Hanging up: DISC with CloseLogicalChannel (source user) | Hanging up: the same |
| 7 | The remote's DISC: UA with CloseLogicalChannelAck, Released | the same |

The address octet (6.1/V.76) is the DLCI in bits 8 to 3, C/R in bit 2,
EA 1: the initiator's commands and the responder's responses have C/R 1,
the responder's commands and the initiator's responses 0, as LAPM's
originator and answerer have. The data channel on DLCI 0 is therefore
what Appendix I/V.70 describes for interworking with a V.42 modem.

The remote opens a data channel first (before our flags are out): it is
taken, and the initiator opens no other.

### What is refused

| Frame | Answer |
|---|---|
| SABME opening a channel that is not data (audio, the out-of-band control channel, another DataType) | DM, F as P, with OpenLogicalChannelReject: dataTypeNotSupported |
| SABME opening a second data channel | DM with OpenLogicalChannelReject: dataTypeNotAvailable (the initiator's answer to a colliding open, 6.2.2/V.70) |
| SABME opening a data channel in UNERM, or with an 8 or 32 bit FCS | DM with OpenLogicalChannelReject: unsuitableReverseParameters |
| SABME whose user data is not an OpenLogicalChannel (no FI 133, a message not read here) | DM with OpenLogicalChannelReject: unknownDataType |
| XID command with a TerminalCapabilitySet (L-SETPARM, 8.3/V.75), on any DLC | XID response with TerminalCapabilitySetReject (unspecified), its sequence number: the defaults hold (6.2.1/V.70) |
| DISC on a DLC that is not open | DM, F as P (7.4/V.76) |
| Any other frame on a DLC that is not open, a two-octet address | Dropped |

## H.245 in aligned PER

V.75's messages are H.245's MultimediaSystemControlMessage, which H.245
encodes in X.691's aligned PER. `h245.cpp` writes and reads the subset
V.75 lists, with what the encoding of these messages needs:

| ASN.1 | Encoding |
|---|---|
| SEQUENCE | Its extension bit (0 written), a bit for each OPTIONAL root component, the components; extension additions read as open types and passed over |
| CHOICE | Its extension bit, the root alternative's index in as few bits as the root count needs (4 bits for RequestMessage's 11, 5 for ResponseMessage's 19); an extension alternative read as an open type (unknown) |
| INTEGER (lo..hi) | A bit field up to range 255 (windowSize 7 bits), one aligned octet for 256 (SequenceNumber, G.729 Annex A frames), two for up to 65536 (LogicalChannelNumber, N401, portNumber); maxBitRate (0..4294967295) its octet count in 2 bits, then the octets aligned |
| BOOLEAN, NULL | One bit, nothing |

The tests hold the encodings an ASN.1 compiler (Python's asn1tools, on
H.245's module) gives for each message.

## Tests

| Test | What it covers |
|---|---|
| `Link.H245Messages`, `Link.H245Extensions` (`link/test/link_test.cpp`, label `quick`) | Each message against the compiler's octets and read back; V.75's FI 133; an OpenLogicalChannel with an extension addition, no port, N401 256, k 7 and a 32 bit FCS; the messages cut short anywhere |
| `Link.V76`, `Link.V76Refused` | Two Links back to back: the initiator's data channel, 35000 octets both ways, again with bit errors, the responder's DISC; a responder given an audio channel, a DISC on a DLC not open, a capability set, a 32 bit FCS, then a data channel on DLCI 63 and a second one |
| `Core.DsvdBoth`, `Core.DsvdCallerAlone` (`core/test/core_test.cpp`, label `quick`) | Two modems with `AT+A8E=1,1,,1+DSVD=1`: V.8 bis chooses DSVD, V.76 with the data channel, the DTEs' characters both ways, the answerer's hang-up closing the channel; `+DSVD` at the caller alone: data and V.42 as usual |
| `Core.A8e` | `+DSVD`'s value and range |

## Not done

- The voice channel: G.729 Annex A in UNERM UI frames, V.75's audio
  header (clause 9/V.75). It is the next stage.
- More than one data channel, data channels in UNERM (V.70's synchronous
  data), V.75's segmentation (6.5/V.75) and break (clause 10/V.75),
  compression on a data channel (V.76's XID or H.245's
  v76wCompression).
- The out-of-band control channel (H.245's dsvdControl) and the
  capability exchange: our TerminalCapabilitySet is never sent, the
  remote's is turned down.
- V.76's options: SREJ, the 8 and 32 bit FCS, two-octet addresses,
  suspend/resume (Annex A), UIH frames, TEST.
- DSVD during an analogue telephone call (V.8 bis's MRd, CRd and ESi in
  the middle of a call, 6.1.3.3/V.70), the return to telephony
  (EndSessionCommand), leased lines (6.1.4/V.70).
- V.251's DTE control of V.8 bis (`+A8M`).
- A modulation limited to V.34 or V.32 bis once DSVD is chosen.
