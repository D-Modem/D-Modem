# ISDN: the clear 64 kbit/s channel

An ISDN B-channel carries 64 kbit/s: 8000 octets a second each way. A
voice call puts G.711 audio in those octets, and every D-Modem mode
already runs on that (V.90 and V.92 were made for a server on such a
line). A data call puts data straight in the octets, with no audio at
all. This page is about that second kind of call: the clear channel.

Phase 1 of ISDN support gave D-Modem a clear 64 kbit/s line mode and
the two ways for a call to reach it: SIP in CLEARMODE, and an Asterisk
channel with a digital bearer. Phase 2 runs the B-channel protocols of
ISDN terminal adapters on it: X.75 and synchronous PPP (`AT+B2`, below),
and gives Asterisk a CLEARMODE codec (`codec_clearmode`). V.110 and
V.120 come later.

## AT+MS=CLEAR

`AT+MS=CLEAR` (or `AT+MS=64`) is the clear channel pump
(`dsp/dp_clear.cpp`):

- No modulation: each octet of the channel carries 8 of the link layer's
  bits, the first bit the octet's least significant. `CONNECT 64000`,
  both ways.
- No call progress and no answer tone: `ATD` and `ATA` start the pump at
  once, as on a leased line. There is no answer delay (S92) either.
- What runs on it is `AT+B2`'s: X.75 by default, or sync PPP, or the
  modem's own link (below).
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

## AT+B2: X.75, PPP or the modem's link

`AT+B2` chooses the protocol on the clear channel, with CAPI 2.0's
numbers for layer 2. Other modes do not look at it. The isdn4linux
commands for this (`AT&E`, `ATS14`, `ATS18`) already mean other things
here (the CONNECT rate, quiet mode, the loop test's timer), so they stay
as they are.

| `AT+B2=` | on the channel | `CONNECT` (with `\V1`) |
|---|---|---|
| `0` (default) | X.75 | `CONNECT 64000/X.75` |
| `5` | synchronous PPP | `CONNECT 64000/PPP` |
| `7` | the modem's link: V.42, MNP, or characters with `\N0` | as for a modem |

**X.75** (`link/lapb.cpp`) is the X.75 SLP (LAPB) that isdn4linux and
the CAPI cards speak, and what ISDN BBSes and mailers expected:

- HDLC frames, basic mode (modulo 8), a window of 7 frames.
- The caller sends commands to address B (`0x01`) and answers at A
  (`0x03`); the called end the other way round.
- The caller sends SABM. The called end waits for it, and sends its own
  after 4 T1 with nothing heard (as isdn4linux does).
- I frames carry up to 256 of the DTE's characters; up to 2048 are taken
  from the far end. T1 is 1 s plus the time of the frames in flight,
  N2 = 4 retries. Lost frames come again on REJ, or on T1's poll.
- `ATH` sends DISC; a DISC or DM from the far end is `NO CARRIER`.
- V.42bis over X.75 (as ISDN adapters could) is not done yet.

**Sync PPP** (`link/ppp_sync.cpp`, RFC 1618) is for a PPP stack on the
DTE, such as pppd on the modem's serial port: it speaks PPP in its
asynchronous framing (RFC 1662), the same as with an analogue modem.
D-Modem undoes the escapes of each frame, checks and drops its FCS, and
sends it in an HDLC frame of its own. Frames from the line go to the DTE
escaped (every octet below `0x20`, whatever the ACCM) with an FCS. A
frame with a bad FCS from the DTE, or one longer than 2048 octets, is
dropped and logged; PPP's own retries cover it. Tried between two
D-Modems in `dsp/test/clear_call_test.cpp`; not yet with pppd at each
end.

**The modem's link** (`7`) is phase 1's clear channel: V.42, V.44,
V.42bis and MNP run over it as over any modulation, and `\N0` gives plain
asynchronous characters (start bit, 8 data bits, stop bit) on the bit
stream. A D-Modem from before `AT+B2` speaks this.

MSN selection (isdn4linux's `AT&E`, CAPI's called party number) is not
done: every number a line brings to the modem is the modem's.

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

Asterisk has no CLEARMODE codec of its own; `codec_clearmode` (below)
gives it one. Other peers that have it: another dmodemd, a gateway,
Osmocom's media gateway. RFC 4040 has no recovery for lost packets: run
it on a clean network. X.75 retransmits what is lost.

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

## codec_clearmode: CLEARMODE in Asterisk

`asterisk/codec_clearmode.c` adds RFC 4040's codec to Asterisk, built
and installed with `app_dmodem`. It registers the `clearmode` codec
(8000 octets a second, frames of 10 to 150 ms) and its format, and maps
it to `CLEARMODE/8000` in RTP, using only Asterisk's public API. There is
no translator: a clear call's octets are data, so a CLEARMODE channel
bridges only with another CLEARMODE channel, or goes to `DModem()`.

```
; modules.conf (autoload finds it too)
load = codec_clearmode.so

; pjsip.conf, the endpoint of the ISDN gateway or the other D-Modem
[isdn-gw]
type = endpoint
disallow = all
allow = clearmode,alaw   ; clearmode first: data calls, alaw: voice
```

A call that reaches `DModem()` in CLEARMODE is clear, whatever the
profile's `clear` says, and the modem runs `AT+B2`'s protocol on it.
`asterisk/test/sip.sh` with `CODEC=clearmode` runs dmodemd's X.75 call
both ways through Asterisk in CLEARMODE.

The idea comes from a gist by 7marcus9; this module is written anew
for D-Modem. dmodem-ari does not take CLEARMODE: ARI's external media
has G.711 alone.

## libdmodem

`dmodem_config`'s `clear` (or `dmodem_set_clear()` before each call,
for a program that learns the bearer per call) makes the line clear:
every call runs the clear channel pump, and `dmodem_process_g711()` takes
the octets as they are. `dsp/test/clear_call_test.cpp` has two modems on
a simulated clear channel: `CONNECT 64000`, 200 kB each way through
V.42, every octet value without V.42, a late answerer, no answerer, a
nailed-up channel, and X.75 and PPP checked frame by frame against a
decoder of the test's own (with lost audio and a release for X.75).

## What comes next

The feasibility study's later phases build on this line mode:

1. Done: X.75 and sync PPP (`AT+B2`), and `codec_clearmode`. Still to
   come here: V.42bis over X.75, and MSN selection.
2. V.110 and V.120 rate adaption, and mISDN for BRI cards on Linux.
3. Maybe CAPI 2.0 emulation, and hardware.
