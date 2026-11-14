# V.70 (DSVD): technical details

The plain-English overview is [v70.md](v70.md). The references are to
ITU-T V.70 (08/96), V.75 (08/96), V.76 (08/96), V.8 bis (11/2000),
G.729 (06/2012, Annex A) and H.245 (its ASN.1 module, which later
editions extend compatibly).

## The parts

| Part | Where | What it does |
|---|---|---|
| `+DSVD`, `Modem::dsvd_offered()` | `core/at.cpp`, `core/modem.cpp` | The `+DSVD=<on>` (0, 1) of D-Modem, kept with the extended parameters. When V.8 bis is enabled (`<v8b>` 1 of `+A8E`, `v8bis_call()`), the modem offers and selects DSVD. V.251 gives the selection of the modes for V.8 bis to "manufacturer-determined procedures". The DTE control of V.70 itself (7.2/V.70) is the `+A8M` of V.251, which D-Modem does not have |
| `MDMPRM_V8BIS` (76) | `dsp/modem_param.h`, `core/interface.cpp` | The pumps read it: bit 0 V.8 bis, bit 1 DSVD offered. `DP_V8BIS` sets it to its selection: 1 data, 2 DSVD (`Modem::svd_`) |
| V.8 bis's SVD mode | `dsp/dp_v8bis.cpp` | When the modem offers DSVD, our CL has the SVD mode together with data, with V.70, V.34, V.32 bis, V.14 and V.76 (8.3.2/V.8 bis). The MS of the caller selects SVD when the two sides have V.70, V.76 and V.34 or V.32 bis: V.70, V.76, the faster modulation, V.14 if the two sides have it. The answerer accepts an MS with the SVD mode only if it offered DSVD and the MS has the items that DSVD must have. If not, it sends NAK(3) and goes to V.8 without V.8 bis. A NAK to the MS of the caller cancels its selection |
| `Link::start_v76()` | `link/link.cpp` | At the connect of the pump (`ST_DP_LINK`) with DSVD selected: no detection phase, no XID and no compression. The HDLC frames of the link go to the multiplexer. The initiator of V.76 is the initiator of V.8 bis (6.1.4/V.70), here the answerer |
| `Mux` | `link/v76.h`, `link/v76.cpp` | The multiplexer of V.76 and the control entity of V.75 for the data channel (refer to the subsequent sections) |
| `Lapm` as a DLC | `link/lapm.cpp` | `set_dlc()`: the DLCI and the role in the address octet. `open()`: L-ESTABLISH (a SABME with no XID before it). `set_user_data()`: the messages of V.75 in our SABME and DISC, or in the UA and DM that answer the SABME and DISC of the remote. The remaining part is the data phase of LAPM, as the ERM of V.76 with the defaults of Annex C is (REJ recovery, k 15, N401 128, the 16 bit FCS) |
| `g729::Encoder`, `g729::Decoder` | `link/g729a.h`, `link/g729a_*.cpp` | G.729 Annex A (refer to the subsequent section) |
| `Owner::voice()`, `voice_state()`, `voice_in()`, `voice_out()` | `link/link.h`, `core/modem.cpp` | The voice path: `Host::dsvd_voice()` and its hooks where the host has one (the SmartUSB56 of dmodemd with a handset). If not, the `VoicePath` of `Options::dsvd` (the `--dsvd-audio` of dmodemd, `frontend/dmodemd/dsvd_audio.cpp`). With neither, there is no voice channel |
| `h245::encode()`, `decode()` | `link/h245.h`, `link/h245.cpp` | The messages of H.245 in aligned PER (X.691): OpenLogicalChannel (a data or G.729 Annex A channel, V76LogicalChannelParameters in the two directions), its Ack and Reject, CloseLogicalChannel and its Ack, the number of TerminalCapabilitySet, its Ack and Reject. The decoder ignores the extension additions of a later encoder. A message that is cut short is not a valid message |

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
and EA 1. The commands of the initiator and the responses of the responder have C/R 1.
The commands of the responder and the responses of the initiator have C/R 0, as the
originator and answerer of LAPM have. Thus, the data channel on DLCI 0 is
what Appendix I/V.70 describes for interworking with a V.42 modem.

