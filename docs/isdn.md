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
and gives Asterisk a CLEARMODE codec (`codec_clearmode`). Phase 3 adds
V.110 rate adaption (`AT+CBST`) and V.120 (`AT+B2=9`), and CAPI 2.0 for
ISDN applications (`dmodem-capid`).

## AT+MS=CLEAR

`AT+MS=CLEAR` (or `AT+MS=64`) is the clear channel pump
(`dsp/dp_clear.cpp`):

- No modulation: each octet of the channel carries 8 of the link layer's
  bits, the first bit the octet's most significant, as I.460 and RFC
  4040 have it (and Osmocom, and a TDM time slot). `CONNECT 64000`,
  both ways. (Phases 1 and 2 put the first bit in the least significant
  bit; a D-Modem built before phase 3 does not understand a newer one's
  X.75, PPP or V.42 on a clear channel.)
- No call progress and no answer tone: `ATD` and `ATA` start the pump at
  once, as on a leased line. There is no answer delay (S92) either.
- What runs on it is `AT+B2`'s: X.75 by default, or sync PPP, or the
  modem's own link (below).
- The start-up is D-Modem's own: each end sends HDLC flags (`0x7E`)
  until it has heard 32 of the other's in a row, then 64 more flags,
  then 64 octets of ones (`0xFF`), and then the link's bits with
  `CONNECT`. What the far end sends from its ones on goes to the link.
  Other ISDN equipment sends flags and then its frames with no ones
  between (an X.75 adapter's SABM): the link takes it from there. No
  flags within S7 is `NO CARRIER`.
- With V.110 (`AT+CBST`, below) the start is V.110's own instead.
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

MSN selection by AT command (isdn4linux's `AT&E`, CAPI's called party
number) is not done: every number a line brings to the modem is the
modem's. On an mISDN card, dmodemd's `--misdn-msn` picks the MSN (below).

## AT+B2=9: V.120

`AT+B2=9` is V.120 (`link/lapb.cpp`, CAPI's B2 protocol 9), the protocol
of North American ISDN terminal adapters:

- Q.921's frames: a two octet address for logical link 256 (V.120's
  default), the caller on the user side (its commands C/R 0, so `08 01`,
  the called end's `0A 01`).
- Extended mode: SABME, sequence numbers modulo 128 in two control
  octets, a window of 7 frames, the same recovery and timers as X.75.
- Each I frame's information starts with V.120's terminal adaption
  header, one octet: `0x83` (asynchronous, each frame whole). Whatever
  header the far end sends, with its extension octets, is taken off.
- `CONNECT 64000/V.120` with `\V1`; `ATH` sends DISC.
- Not done: other logical links, more than one at a time, UI frames,
  the header's break and error bits, and the link's parameters from the
  call's low layer compatibility.

## AT+CBST: V.110 rate adaption

V.110 carries a slower user rate in a 64 kbit/s channel: 80 bit frames
at an intermediate rate of 8, 16, 32 or 64 kbit/s (RA1), that rate in
the first 1, 2, 4 or 8 bits of each octet, the rest ones (RA2). ISDN
terminal adapters used it, and GSM's circuit switched data still rides
on it through the network (Osmocom). `dsp/v110.cpp` has the frames,
`dsp/dp_clear.cpp` runs them.

`AT+CBST=<speed>[,<name>[,<ce>]]` is 27.007's bearer service command:

| `<speed>` | user rate | intermediate rate |
|---|---|---|
| `0` (default) | no V.110 (the answerer takes the caller's) | |
| `65` | 300 (on 600's frames, each bit twice) | 8k |
| `66`, `68`, `70` | 1200, 2400, 4800 | 8k |
| `71` | 9600 | 16k |
| `75`, `79` | 14400, 19200 | 32k |
| `80`, `81` | 28800, 38400 | 64k |
| `83` | 56000 (7 bits an octet, no frames) | 64k |

- `<name>` `0` (default) is asynchronous: the DTE's characters as start
  and stop bits at the user rate, through the modem's link whatever
  `AT+B2` says (`\N0` for plain characters, or V.42 if both ends ask
  for it). `1` is synchronous: `AT+B2`'s X.75, V.120 or PPP in HDLC at
  the user rate. `<ce>` has no use here (0 or 1).
- Each end sends frames with S and X OFF and its data bits ones until it
  has frame sync (three good alignment patterns in a row), then S and X
  ON. `CONNECT <rate>` once in sync with the other end's X ON (`CONNECT
  9600/V.110` with `\V1` and no V.42). The user's bits go to the link
  while the other end's S bits are ON; three bad patterns in a row lose
  sync, and it is found again.
- An answerer with `AT+CBST=0` looks for the caller's frames at each
  intermediate rate while it waits for D-Modem's flags. The first rate in
  sync gives the user rate by its E bits: 600 to 38400, including 7200,
  12000 and 24000, which have no `<speed>` here. 300 is found as 600,
  and 56000 not at all: set them on the answerer too.
- 48000 bit/s and the network independent clocking bits (E4 to E6) are
  not done.

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

## dmodemd --misdn: a BRI card on Linux

With `--misdn <dev>` dmodemd's line is an ISDN basic rate interface
through Linux's mISDN, the kernel's ISDN stack: an HFC-S PCI or USB
card, an AVM Fritz!Card PCI, or another card with an mISDN driver. It
takes the S0 bus's terminal side, one call at a time.

```
modprobe hfcpci            # or hfcsusb, avmfritz, ...
dmodemd --misdn 0 --misdn-msn 4711
```

`<dev>` is the device's number or its name (`hfc-pci.1`). dmodemd must
start as root, because listing mISDN's devices needs `CAP_NET_RAW`; it
opens the D-channel and then goes on as `nobody`, as it does for its pty.

- **Layer 2** (LAPD, the TEI) is the kernel's. Q.931, the call control
  on top of it, is dmodemd's own (`dmodemd/q931.cpp`, ETSI's Euro-ISDN
  user side).
- **Calls out** send the whole number in the SETUP, with sending
  complete. With `AT+MS=CLEAR` the bearer is unrestricted digital
  information, and the clear channel modes above run on it: X.75, V.110,
  V.120 and the rest. Any other `+MS` mode makes a 3.1 kHz audio call,
  and the modem runs as it does on any digital line. The network picks
  the B-channel. Cause 17 (user busy) is `BUSY`, causes 18 and 19 are
  `NO ANSWER`, and anything else is `NO CARRIER`.
- **Calls in** ring when they are for our MSN (`--misdn-msn`: the
  called number must end with it), or for any number without one. They
  need a bearer of speech, 3.1 kHz audio or unrestricted digital
  information, and a B-channel. `RING` repeats every 5 seconds, and the
  calling number is the caller ID (`AT+VCID=1`). A call with the bearer
  unrestricted digital information is answered on the clear channel.
  With a call up, another call for us gets `RELEASE COMPLETE` with
  cause 17.
- **The B-channel** runs in the card's transparent mode (`ISDN_P_B_RAW`).
  Its octets are A-law, bit reversed: the cards send an octet's least
  significant bit first, and G.711 and the clear channel send the most
  significant first. `--misdn-noflip` takes the octets as they come, for
  a card that does not need the reversal. The modem answers each block
  of octets with as many, so the B-channel's clock paces the modem.
- The bearer we send is always A-law. Overlap dialling, supplementary
  services and PRI cards are not supported.

**Untested on hardware.** `dmodemd/test/misdn_test.py` runs two dmodemd
on fake cards (`dmodemd/test/fake_misdn.cpp`, loaded with `LD_PRELOAD`)
through the script's own network. The test covers a clear channel call
(X.75, caller ID, text each way), a 3.1 kHz V.22bis call, `BUSY` and
`NO ANSWER`. `dmodemd/test/q931_test.cpp` covers Q.931's states, timers
and messages. The bit reversal follows Asterisk's chan_misdn and LCR, but
no card has checked it yet.

## app_dmodem: an ISDN data call through Asterisk

(app_dmodem is built only for your own Asterisk, `-DDMODEM_APP_DMODEM=ON`,
and a built one is not passed on: see [LICENSING.md](../LICENSING.md).
dmodem-ari does not take clear-channel calls yet.)

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
and installed whenever Asterisk's headers are found (it has no D-Modem
code, and is under Asterisk's GPL-2.0-only, so it can be shipped as any
module). It registers the `clearmode` codec
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
nailed-up channel, X.75, V.120 and PPP checked frame by frame against
a decoder of the test's own (with lost audio and a release for X.75 and
V.120), V.110 at each `+CBST` rate with its frames checked the same way
and found by the answerer, V.42 over V.110, X.75 and V.120 over
synchronous V.110, and another device's HDLC start with no ones.

`dsp/test/v110_osmo_test.cpp` runs V.110 against libosmocore's own (the
V.110 code GSM's circuit switched data uses in Osmocom), both ways at
each rate it has: built when CMake's `OSMOCORE_SRC` names a libosmocore
source tree (1.14 or later); only its `src/isdn/v110.c` is compiled, into
the test. CI's build job clones libosmocore 1.14.2 for it; all twelve of
its rates, 600 to 38400 bit/s, pass both ways.

## CAPI 2.0: dmodem-capid

`dmodem-capid` (`capi/`) makes a D-Modem a CAPI 2.0 controller, for the
ISDN software written against CAPI: capi4hylafax, chan_capi,
pppd's capiplugin, and the BBS and mailer software of the isdn4linux
years. It is a program of its own that drives the modem through its DTE
with AT commands, so it works with every line D-Modem has: SIP (with
CLEARMODE for data calls), an Asterisk channel, an mISDN card, and any
line that comes later. The applications reach it over TCP with
libcapi20's remote CAPI module (`rcapi`, the protocol of isdn4k-utils'
`rcapid`), so they run unchanged on the same machine or another one.

```
dmodemd ...                              # its pty: /dev/ttyDM0 (or --tcp)
dmodem-capid -l 6000 /dev/ttyDM0         # or tcp:127.0.0.1:2323

# /etc/capi20.conf where the applications run
REMOTE rcapi localhost 6000
```

`-i` adds AT commands after the controller's own setup (`ATE0V1Q0X3`,
`+CRC=1`, `+VCID=1`, `S0=0`).

It is one controller (1) with one B-channel: one call at a time, PLCI
`0x0101` and NCCI `0x00010101`. What it does with each message:

- **LISTEN_REQ**: the CIP mask is kept. A ring is offered (CONNECT_IND)
  to each application whose mask takes it: CIP 2 (unrestricted digital)
  for `+CRING: SYNC`, the modem's ring of a clear channel call (`AT+CRC`
  below), else the first of 4 (3.1 kHz audio), 16 (telephony) and 1
  (speech) the mask has. The caller's number comes from `AT+VCID`. The
  called number is empty (no MSN selection yet).
- **CONNECT_REQ**: the B protocol is set with AT commands (below), then
  `ATD` with the called number. `CONNECT` is CONNECT_ACTIVE_IND. Then
  the application's CONNECT_B3_REQ gives CONNECT_B3_ACTIVE_IND.
- **CONNECT_RESP**: the first application to accept the call has it
  (the others get DISCONNECT_IND, reason `0x3304`). Its B protocol is
  set, then `ATA`. `CONNECT` is CONNECT_ACTIVE_IND and CONNECT_B3_IND.
  A call nobody takes rings on unanswered. When the rings stop for 10
  s, the applications it was offered to get DISCONNECT_IND.
- **DATA_B3**: the data goes to the modem as it is, and its CONF comes
  once the modem has taken it. The modem's data comes in DATA_B3_INDs
  of up to the application's maxBDataLen, as many unanswered as its
  maxBDataBlocks (7 at most).
- **DISCONNECT_REQ**: `+++` between guard times (D-Modem's is 500 ms),
  then `ATH`; DISCONNECT_B3_IND and DISCONNECT_IND (reason 0) follow.
  `NO CARRIER` from the modem ends the call the same way, reason
  `0x3490`, once the data before it has gone to the application. `BUSY`
  is `0x3491`, `NO ANSWER` `0x3493`.
- FACILITY_REQ answers supplementary services' GetSupportedServices
  (none), and everything else with "facility not supported". ALERT_REQ
  and INFO_REQ are confirmed, and do nothing. SELECT_B_PROTOCOL and
  RESET_B3 are refused.

The B protocols (CAPI's B1, B2, B3) and the AT commands for them:

| B1 | B2 | B3 | the modem |
|---|---|---|---|
| 0 (64 kbit/s HDLC) | 0 (X.75) | 0 | `+MS=CLEAR;+B2=0` |
| 0 | 1 (transparent) | 0 | `+MS=CLEAR;+B2=5`: each DATA_B3 a whole frame |
| 0 | 9 (V.120 async) | 0 | `+MS=CLEAR;+B2=9` |
| 2 (V.110 async) | 1 | 0 | `\N0+MS=CLEAR;+CBST=<rate>,0;+B2=7` |
| 3 (V.110 sync) | 0, 1, 9 | 0 | `+CBST=<rate>,1` and B2 as above |
| 7 (modem, negotiated) | 7 (V.42, MNP) | 0, 7 | the modem's own `+MS`, `\N3` |
| 7, 8 (modem) | 1 | 0 | the modem's own `+MS`, `\N0` |

An empty B protocol is CAPI's default, X.75. The V.110 rate is B1's
configuration's first word (300 to 56000, or 0 for the caller's). B1 0
with B2 1 is what capiplugin asks for: the controller turns each
DATA_B3 into a frame in RFC 1662's framing for the modem's sync PPP
(`+B2=5`), and each frame back into a DATA_B3. Other protocols (fax, the
B1 1 bit-transparent channel, X.25) are refused with CAPI's "not
supported" info.

On a modem call (B1 7) the modem's own modulation is used: the
controller reads `AT+MS?` at the start, and sets it again on each such
call.

`AT+CRC=1` (27.007's) is the one addition to the modem for this: a ring
is `+CRING: SYNC` for a call on a clear channel and `+CRING: VOICE` for
any other, instead of `RING`.

Some limits: one call at a time; the online data is watched for the
modem's `NO CARRIER` line (so that line inside the data would end the
call); no fax (CAPI's B1 4) yet; no Windows `capi2032.dll` yet.
libdmodem's `dmodem_ring()` rings a modem for a program that learns of
calls itself; `capi/test/capi_test.cpp` uses it.

`capi/test/capi_test.cpp` has the controller on one of two libdmodem
modems on a simulated line, with an application of the test's:

- calls out over X.75, V.120, frames (sync PPP), V.110 at 9600 bit/s,
  and a modem call (V.42 over V.90) with data both ways;
- a call in on a clear channel with the caller's number, and a second
  listening application that loses it;
- hangups from either end, a call nobody answers, a call that stops
  ringing, and a B protocol the controller lacks;
- the TCP server in rcapi's framing, against a modem of the test's.

Tried with libcapi20 from isdn4k-utils (its `rcapi` module): `capiinfo`
lists the controller and its profile, and an application of libcapi20's
called a libdmodem modem through `dmodem-capid` with X.75, frames, V.120,
V.110 at 9600 bit/s and a modem call (V.42 over V.90), its data echoed
back byte for byte, then hung up. Not yet tried: an incoming call
through libcapi20, and the applications themselves (capi4hylafax,
chan_capi, capiplugin).

## What comes next

The feasibility study's later phases build on this line mode:

1. Done: X.75 and sync PPP (`AT+B2`), and `codec_clearmode`. Still to
   come here: V.42bis over X.75, and MSN selection.
2. Done: V.110 (`AT+CBST`), V.120 (`AT+B2=9`), and mISDN for BRI cards
   on Linux (`--misdn`), which still needs a test on a real card.
3. CAPI 2.0: done for data and modem calls (`dmodem-capid`, above). Fax
   over CAPI and a Windows `capi2032.dll` are still to come. Hardware
   (an S0 USB stick) is being designed.
