# Modem relay (V.150.1): technical details

The plain-English overview is [v150.md](v150.md).

dmodemd on SIP is a V.150.1 modem-over-IP endpoint: the modem and its
gateway in one (U-MR, `mr=1`). The far end is a V.150.1 gateway that
terminates its own modem's modulation, or another such endpoint. All
the references here are to ITU-T V.150.1 (01/2003).

## The parts

| Part | Where | What it does |
|---|---|---|
| `DP_RELAY` (151) | `dsp/dp_relay.cpp` | The relay pump. It waits for the relay to come up, `CONNECT`s at its rates, then moves octets between the host and the core's asynchronous link as start-stop characters (8N1), within the rate each way, and sends silence. No relay within S7, or the relay over, is `DPSTAT_ERROR` (`NO CARRIER`). It has no `AT+MS` name: the core picks it |
| `struct dp_relay`, `MDMPRM_RELAY` (75) | `dsp/modem_defs.h`, `core/interface.cpp` | The pump's view of the host: `state()` (0 not up, 1 up with the rates, -1 over), `read()`, `write()` |
| `Host::relay_line()`, `relay_state()`, `relay_read()`, `relay_write()` | `core/dcore.h` | The host says a call is relayed. `Modem::relay_channel()` is true for one at 8000 Hz in data mode (`+FCLASS=0`); `change_dp()` then starts `DP_RELAY` in place of `+MS`'s pump: at once when answering (the answer delay is short, as on a clear channel), after call progress hears the answer tone when calling. `DP_RELAY` is in the list of pumps with no V.42 and MNP of the core's own: the far gateway's link carries the error correction |
| `dmd::v150::Sse`, `Sprt`, `Session` | `frontend/dmodemd/v150.h`, `v150.cpp` | Annex C's SSE coding, Annex B's SPRT, and the MoIP messages of clause 15.4 as a session, as plain data (the caller's clock, packets as octets) |
| `Relay`, `Media` | `frontend/dmodemd/sip.cpp` | The session and its SPRT socket, shared by the audio thread (the SSEs, SPRT's packets every 20 ms, ANSam as events) and the modem thread (`sip::relay_state()` and friends, through `SipLine`) |

## The call set-up (Annex E)

With `SIP_V150=yes`, a data call's offer (not one for `AT+MS=CLEAR`, not
in fax mode) has, beside the G.711 audio:

    m=audio <port> RTP/AVP 0 8 101 118
    a=rtpmap:101 telephone-event/8000
    a=fmtp:101 0-16,32-35
    a=rtpmap:118 v150fw/8000
    m=audio <port> udpsprt 120
    a=sprtmap:120 v150mr/8000
    a=fmtp:120 mr=1;mg=2;CDSCselect=1;mrmods=1,3-4,12;jmdelay=no

The SSE format (`v150fw`) goes on the audio line; the modem relay line
has its own UDP socket. An answer with the `udpsprt` line on (port not
0) and `v150fw` on its audio starts the relay; one without it is an
ordinary audio call, our `udpsprt` line turned off in the call's SDP
from then on. An offer with both is answered with both (the far end's
payload types) unless `SIP_V150` is not `yes` or the modem is in fax
mode; the call is then relayed as soon as the modem's pump asks
(`sip::relay()`). The far end's `mg` comes from its `a=fmtp`; no
`sprtparm` is sent or read, so both ends use Table B.2's defaults.

`line_of()` skips `udpsprt` lines when it looks for the audio, and
`take_relay()` makes the `Relay` once per call (a re-INVITE keeps it).

## Call discrimination (clause 20, Figure 21)

D-Modem uses the audio procedure (`CDSCselect=1`): the answer tone as
RFC 4733 events (20.4.3).

| | Caller (G1) | Answerer (G2) |
|---|---|---|
| 1 | Call progress hears the answer tone (the far end's RFC 4733 event 34 or 32, played by `EventPlayer`, or the tone in the audio) and hands over to `DP_RELAY` | `DP_RELAY` starts at `ATA` (or S0) and asks for the relay's state, which starts the session: ANSam goes as RFC 4733 event 34 (volume -12 dBm0) every 20 ms in place of the audio, its duration growing, a new event before 60000 samples |
| 2 | `SSE:M`, reason CM, the information Table 13's bits for V.34, V.32/V.32bis, V.22/V.22bis and V.21 (`0x5808`) | |
| 3 | | On `SSE:M`: ANSam ends (three end packets), `SSE:M` back with reason p' (19), modem relay on |
| 4 | On `SSE:M` (or the first SPRT packet): modem relay on | |
| 5 | `INIT` on TC2 (no optional data types) | `INIT` on TC2 |
| 6 | | On the caller's `INIT`: `JM_INFO` (call function data `0x8600`, the modulations chosen `0xA000 \| mods >> 3`, LAPM `0x5800`) and `CONNECT` with the best modulation both have (V.34 33600, V.32bis 14400, V.22bis 2400, V.21 300), EC V.42/LAPM, no compression |
| 7 | On the answerer's `CONNECT`: `CONNECT` back, its SELMOD, its rates the other way round. Up | On the caller's `CONNECT`: up |

Each SSE is sent three times, 20 ms apart, with the same RTP timestamp
(C.3.1). While modem relay is on, the RTP audio stops and the modem gets
silence; the audio thread still exchanges a frame with the modem every
20 ms, which is the modem's clock.

The relay's rates (`state()`) are the far end's `CONNECT`'s TDSR (ours
towards the far modem) and RDSR, or SELMOD's best if they are 0.

An `SSE` of the audio, VBD, fax relay or text relay states, or with the
Cleardown reason, after the relay started ends it; so does a
`CLEARDOWN`. A `BREAK` is answered with `BREAKACK`; `JM_INFO`,
`XID_XCHG`, `MR_EVENT`, `PROF_XCHG` and `START_JM` are taken and not
acted on.

## The data (clause 14, 15.4.11)

Both ends declare V.42/LAPM in `CONNECT`, so the data type is octet
without formatting (`I_OCTET`, 14.5) both ways, at most 131 octets a
message on TC1. `I_RAW-OCTET` (with its repeated segments) and
`I_CHAR-STAT`/`I_CHAR-DYN` are taken too.

Trans-compression (clause 22.2): dmodemd says `mg=2` (D-TCX) and has no
compression on IP. A D-TCX far end then sends plain octets whatever its
modem compresses. A far end that is not D-TCX, whose `CONNECT` shows
compression, would send its modem's compressed data for ours to
decompress: the session sends `CLEARDOWN` (reason 3, data compression
disconnect) and ends.

## SPRT (Annex B)

`dmd::v150::Sprt`:

- The header of Figure B.3: SSID 0, the payload type of the SDP, TC, the
  SEQUENCE NUMBER (0 on TC0 and on packets with no payload), NOA and the
  BASE SEQUENCE NUMBER (TC1 and TC2's: the next the user takes), up to
  three acknowledgements.
- TC1 and TC2 reliable: every payload received is acknowledged, in the
  next packet out or in an acknowledgement-only packet on TC0 after TA01
  (90 ms), or at once when three are owed. Packets out of order are held
  within the window and delivered in order; copies are acknowledged and
  dropped. Unacknowledged packets go again after TR03 (500 ms).
- Flow control (B.2.3.5): a channel sends only while its next SEQUENCE
  NUMBER is within its window (TC1 32, TC2 8) of the far end's BASE
  SEQUENCE NUMBER. Our BSN moves when the session takes a payload, which
  it does only while it holds fewer than 1024 octets the modem has not
  read; when it moved and nothing carried it, an empty packet on that
  channel carries it after TA02 (130 ms on TC1, 500 ms on TC2).
- TC3 unreliable sequenced: older packets than the last are dropped.
- Table B.2's payload sizes (132 on TC1 and TC2).

## Tests

| Test | What it covers |
|---|---|
| `V150.*` (`frontend/dmodemd/test/v150_test.cpp`, label `quick`) | The SSE and `CONNECT` codings; SPRT's header, acknowledgement timing and BSN update; 5000 octets on TC1 with every third packet lost and a slow reader; two sessions through the start (V.34 33600), a V.22bis-only answerer, no modulation in common (cleared down), 20000 octets each way with every fifth packet lost, a hang-up (`CLEARDOWN`), and a far end that is not D-TCX and compresses |
| `CoreRelay.*` (`core/test/core_test.cpp`, label `quick`) | Two modems on relayed lines joined by the test's own relay: the answerer in `DP_RELAY` at once, the caller after call progress hears 2100 Hz; `CONNECT 33600` at both, 2000 and 1500 octets through a 64-octet window, and `NO CARRIER` at both when the relay ends |
| `dmodemd_v150` (`frontend/dmodemd/test/v150_test.py`, label `quick`) | dmodemd with `SIP_V150=yes` against the script's own SIP peer and gateway (SSE on RTP, SPRT with its acknowledgements): calling out (the offer's lines, ANSam events from the gateway, `SSE:M` CM, `INIT`, `JM_INFO`, `CONNECT`, a line each way, the gateway's `CLEARDOWN` giving `NO CARRIER`) and answering (dmodemd's ANSam events, `SSE:M` p', `INIT`, `JM_INFO`, `CONNECT`, a line each way) |

## Not done

- VBD (voice band data) as the call discrimination's outcome: an
  `SSE:V` from the far end ends the relay.
- The mixed and VBD-select call discrimination modes (`CDSCselect` 2
  and 3).
- `sprtparm`, `jmdelay=yes` (`START_JM`), XID exchange and profiles.
- Modulations beyond the four offered (V.17, V.23, V.90 and up).
- Retrains and rate renegotiations (`MR_EVENT`) do not change the rate.
- Interoperation with a commercial gateway has not been tried.