If the remote opens a data channel first (before our flags are sent), the initiator accepts
it and does not open a different channel.

### What is refused

| Frame | Answer |
|---|---|
| SABME opening a channel that is neither data nor audio (the out-of-band control channel, another DataType) | DM, F as P, with OpenLogicalChannelReject: dataTypeNotSupported |
| SABME opening a second data channel | DM with OpenLogicalChannelReject: dataTypeNotAvailable (the initiator's answer to a colliding open, 6.2.2/V.70) |
| SABME opening a data channel in UNERM, or with an 8 or 32 bit FCS | DM with OpenLogicalChannelReject: unsuitableReverseParameters |
| SABME whose user data is not an OpenLogicalChannel (no FI 133, a message not read here) | DM with OpenLogicalChannelReject: unknownDataType |
| XID command with a TerminalCapabilitySet (L-SETPARM, 8.3/V.75), on any DLC | XID response with TerminalCapabilitySetReject (unspecified), its sequence number: the defaults hold (6.2.1/V.70) |
| DISC on a DLC that is not open | DM, F as P (7.4/V.76) |
| Any other frame on a DLC that is not open, a two-octet address | Dropped |

## The voice channel

If the initiator has a voice path, its voice channel follows the SABME of its data
channel:

| Step | Initiator | Responder |
|---|---|---|
| 1 | SABME on the next DLCI (1; 2 if the data channel has 1), P 1: OpenLogicalChannel, channel the data channel's + 1, port 1, audio (g729AnnexA, 1 frame), UNERM both ways, CRC 16, N401 11 (the header and a frame), the audio header (`v75Parameters.audioHeaderPresent`) | |
| 2 | | With a voice path: UA with OpenLogicalChannelAck, the channel open (`voice_state(true)`). Without: DM with OpenLogicalChannelReject, dataTypeNotSupported |
| 3 | UA: open (`voice_state(true)`); DM: none, the call goes on with data alone | |
| 4 | UI commands on the DLC: the audio header, then 10 octets of G.729 Annex A, or in silence a 2 octet SID frame now and then (below) | the same |
| 5 | Hanging up: DISC with CloseLogicalChannel before the data channel's | the same |
| 6 | The remote's DISC: UA with CloseLogicalChannelAck, closed (`voice_state(false)`) | the same |

If nobody answers a SABME, the modem sends it again after about one second of line bits,
four times in total. If a SABME comes again after the channel is open, the modem
answers it with the UA again.

The audio header (clause 9/V.75, Table 7) is one octet: bit 1 silence,
bit 2 a SID frame, bits 3 to 7 a sequence number that counts the
sent frames modulo 32. If there is a gap of up to 3 in the received numbers, the decoder
conceals it with its erasure (the frame erasure concealment of G.729) before
it decodes the frame.

### Silence

V.70 leaves silence compression for further study, and its OLC cannot ask for an
Annex B coder. But in V.75, the audio header can carry silence
for a coder without silence compression (clause 9/V.75). Thus, when we send the header
(always to a different D-Modem, because the two sides ask for it), our encoder runs the
VAD and DTX of G.729 Annex B (`Encoder::set_dtx(true)`):

| Encoder's frame | On the DLC |
|---|---|
| Speech | The header (silence 0, SID 0), 10 octets |
| SID (the noise has changed, Annex B's DTX) | The header (silence 1, SID 1), 2 octets: Annex B's 15 bits (MA predictor 1, LSF stages 5 and 4, gain 5) and a 0 bit |
| None (silence, no change) | Nothing; the sequence number does not count it |

When the receiver gets a frame with the silence bit, it decodes the SID (or, with
only the silence bit, an untransmitted frame). Then, while no frame
comes, it makes 10 ms of comfort noise for each 10 ms of the bits of the line
(`Mux::clock()`, half the time of a frame behind, so that a late frame does not
occur two times). This continues until a speech frame comes. The OLC still gives g729AnnexA, not
the `g729AnnexAwSilenceSuppression` of H.245, because V.75 does not list it.

The pump tells the multiplexer (`Mux::clock()`) how many bits it took.
Each 10 ms of these bits at the transmit rate (288 bits if the rate is not known)
gives the credit for one voice frame, up to a maximum of 4 saved credits. With a credit and 80 samples
from `voice_in()`, the next frame to send is a voice frame, before all frames
of the data channel. The decoded audio goes to `voice_out()`.

| Frame | Answer |
|---|---|
| An audio OpenLogicalChannel, no voice path here | DM, OpenLogicalChannelReject: dataTypeNotSupported |
| A second audio channel | DM, dataTypeNotAvailable |
| An audio channel in ERM, or with an 8 or 32 bit FCS | DM, unsuitableReverseParameters |

## G.729 Annex A

`g729a_enc.cpp` and `g729a_dec.cpp` are the coder of G.729 Annex A in 16 and
32 bit fixed point. We wrote them for D-Modem from the text of the Recommendation.
The fixed-point operations (`g729_ops.h`, the ETSI basic operators
with their overflow flag) are in the sequence in which the reference of Annex A uses them. Thus,
the bits are the same. `g729a_tab.cpp` holds the tables of Annex A.
A frame is 80 samples (10 ms) in and 11 parameters out, packed MSB first
into 10 octets, as the G729 payload of RFC 3551 has them.

`g729b.cpp` is Annex B with Annex A (the "BA" coder of the ITU reference).
It has these parts, at the two ends:

- The VAD (12 autocorrelations, with the lag window extended to them).
- DTX (when to send a SID: Itakura distance and energy change).
- The LSF and energy quantisers of the SID.
- The comfort noise excitation (Gaussian plus random pulses at the level of the SID).

The encoder
returns the type of the frame (`encode()`: 10, 2 or 0 octets). The decoder
accepts the 2 octets of a SID, or none for an untransmitted frame. When DTX is disabled
(`set_dtx(false)`, the default), the encoder is only the encoder of Annex A.

| Test | Result |
|---|---|
| Encoder: ALGTHM, FIXED, LSP, PITCH, SPEECH, TAME, TEST `.IN` against `.BIT` | Every frame the same bits |
| Decoder: all ten `.BIT` (ERASURE, OVERFLOW and PARITY among them) against `.PST` | Every sample the same |
| Annex B encoder, DTX on: `tstseq1` to `tstseq4` `.bin` against `tstseqNa.bit` | Every frame the same type and bits |
| Annex B decoder: `tstseq1a` to `tstseq4a.bit`, `tstseq5.bit`, `tstseq6.bit` (erasures in silence) against `.out` | Every sample the same |
| bcg729 (1.1), both ways, on SPEECH.IN | bcg729 itself is not bit exact with the vectors (0 of 3750 SPEECH frames agree with the bits of the ITU). Thus, the bits are different. The output of each decoder from the frames of the other coder is as close to the speech as from its own frames (about 7 dB SDR, the usual value of the coder on this file). The two decoders agree to about 34 dB on the same frames |

The vectors come from the G.729 software package of ITU-T
(`docs/specs/fetch_g729.sh`), and the `tstseq*` of Annex A and Annex B. We use only
its test vector files. CI downloads them for its Linux
build.

## H.245 in aligned PER

The messages of V.75 are the MultimediaSystemControlMessage of H.245. H.245
encodes it in the aligned PER of X.691. `h245.cpp` writes and reads the subset that
V.75 lists, with the parts of the encoding that these messages use:

| ASN.1 | Encoding |
|---|---|
| SEQUENCE | Its extension bit (0 written), a bit for each OPTIONAL root component, the components; extension additions read as open types and passed over |
| CHOICE | Its extension bit, the root alternative's index in as few bits as the root count needs (4 bits for RequestMessage's 11, 5 for ResponseMessage's 19); an extension alternative read as an open type (unknown) |
| INTEGER (lo..hi) | A bit field up to range 255 (windowSize 7 bits), one aligned octet for 256 (SequenceNumber, G.729 Annex A frames), two for up to 65536 (LogicalChannelNumber, N401, portNumber); maxBitRate (0..4294967295) its octet count in 2 bits, then the octets aligned |
| BOOLEAN, NULL | One bit, nothing |

The tests hold the encodings that an ASN.1 compiler (the asn1tools of Python, on
the module of H.245) gives for each message.

## Tests

| Test | What it covers |
|---|---|
| `Link.H245Messages`, `Link.H245Extensions` (`link/test/link_test.cpp`, label `quick`) | Each message against the compiler's octets and read back; V.75's FI 133; an OpenLogicalChannel with an extension addition, no port, N401 256, k 7 and a 32 bit FCS; the messages cut short anywhere |
| `Link.V76`, `Link.V76Refused` | Two Links back to back: the initiator's data channel, 35000 octets both ways, again with bit errors, the responder's DISC; a responder given an audio channel, a DISC on a DLC not open, a capability set, a 32 bit FCS, then a data channel on DLCI 63 and a second one |
| `Core.DsvdBoth`, `Core.DsvdCallerAlone` (`core/test/core_test.cpp`, label `quick`) | Two modems with `AT+A8E=1,1,,1+DSVD=1`: V.8 bis chooses DSVD, V.76 with the data channel, the DTEs' characters both ways, the answerer's hang-up closing the channel; `+DSVD` at the caller alone: data and V.42 as usual |
| `Core.DsvdVoice` | Both modems with a voice path: the voice channel opened after the data channel, each side's tone heard at the other (over 80% of the received signal's energy at the tone), the data alongside, the channel closed at hang-up |
| `Link.V76Voice`, `Link.V76VoiceHeader` (`link/test/link_test.cpp`) | Two Links at 28800 bit/s: two seconds of voice both ways with data alongside; the audio header's sequence numbers, an ERM audio channel and a second one refused, three lost frames concealed, the DISC |
| `Link.V76VoiceSilence` | One side quiet (low noise) for two seconds: the far end still hears 10 ms every 10 ms, comfort noise within 10 dB of the room's level and no tone, and that side's data gets over half as much again as while talking |
| `G729a.*`, `G729aBcg729.BothWays`, `DsvdAudio.*` | The coder ([testing-details.md](testing-details.md)) |
| `Core.A8e` | `+DSVD`'s value and range |

## Not done

- Silence without the audio header (a remote that does not ask for
  it gets all frames), and the g729AnnexAwSilenceSuppression of H.245.
- The voice on real SmartUSB56 hardware: we tested it only between simulated
  modems.
- A voice channel that the responder opens, or a voice channel opened after the call
  started (the initiator opens it one time, at the start).
- More than one data channel, data channels in UNERM (V.70's synchronous
  data), V.75's segmentation (6.5/V.75) and break (clause 10/V.75),
  compression on a data channel (V.76's XID or H.245's
  v76wCompression).
- The out-of-band control channel (the dsvdControl of H.245) and the
  capability exchange: the modem never sends our TerminalCapabilitySet, and it refuses the
  TerminalCapabilitySet of the remote.
- V.76's options: SREJ, the 8 and 32 bit FCS, two-octet addresses,
  suspend/resume (Annex A), UIH frames, TEST.
- DSVD during an analogue telephone call (V.8 bis's MRd, CRd and ESi in
  the middle of a call, 6.1.3.3/V.70), the return to telephony
  (EndSessionCommand), leased lines (6.1.4/V.70).
- V.251's DTE control of V.8 bis (`+A8M`).
- A modulation limited to V.34 or V.32 bis after the modems select DSVD.
