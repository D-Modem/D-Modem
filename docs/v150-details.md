# Modem relay (V.150.1): technical details

The plain-English overview is [v150.md](v150.md).

dmodemd on SIP is a V.150.1 modem-over-IP endpoint. It is the modem and
its gateway in one unit (U-MR, `mr=1`). The far end is a V.150.1 gateway
that terminates the modulation of its own modem, or another endpoint of
this type. All of the references on this page are to ITU-T V.150.1
(01/2003).

## The parts

| Part | Where | What it does |
|---|---|---|
| `DP_RELAY` (151) | `dsp/dp_relay.cpp` | The relay pump. It waits until the relay starts, and `CONNECT`s at its rates. Then it moves octets between the host and the asynchronous link of the core as start-stop characters (8N1), in the limit of the rate in each direction, and sends silence. If no relay starts in S7, or if the relay stops, the result is `DPSTAT_ERROR` (`NO CARRIER`). It has no `AT+MS` name. The core selects it |
| `struct dp_relay`, `MDMPRM_RELAY` (75) | `dsp/modem_defs.h`, `core/interface.cpp` | The interface of the pump to the host: `state()` (0 not started, 1 started with the rates, -1 stopped), `read()`, `write()` |
| `Host::relay_line()`, `relay_state()`, `relay_read()`, `relay_write()` | `core/dcore.h` | The host tells that a call is relayed. `Modem::relay_channel()` is true for a relayed call at 8000 Hz in data mode (`+FCLASS=0`). Then `change_dp()` starts `DP_RELAY` in place of the pump of `+MS`. When the modem answers, it starts immediately (the answer delay is short, as on a clear channel). When the modem calls, it starts after call progress hears the answer tone. `DP_RELAY` is in the list of pumps without the V.42 and MNP of the core. The link of the far gateway does the error correction |
| `dmd::v150::Sse`, `Sprt`, `Session` | `frontend/dmodemd/v150.h`, `v150.cpp` | The SSE coding of Annex C, the SPRT of Annex B, and the MoIP messages of clause 15.4 as a session, as plain data (the clock of the caller, packets as octets) |
| `Relay`, `Media` | `frontend/dmodemd/sip.cpp` | The session and its SPRT socket. The audio thread (the SSEs, the packets of SPRT each 20 ms, ANSam as events) and the modem thread (`sip::relay_state()` and the related functions, through `SipLine`) share them |

## The call set-up (Annex E)

With `SIP_V150=yes`, the offer of a data call has these lines, together
with the G.711 audio. This does not apply to a call for `AT+MS=CLEAR` or
to a call in fax mode:

    m=audio <port> RTP/AVP 0 8 101 118
    a=rtpmap:101 telephone-event/8000
    a=fmtp:101 0-16,32-35
    a=rtpmap:118 v150fw/8000
    m=audio <port> udpsprt 120
    a=sprtmap:120 v150mr/8000
    a=fmtp:120 mr=1;mg=2;CDSCselect=1;mrmods=1,3-4,12;jmdelay=no

The SSE format (`v150fw`) goes on the audio line. The modem relay line
has its own UDP socket. If an answer has the `udpsprt` line enabled (port
not 0) and `v150fw` on its audio, the relay starts. If an answer does not
have them, the call is a usual audio call. From then on, our `udpsprt`
line is disabled in the SDP of the call.

If an offer has the two lines, dmodemd answers with the two lines (with
the payload types of the far end). This does not occur if `SIP_V150` is
not `yes` or if the modem is in fax mode. Then the call is relayed when
the pump of the modem asks (`sip::relay()`). The `mg` of the far end
comes from its `a=fmtp`. dmodemd does not send or read `sprtparm`. Thus,
the two ends use the defaults of Table B.2.

`line_of()` skips `udpsprt` lines when it looks for the audio.
`take_relay()` makes the `Relay` one time for each call (a re-INVITE
keeps it).

## Call discrimination (clause 20, Figure 21)

D-Modem uses the audio procedure (`CDSCselect=1`): the answer tone as
RFC 4733 events (20.4.3).

| | Caller (G1) | Answerer (G2) |
|---|---|---|
| 1 | Call progress hears the answer tone and gives control to `DP_RELAY`. The answer tone is the RFC 4733 event 34 or 32 of the far end, which `EventPlayer` plays, or the tone in the audio | `DP_RELAY` starts at `ATA` (or S0) and asks for the state of the relay. This starts the session. ANSam goes as RFC 4733 event 34 (volume -12 dBm0) each 20 ms in place of the audio. Its duration increases, and a new event starts before 60000 samples |
| 2 | `SSE:M`, reason CM, with the bits of the information Table 13 for V.34, V.32/V.32bis, V.22/V.22bis and V.21 (`0x5808`) | |
| 3 | | On `SSE:M`: ANSam stops (three end packets), `SSE:M` goes back with reason p' (19), and modem relay starts |
| 4 | On `SSE:M` (or the first SPRT packet): modem relay starts | |
| 5 | `INIT` on TC2 (no optional data types) | `INIT` on TC2 |
| 6 | | On the `INIT` of the caller: `JM_INFO` (call function data `0x8600`, the selected modulations `0xA000 \| mods >> 3`, LAPM `0x5800`) and `CONNECT` with the best modulation that the two ends have (V.34 33600, V.32bis 14400, V.22bis 2400, V.21 300), EC V.42/LAPM, no compression |
| 7 | On the `CONNECT` of the answerer: `CONNECT` back, with its SELMOD and its rates in the opposite direction. The relay is started | On the `CONNECT` of the caller: the relay is started |

