# ISDN: the clear 64 kbit/s channel

An ISDN B-channel carries 64 kbit/s: 8000 octets a second each way. A
voice call puts G.711 audio in those octets, and every D-Modem mode
already runs on that (V.90 and V.92 were made for a server on such a
line). A data call puts data straight in the octets, with no audio at
all. This page is about that second kind of call: the clear channel.

This is phase 1 of ISDN support. It gives D-Modem a clear 64 kbit/s line
mode and the two ways for a call to reach it: SIP in CLEARMODE, and an
Asterisk channel with a digital bearer. The B-channel protocols that
other ISDN equipment speaks (X.75, sync PPP, V.110, V.120) come next, on
top of this line mode.

## AT+MS=CLEAR

`AT+MS=CLEAR` (or `AT+MS=64`) is the clear channel pump
(`dsp/dp_clear.cpp`):

- No modulation: each octet of the channel carries 8 of the link layer's
  bits, the first bit the octet's least significant. `CONNECT 64000`,
  both ways.
- No call progress and no answer tone: `ATD` and `ATA` start the pump at
  once, as on a leased line. There is no answer delay (S92) either.
- V.42, V.44, V.42bis and MNP run over it as over any modulation; `\N0`
  gives plain asynchronous characters (start bit, 8 data bits, stop bit)
  on the bit stream.
- The start-up is D-Modem's own, so the far end is another D-Modem: each
  end sends HDLC flags (`0x7E`) until it has heard 32 of the other's in a
  row, then 64 more flags, then 64 octets of ones (`0xFF`), and then the
  link's bits with `CONNECT`. What the far end sends from its ones on
  goes to the link. No flags within S7 is `NO CARRIER`.
- Nothing on a clear channel tells a carrier from none: the call ends
  when the line ends it (a SIP BYE, the PBX's hangup), or with `ATH`.
- It needs a digital line at 8000 Hz (SIP, a PBX's channel). On an
  analogue line, `+MS=CLEAR` runs V.34 instead.

The line gives the modem 16 bit samples, so the pump takes each octet as
the G.711 sample it would be. A-law gives every octet back as it was;
mu-law has one sample for its two zeros, so `0x7F` would arrive as
`0xFF`. Hosts that know a call is clear use A-law for it (below). On a
mu-law G.711 call with `+MS=CLEAR` set by hand, the pump runs but logs
the problem.

`AT&L2+MS=CLEAR` is a nailed-up 64 kbit/s channel (a leased DS0 or E1
time slot): ATD with no number calls, `S0=1` answers with no ring, and
the call starts again by itself after `NO CARRIER`, as for the other
leased-line modes ([leased.md](leased.md)).

## dmodemd: SIP in CLEARMODE

RFC 4040's CLEARMODE is the 64 kbit/s clear channel as an RTP payload
(`CLEARMODE/8000`, a dynamic payload type): the octets as they are, no
codec. dmodemd takes it like this:

- **Dialling with `AT+MS=CLEAR`**: the offer has CLEARMODE first (payload
  type 97), then PCMA and PCMU. An answer in CLEARMODE is a clear call;
  an answer in PCMA is clear too when nothing on the way transcodes.
- **A call that comes in** with CLEARMODE before every G.711 format in
  its offer is an ISDN data call: dmodemd answers it in CLEARMODE and the
  modem runs the clear channel pump, whatever `+MS` says. An offer with
  G.711 first gets G.711, as before.
- A CLEARMODE call has no telephone-events: their tones would be data.
- `SIP_CLEARMODE=no` turns CLEARMODE off: neither offered nor taken.

Asterisk has no CLEARMODE codec, so a clear call over SIP needs a peer
that has one (another dmodemd, a gateway, Osmocom's media gateway). RFC
4040 has no recovery for lost packets: run it on a clean network, and
use V.42 on top.

`dmodemd/test/clearmode_test.py` checks both directions against a SIP
peer and a far clear channel of the script's own.

## app_dmodem: an ISDN data call through Asterisk

A call that reaches `DModem()` with an unrestricted digital bearer
(`${CHANNEL(transfercapability)}` is `DIGITAL`, as chan_dahdi sets it
for an ISDN data call on a BRI or PRI span) runs on the clear channel.
The module reads and writes the channel in its own G.711 format, so the
octets come through as they are, and the modem takes them as clear.

The profile's `clear` option (`dmodem.conf`) chooses:

| `clear` | |
|---|---|
| `auto` (default) | clear for a call with a digital bearer |
| `yes` | every call is clear |
| `no` | never: digital calls run as G.711 audio, as before |

FreeSWITCH's `mod_dmodem` has the same option through the shared bridge;
`auto` there is the same as `no`, since FreeSWITCH gives the module no
bearer capability.

Not yet tried on real ISDN hardware: chan_dahdi is expected to hand a
digital call's octets to the application unchanged (its audio mode off,
no echo canceller or gain), which needs confirming on a card. The
restricted digital bearer (56 kbit/s, North America) is not taken as
clear.

## libdmodem

`dmodem_config`'s `clear` (or `dmodem_set_clear()` before each call,
for a program that learns the bearer per call) makes the line clear:
every call runs the clear channel pump, and `dmodem_process_g711()` takes
the octets as they are. `dsp/test/clear_call_test.cpp` has two modems on
a simulated clear channel: `CONNECT 64000`, 200 kB each way through
V.42, every octet value without V.42, a late answerer, no answerer, and
a nailed-up channel.

## What comes next

The feasibility study's later phases build on this line mode:

1. X.75 (LAPB, as the ISDN BBSes and mailers used it) and sync PPP in
   HDLC, with isdn4linux-style AT commands (`AT&E`, `ATS14`, `ATS18`).
2. V.110 and V.120 rate adaption, and mISDN for BRI cards on Linux.
3. Maybe CAPI 2.0 emulation, and hardware.