The endpoint sends each SSE three times, 20 ms apart, with the same RTP
timestamp (C.3.1). While modem relay operates, the RTP audio stops and
the modem gets silence. The audio thread continues to exchange a frame
with the modem each 20 ms. This is the clock of the modem.

The rates of the relay (`state()`) are the TDSR (our rate to the far
modem) and RDSR of the `CONNECT` of the far end. If they are 0, the rates
are the best rate of SELMOD.

After the relay starts, these messages stop it:

- An `SSE` of the audio, VBD, fax relay or text relay states.
- An `SSE` with the Cleardown reason.
- A `CLEARDOWN`.

The endpoint answers a `BREAK` with `BREAKACK`. It accepts `JM_INFO`,
`XID_XCHG`, `MR_EVENT`, `PROF_XCHG` and `START_JM`, but does nothing with
them.

## The data (clause 14, 15.4.11)

The two ends declare V.42/LAPM in `CONNECT`. Thus, the data type is
octet without formatting (`I_OCTET`, 14.5) in the two directions, with a
maximum of 131 octets in each message on TC1. The endpoint also accepts
`I_RAW-OCTET` (with its repeated segments) and `I_CHAR-STAT`/`I_CHAR-DYN`.

Trans-compression (clause 22.2): dmodemd tells `mg=2` (D-TCX) and has no
compression on IP. Thus, a D-TCX far end sends plain octets, also when
its modem compresses. A far end that is not D-TCX, and whose `CONNECT`
shows compression, sends the compressed data of its modem for our modem
to decompress. In this condition, the session sends `CLEARDOWN` (reason 3,
data compression disconnect) and stops.

## SPRT (Annex B)

`dmd::v150::Sprt`:

- The header of Figure B.3: SSID 0, the payload type of the SDP, TC, the
  SEQUENCE NUMBER (0 on TC0 and on packets with no payload), NOA and the
  BASE SEQUENCE NUMBER (for TC1 and TC2: the next number that the user
  accepts), and up to three acknowledgements.
- TC1 and TC2 are reliable. The endpoint acknowledges each payload that it
  receives. The acknowledgement goes in the next packet out, or in an
  acknowledgement-only packet on TC0 after TA01 (90 ms). If three
  acknowledgements are waiting, it goes immediately. The endpoint holds
  packets that are out of order in the window and delivers them in
  order. It acknowledges copies and discards them. It sends packets
  without acknowledgement again after TR03 (500 ms).
- Flow control (B.2.3.5): a channel sends only while its next SEQUENCE
  NUMBER is in its window (TC1 32, TC2 8) of the BASE SEQUENCE NUMBER of
  the far end. Our BSN moves when the session accepts a payload. The
  session does this only while it holds fewer than 1024 octets that the
  modem has not read. If the BSN moved and no packet sent it, an empty
  packet on that channel sends it after TA02 (130 ms on TC1, 500 ms on
  TC2).
- TC3 is unreliable sequenced. The endpoint discards packets that are
  older than the last packet.
- The payload sizes of Table B.2 (132 on TC1 and TC2).

## Tests

| Test | What it covers |
|---|---|
| `V150.*` (`frontend/dmodemd/test/v150_test.cpp`, label `quick`) | The SSE and `CONNECT` codings. The header of SPRT, the acknowledgement timing and the BSN update. 5000 octets on TC1 with each third packet lost and a slow reader. Two sessions through the start (V.34 33600), an answerer with only V.22bis, no modulation in common (cleared down), 20000 octets in each direction with each fifth packet lost, a hang-up (`CLEARDOWN`), and a far end that is not D-TCX and compresses |
| `CoreRelay.*` (`core/test/core_test.cpp`, label `quick`) | Two modems on relayed lines, connected by the relay of the test. The answerer is in `DP_RELAY` immediately, and the caller after call progress hears 2100 Hz. `CONNECT 33600` at the two ends, 2000 and 1500 octets through a 64-octet window, and `NO CARRIER` at the two ends when the relay stops |
| `dmodemd_v150` (`frontend/dmodemd/test/v150_test.py`, label `quick`) | dmodemd with `SIP_V150=yes` against the SIP peer and gateway of the script (SSE on RTP, SPRT with its acknowledgements). Calls out: the lines of the offer, ANSam events from the gateway, `SSE:M` CM, `INIT`, `JM_INFO`, `CONNECT`, a line in each direction, and the `CLEARDOWN` of the gateway that gives `NO CARRIER`. Answers: the ANSam events of dmodemd, `SSE:M` p', `INIT`, `JM_INFO`, `CONNECT`, a line in each direction |

## Not done

- VBD (voice band data) as the result of the call discrimination. An
  `SSE:V` from the far end stops the relay.
- The mixed and VBD-select call discrimination modes (`CDSCselect` 2
  and 3).
- `sprtparm`, `jmdelay=yes` (`START_JM`), XID exchange and profiles.
- Modulations other than the four that the endpoint offers (V.17, V.23,
  V.90 and higher).
- Retrains and rate renegotiations (`MR_EVENT`) do not change the rate.
- Nobody has tried interoperation with a commercial gateway.
