# ISDN: the clear 64 kbit/s channel: technical details

This is the technical companion to [ISDN: the clear 64 kbit/s channel](isdn.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

An ISDN B-channel (bearer channel) carries 64 kbit/s: 8000 octets a
second in each direction. There are two kinds of call on it:

- A **voice call** puts G.711 audio in those octets. Every D-Modem mode
  already runs on that; V.90 and V.92 were designed for a server on
  exactly such a line.
- A **data call** puts data straight into the octets, with no audio at
  all. This is the *clear channel*, and it is what this page is about.

ISDN support arrived in three phases:

| Phase | What it added |
|---|---|
| 1 | A clear 64 kbit/s line mode, and two ways for a call to reach it: SIP in CLEARMODE, and an Asterisk channel with a digital bearer. |
| 2 | The B-channel protocols of ISDN terminal adapters: X.75 and synchronous PPP (`AT+B2`), and a CLEARMODE codec for Asterisk (`codec_clearmode`). |
| 3 | V.110 rate adaption (`AT+CBST`), V.120 (`AT+B2=9`), and CAPI 2.0 for ISDN applications (`dmodem-capid`). |

## AT+MS=CLEAR

`AT+MS=CLEAR` (or `AT+MS=64`) selects the clear channel pump,
`dsp/dp_clear.cpp`.

### What the pump does

- **No modulation.** Each octet of the channel carries 8 of the link
  layer's bits. The first bit goes in the octet's most significant bit,
  as ITU-T I.460 and RFC 4040 specify (and as Osmocom and a TDM time
  slot do). The modem reports `CONNECT 64000`, the same rate both ways.
- **Bit order changed in phase 3.** Phases 1 and 2 put the first bit in
  the *least* significant bit. A D-Modem built before phase 3 therefore
  cannot talk X.75, PPP or V.42 on a clear channel to a newer one.
- **No call progress and no answer tone.** `ATD` and `ATA` start the
  pump at once, as on a leased line. The answer delay (S92) does not
  apply either.
- **The protocol on top** is chosen by `AT+B2`: X.75 by default, or
  synchronous PPP, or the modem's own link layer (see
  [AT+B2](#atb2-x75-ppp-or-the-modems-link)).
- **With V.110** (`AT+CBST`, see [below](#atcbst-v110-rate-adaption))
  the start-up is V.110's own instead of the one described next.
- **It needs a digital line at 8000 Hz**, such as SIP or a PBX's
  channel. On an analogue line, `+MS=CLEAR` runs V.34 instead.

### The start-up

The 64 kbit/s start-up is D-Modem's own design:

1. Each end sends HDLC flags (`0x7E`) until it has heard 32 of the
   other end's flags in a row (4 ms).
2. It then sends 64 more flags (8 ms), so that the far end can hear
   them too.
3. It sends 64 octets of ones (`0xFF`, 8 ms).
4. It sends the link layer's bits and reports `CONNECT`.

Everything the far end sends from its ones onwards goes to the link
layer; the flags before them are dropped.

Other ISDN equipment does not send the ones. An X.75 terminal adapter,
for example, sends flags and then goes straight on to its frames (its
SABM). When D-Modem sees something other than a flag or ones after the
flags, it hands the data to the link layer from that point, with a flag
in front for the link's HDLC framer.

If no flags arrive within S7 seconds, the result is `NO CARRIER`.

### How a call ends

Nothing on a clear channel distinguishes a carrier from silence. The
call ends when the line ends it (a SIP BYE, the PBX hanging up), or
when the DTE sends `ATH`.

### The line's G.711 law

The line gives the modem 16 bit samples, so the pump treats each octet
as the G.711 sample it would be:

- **A-law** gives every octet back exactly as it was sent.
- **mu-law** has a single sample for its two zero codes, so `0x7F`
  arrives as `0xFF`.

Hosts that know a call is clear therefore use A-law for it (see the
hosts below). On a mu-law G.711 call where `+MS=CLEAR` was set by hand,
the pump still runs, and its start-up log line notes that `0x7F` will
arrive as `0xFF`.

### A nailed-up channel

`AT&L2+MS=CLEAR` is a nailed-up 64 kbit/s channel, such as a leased DS0
or an E1 time slot. It behaves like the other leased-line modes
([leased.md](leased.md)):

- `ATD` with no number makes the call.
- `S0=1` answers without a ring.
- After `NO CARRIER`, the call starts again by itself.

## AT+B2: X.75, PPP or the modem's link

`AT+B2` chooses the protocol that runs on the clear channel. Its values
are CAPI 2.0's numbers for layer 2 (B2) protocols. Other modes ignore
it.

The isdn4linux commands for the same job (`AT&E`, `ATS14`, `ATS18`)
already mean something else in D-Modem (the CONNECT message's rate,
quiet mode, and the loop test's timer), so they keep those meanings.

| `AT+B2=` | Protocol on the channel | `CONNECT` (with `\V1`) |
|---|---|---|
| `0` (default) | X.75 | `CONNECT 64000/X.75` |
| `5` | synchronous PPP | `CONNECT 64000/PPP` |
| `7` | the modem's own link: V.42, MNP, or plain characters with `\N0` | as for a modem call |

### X.75

`link/lapb.cpp` implements the X.75 single link procedure (SLP, also
known as LAPB) that isdn4linux and the CAPI cards speak. ISDN BBSes and
mailers expected it.

- **Framing:** HDLC frames in basic mode (sequence numbers modulo 8),
  with a window of 7 frames. There is no XID exchange; nothing is
  negotiated.
- **Addresses:** the calling end sends its commands to address B
  (`0x01`) and answers at address A (`0x03`). The called end does the
  opposite.
- **Link set-up:** the calling end sends SABM. The called end waits for
  the caller's SABM; if nothing arrives within 4 × T1, it sends its own
  (as isdn4linux does).
- **Frame sizes:** an I frame carries up to 256 of the DTE's
  characters. Frames of up to 2048 octets of information are accepted
  from the far end.
- **Timers and retries:** T1 is 1 second, plus the time to send one of
  our largest frames and two of the far end's largest. T1 is counted in
  transmitted bits. N2 is 4 tries.
- **Recovery:** lost frames are sent again after a REJ, or after T1
  expires and the link is polled.
- **Release:** `ATH` sends DISC. A DISC or DM from the far end gives
  `NO CARRIER`.
- **Not done yet:** V.42bis compression over X.75, which some ISDN
  adapters offered.

### Synchronous PPP

`link/ppp_sync.cpp` implements PPP over synchronous HDLC (RFC 1618). It
is for a PPP stack on the DTE, such as pppd on the modem's serial port.

- **Towards the DTE**, PPP uses its asynchronous framing (RFC 1662), the
  same as with an analogue modem.
- **From the DTE to the line:** D-Modem removes the escapes from each
  frame, checks and removes its FCS (frame check sequence), and sends
  the frame in an HDLC frame of its own.
- **From the line to the DTE:** each frame is escaped and given an FCS.
  Every octet below `0x20` is escaped, whatever the ACCM (the
  asynchronous control character map) says.
- **Bad frames:** a frame from the DTE with a bad FCS, or one longer than
  2048 octets, is dropped and logged. PPP's own retries cover the loss.
- **Testing:** tried between two D-Modems in
  `dsp/test/clear_call_test.cpp`. It has not yet been tried with pppd at
  each end.

### The modem's link

`AT+B2=7` is phase 1's clear channel. V.42, V.44, V.42bis and MNP run
over it just as over any modulation. With `\N0` it carries plain
asynchronous characters (start bit, 8 data bits, stop bit) on the bit
stream. A D-Modem from before `AT+B2` speaks this.

### MSN selection

An MSN (multiple subscriber number) is one of the numbers on an ISDN
line. Choosing which MSN to answer by AT command (isdn4linux's `AT&E`,
or CAPI's called party number) is not done: every number a line brings
to the modem is taken as the modem's. On an mISDN card, dmodemd's
`--misdn-msn` option picks the MSN instead (see
[below](#dmodemd---misdn-a-bri-card-on-linux)).

## AT+B2=9: V.120

`AT+B2=9` selects V.120 (`link/lapb.cpp`, CAPI's B2 protocol 9), the
protocol of North American ISDN terminal adapters.

- **Addressing:** Q.921's frames, with a two octet address for logical
  link 256 (V.120's default). The calling end takes the user side, so
  its commands have C/R 0 (address `08 01`) and the called end's
  commands have C/R 1 (address `0A 01`).
- **Extended mode:** SABME, sequence numbers modulo 128 in two control
  octets, and a window of 7 frames. Recovery and timers are the same as
  for X.75.
- **Terminal adaption header:** each I frame's information starts with
  V.120's one octet header, `0x83` (asynchronous mode, each frame
  complete). Whatever header the far end sends, including any extension
  octets, is removed.
- **Result codes:** `CONNECT 64000/V.120` with `\V1`. `ATH` sends DISC.
- **Not done:**
  - logical links other than 256, or more than one at a time;
  - UI frames;
  - the header's break and error bits;
  - taking the link's parameters from the call's low layer
    compatibility.

## AT+CBST: V.110 rate adaption

V.110 carries a slower user rate inside a 64 kbit/s channel, in two
steps:

- **RA1:** the user's bits go into 80 bit frames at an intermediate rate
  of 8, 16, 32 or 64 kbit/s.
- **RA2:** that intermediate rate fills the first 1, 2, 4 or 8 bits of
  each octet; the remaining bits are ones.

ISDN terminal adapters used V.110, and GSM's circuit switched data still
travels on it through the network (Osmocom). `dsp/v110.cpp` builds and
synchronises the frames, and `dsp/dp_clear.cpp` runs them.

### The command

`AT+CBST=<speed>[,<name>[,<ce>]]` is 3GPP TS 27.007's bearer service
type command.

| `<speed>` | User rate (bit/s) | Intermediate rate |
|---|---|---|
| `0` (default) | no V.110 (an answerer takes the caller's rate) | |
| `65` | 300 (on 600's frames, each bit twice) | 8 kbit/s |
| `66`, `68`, `70` | 1200, 2400, 4800 | 8 kbit/s |
| `71` | 9600 | 16 kbit/s |
| `75`, `79` | 14400, 19200 | 32 kbit/s |
| `80`, `81` | 28800, 38400 | 64 kbit/s |
| `83` | 56000 (7 bits in each octet, no frames) | 64 kbit/s |

`<name>` chooses how the user's data is carried:

- **`0` (default), asynchronous:** the DTE's characters, with start and
  stop bits, at the user rate. They go through the modem's own link
  whatever `AT+B2` says: `\N0` for plain characters, or V.42 if both
  ends ask for it.
- **`1`, synchronous:** `AT+B2`'s X.75, V.120 or PPP, in HDLC at the
  user rate.

`<ce>` (connection element) has no use here; it accepts 0 or 1.

### The start-up and synchronisation

1. Each end sends frames with the S and X status bits OFF and its data
   bits all ones.
2. It does so until it has frame synchronisation: three good alignment
   patterns in a row.
3. It then sends S and X ON.
4. Once it is in sync and the other end's X is ON, the modem reports
   `CONNECT <rate>` (`CONNECT 9600/V.110` with `\V1` and no V.42).

The user's bits go to the link layer while the other end's S bits are
ON. Three bad alignment patterns in a row lose synchronisation, and the
pump hunts for it again.

At 56000 bit/s there are no frames, so the modem connects at once.

### An answerer that finds the rate itself

An answerer with `AT+CBST=0` looks for the caller's V.110 frames at each
intermediate rate while it waits for D-Modem's flags. The first rate at
which it gets frame sync gives the user rate, read from the frames' E1
to E3 bits.

- It finds every rate from 600 to 38400 bit/s, including 7200, 12000
  and 24000, which have no `<speed>` value of their own.
- 300 bit/s is found as 600 (it uses 600's frames), and 56000 is not
  found at all (it has no frames). For those two rates, set `+CBST` on
  the answerer too.

### Not done

48000 bit/s, and the network independent clocking bits (E4 to E6).

## dmodemd: SIP in CLEARMODE

RFC 4040's CLEARMODE carries the 64 kbit/s clear channel as an RTP
payload (`CLEARMODE/8000`, on a dynamic payload type): the octets as
they are, with no codec. dmodemd (`frontend/dmodemd/sip.cpp`) handles it as
follows.

- **Dialling with `AT+MS=CLEAR`:** the offer lists CLEARMODE first (on
  payload type 97), then PCMA and PCMU. An answer in CLEARMODE is a
  clear call. An answer in PCMA also runs the clear channel (the modem's
  `+MS=CLEAR` asks for it), and works as long as nothing on the way
  transcodes.
- **Incoming calls:** an offer that lists CLEARMODE before every G.711
  format is taken as an ISDN data call. dmodemd answers it in CLEARMODE,
  and the modem runs the clear channel pump whatever `+MS` says. An
  offer with G.711 first gets G.711, as before.
- **No telephone-events:** a CLEARMODE call never uses RFC 4733
  telephone-events, because their tones would be data.
- **Turning it off:** `SIP_CLEARMODE=no` (also `0`, `off` or `false`)
  turns CLEARMODE off; it is then neither offered nor accepted.
- **SRTP:** CLEARMODE goes over SRTP as well (`SIP_SRTP`, see the
  [README](../README.md)).

### Peers

Asterisk has no CLEARMODE codec of its own; `codec_clearmode` (see
[below](#codec_clearmode-clearmode-in-asterisk)) adds one. Other peers
that support it include another dmodemd, an ISDN gateway, and
Osmocom's media gateway.

RFC 4040 has no recovery for lost packets, so run it on a clean
network. X.75 retransmits whatever is lost.

### Test

`frontend/dmodemd/test/clearmode_test.py` plays the far end itself: a SIP peer,
and the far end of the clear channel pump on the call's RTP. It checks
both directions:

- **Out:** dmodemd dials; its offer must list CLEARMODE, PCMA and PCMU
  in that order. The script answers in CLEARMODE, and dmodemd must
  report `CONNECT 64000` and pass a line of text each way.
- **In:** the script calls dmodemd with CLEARMODE first in the offer.
  dmodemd must answer in CLEARMODE with no telephone-event, report
  `CONNECT 64000`, and pass a line of text each way.

With `--srtp` the script runs the same two calls over SRTP.

## dmodemd --misdn: a BRI card on Linux

With `--misdn <dev>`, dmodemd's line is an ISDN basic rate interface
(BRI) through mISDN, the Linux kernel's ISDN stack. Suitable cards
include an HFC-S PCI or USB card, an AVM Fritz!Card PCI, or any other
card with an mISDN driver. dmodemd takes the terminal side of the S0
bus, and handles one call at a time.

```
modprobe hfcpci            # or hfcsusb, avmfritz, ...
dmodemd --misdn 0 --misdn-msn 4711
```

`<dev>` is the device's number or its name (for example `hfc-pci.1`).

dmodemd must start as root, because listing mISDN's devices needs
`CAP_NET_RAW`. It opens the D-channel (the signalling channel) and then
carries on as `nobody`, as it does for its pty.

### Options

| Option | Effect |
|---|---|
| `--misdn <dev>` | the line is this mISDN device, not SIP |
| `--misdn-msn <msn>` | take calls for this MSN only, and send it as our number on calls out (default: any number) |
| `--misdn-noflip` | take the B-channel's octets as the card gives them, without bit reversal |

### Layers

- **Layer 2** (LAPD, including the TEI, the terminal endpoint
  identifier) is the kernel's.
- **Call control** (Q.931) on top of it is dmodemd's own:
  `frontend/dmodemd/q931.cpp`, the user side of ETSI's Euro-ISDN.

### Calls out

- The SETUP carries the whole number, with *sending complete*.
- With `AT+MS=CLEAR` the bearer is *unrestricted digital information*,
  and the clear channel modes above run on it: X.75, V.110, V.120 and
  the rest.
- Any other `+MS` mode makes a 3.1 kHz audio call, and the modem runs as
  it does on any digital line.
- The network picks the B-channel.
- The release cause becomes the result code:

  | Q.931 cause | Result |
  |---|---|
  | 17 (user busy) | `BUSY` |
  | 18 (no user responding), 19 (no answer) | `NO ANSWER` |
  | anything else | `NO CARRIER` |

### Calls in

- A call rings when it is for our MSN (the called number must end with
  the `--misdn-msn` value), or for any number when no MSN is set. A call
  whose SETUP has no called number rings too.
- It needs a bearer of speech, 3.1 kHz audio or unrestricted digital
  information, and a B-channel. Other calls are left to the other
  terminals on the bus.
- `RING` repeats every 5 seconds. The calling number is the caller ID
  (`AT+VCID=1`).
- A call with an unrestricted digital bearer is answered on the clear
  channel.
- While a call is up, another call for us gets `RELEASE COMPLETE` with
  cause 17 (user busy).

### The B-channel

- The B-channel runs in the card's transparent mode (`ISDN_P_B_RAW`).
- Its octets are A-law, **bit reversed**: the cards send an octet's
  least significant bit first, whereas G.711 and the clear channel send
  the most significant bit first. `--misdn-noflip` turns the reversal
  off, for a card that does not need it.
- The modem answers each block of octets with a block of the same size,
  so the B-channel's clock paces the modem.
- The bearer dmodemd sends is always A-law.

### Not supported

Overlap dialling, supplementary services, and PRI (primary rate) cards.

### Testing

**Untested on real hardware.**

- `frontend/dmodemd/test/misdn_test.py` runs two dmodemd on fake cards
  (`frontend/dmodemd/test/fake_misdn.cpp`, loaded with `LD_PRELOAD`), connected
  through the script's own ISDN network. It covers:
  - a clear channel call (X.75, caller ID, text each way);
  - a 3.1 kHz audio call with V.22bis;
  - `BUSY` (cause 17) and `NO ANSWER` (cause 19);
  - the bit-reversed idle A-law octets of the audio call.
- `frontend/dmodemd/test/q931_test.cpp` covers Q.931's states, timers and
  messages.

The bit reversal follows Asterisk's chan_misdn and LCR, but no real card
has confirmed it yet.

## app_dmodem: an ISDN data call through Asterisk

app_dmodem is built only for your own Asterisk
(`-DDMODEM_APP_DMODEM=ON`), and a built copy is not passed on: see
[LICENSING.md](../LICENSING.md). dmodem-ari does not take clear channel
calls yet.

A call that reaches `DModem()` with an unrestricted digital bearer runs
on the clear channel. That is the case when
`${CHANNEL(transfercapability)}` is `DIGITAL`, as chan_dahdi sets it for
an ISDN data call on a BRI or PRI span. The module reads and writes the
channel in the channel's own G.711 format, so the octets come through
unchanged, and the modem takes them as clear.

The profile's `clear` option (in `dmodem.conf`) decides:

| `clear` | Effect |
|---|---|
| `auto` (default) | clear for a call with a digital bearer |
| `yes` | every call is clear |
| `no` | never: digital calls run as G.711 audio, as before |

FreeSWITCH's `mod_dmodem` has the same option through the shared bridge
(`frontend/pbx/dmodem_bridge.c`). There, `auto` behaves like `no`, because
FreeSWITCH does not tell the module the bearer capability.

**Not yet tried on real ISDN hardware.** chan_dahdi is expected to hand a
digital call's octets to the application unchanged (with its audio mode
off, and no echo canceller or gain), but this needs confirming on a
card. The restricted digital bearer (56 kbit/s, North America) is not
taken as clear.

## codec_clearmode: CLEARMODE in Asterisk

`frontend/asterisk/codec_clearmode.c` adds RFC 4040's codec to Asterisk.

- **Build:** it is built and installed whenever Asterisk's headers are
  found. It contains no D-Modem code and is under Asterisk's
  GPL-2.0-only licence, so it can be shipped like any other module.
- **What it registers:** the `clearmode` codec (8000 octets a second,
  one sample per octet, frames of 10 to 150 ms) and its format, mapped
  to `CLEARMODE/8000` in RTP. It uses only Asterisk's public API.
- **No translator:** a clear call's octets are data, so a CLEARMODE
  channel bridges only with another CLEARMODE channel, or goes to
  `DModem()`.

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
profile's `clear` option says (with `clear = no`, a notice is logged).
The modem runs `AT+B2`'s protocol on it.

`frontend/asterisk/test/sip.sh` with `CODEC=clearmode` runs dmodemd's X.75 call
through Asterisk in CLEARMODE, and checks that it connected at
64000/64000 bit/s.

The idea comes from a gist by 7marcus9; this module was written anew
for D-Modem. dmodem-ari does not take CLEARMODE, because ARI's external
media supports G.711 only.

## libdmodem

For a program that embeds the modem:

- `dmodem_config`'s `clear` field makes the line clear for every call.
  Every call then runs the clear channel pump, and
  `dmodem_process_g711()` takes the octets as they are.
- `dmodem_set_clear()`, called before each call, does the same per call,
  for a program that learns the bearer call by call.

### Tests

`dsp/test/clear_call_test.cpp` runs two modems on a simulated clear
channel. It checks:

- `CONNECT 64000`, then 200 kB each way through X.75 (the default
  `+B2`);
- every octet value, without V.42 (`+B2=7`, `\N0`);
- the clear line set by the program, and per call;
- a late answerer, no answerer, and a nailed-up channel;
- X.75, V.120 and PPP, each checked frame by frame against a decoder of
  the test's own, with lost audio and a release for X.75 and V.120;
- V.110 at each `+CBST` rate, with its frames checked the same way, and
  the rate found by the answerer;
- V.42 over V.110, and X.75 and V.120 over synchronous V.110;
- another device's HDLC start, with no ones after its flags.

`dsp/test/v110_osmo_test.cpp` runs D-Modem's V.110 against
libosmocore's, the V.110 code that GSM's circuit switched data uses in
Osmocom:

- **Osmocom calls:** libosmocore sends at each of its user rates, and
  D-Modem, answering with `+CBST=0`, must find the rate and exchange
  text.
- **D-Modem calls:** D-Modem calls at each `+CBST` rate that
  libosmocore also has, and libosmocore must accept its frames (it
  refuses frames with the wrong E1 to E3 bits).

The test is built when CMake's `OSMOCORE_SRC` names a libosmocore source
tree (1.14 or later). Only its `src/isdn/v110.c` is compiled, into the
test. CI's build job clones libosmocore 1.14.2 for it. All twelve of
libosmocore's rates, 600 to 38400 bit/s, pass with Osmocom calling; the
eight of them that `+CBST` has pass with D-Modem calling.

## CAPI 2.0: dmodem-capid

`dmodem-capid` (`frontend/capi/`) makes a D-Modem a CAPI 2.0 controller. CAPI
(Common ISDN API) is the programming interface that much ISDN software
was written for: capi4hylafax, chan_capi, pppd's capiplugin, and the BBS
and mailer software of the isdn4linux years.

It is a separate program that drives the modem through its DTE with AT
commands. It therefore works with every line D-Modem has: SIP (with
CLEARMODE for data calls), an Asterisk channel, an mISDN card, and any
line added later.

Applications reach it over TCP with libcapi20's remote CAPI module
(`rcapi`, the protocol of isdn4k-utils' `rcapid`). They run unchanged,
on the same machine or another one.

```
dmodemd ...                              # its pty: /dev/ttyDM0 (or --tcp)
dmodem-capid -l 6000 /dev/ttyDM0         # or tcp:127.0.0.1:2323

# /etc/capi20.conf where the applications run
REMOTE rcapi localhost 6000
```

### Options

```
dmodem-capid [-l [addr:]port] [-i commands] <modem>
```

- `<modem>` is the modem's DTE: dmodemd's pty, a serial port, or
  `tcp:host:port`.
- `-l` is where the applications connect. The default is port 6000 on
  every address.
- `-i` adds AT commands after the controller's own set-up, which is
  `ATE0V1Q0X3+CRC=1;+VCID=1S0=0`.

### Messages

It is one controller (number 1) with one B-channel, so one call at a
time. The call's PLCI (physical link connection identifier) is `0x0101`
and its NCCI (network control connection identifier) is `0x00010101`.

- **LISTEN_REQ:** the application's CIP mask (the call types it wants)
  is kept. Each ring is offered (CONNECT_IND) to every application
  whose mask takes it:
  - `+CRING: SYNC`, the modem's ring for a clear channel call (see
    `AT+CRC` below), is offered as CIP 2 (unrestricted digital
    information);
  - any other ring is offered as the first of CIP 4 (3.1 kHz audio),
    16 (telephony) and 1 (speech) that the mask has.

  The caller's number comes from `AT+VCID`. The called number is empty,
  as there is no MSN selection yet.
- **CONNECT_REQ:** the B protocol is set with AT commands (see the table
  below), then `ATD` dials the called number. The modem's `CONNECT`
  becomes CONNECT_ACTIVE_IND. The application's CONNECT_B3_REQ then
  gives CONNECT_B3_ACTIVE_IND.
- **CONNECT_RESP:** the first application to accept the call gets it.
  The others get DISCONNECT_IND with reason `0x3304`. The B protocol is
  set, then `ATA` answers. The modem's `CONNECT` becomes
  CONNECT_ACTIVE_IND and CONNECT_B3_IND.
  - A call that nobody accepts keeps ringing.
  - When the rings stop for 10 seconds, every application it was
    offered to gets DISCONNECT_IND.
- **DATA_B3:** the application's data goes to the modem unchanged, and
  its DATA_B3_CONF comes once the modem has taken it. The modem's data
  arrives in DATA_B3_INDs of up to the application's maxBDataLen, with
  as many outstanding as its maxBDataBlocks (7 at most).
- **DISCONNECT_REQ:** the controller sends `+++` between guard times
  (D-Modem's guard time is 500 ms), then `ATH`. DISCONNECT_B3_IND and
  DISCONNECT_IND (reason 0) follow.
- **The modem's result codes:**

  | Modem | DISCONNECT_IND reason |
  |---|---|
  | `NO CARRIER` | `0x3490` (normal clearing), once the data before it has reached the application |
  | `BUSY` | `0x3491` |
  | `NO ANSWER` | `0x3493` |

- **Other messages:**
  - FACILITY_REQ answers the supplementary services'
    GetSupportedServices (with none), and anything else with "facility
    not supported".
  - ALERT_REQ and INFO_REQ are confirmed, and do nothing.
  - SELECT_B_PROTOCOL and RESET_B3 are refused.

### B protocols

CAPI describes a call's protocol stack as B1 (layer 1), B2 (layer 2)
and B3 (layer 3). The controller turns each supported combination into
AT commands:

| B1 | B2 | B3 | AT commands |
|---|---|---|---|
| 0 (64 kbit/s HDLC) | 0 (X.75) | 0 | `+MS=CLEAR;+CBST=0;+B2=0` |
| 0 | 1 (transparent) | 0 | `+MS=CLEAR;+CBST=0;+B2=5`: each DATA_B3 a whole frame |
| 0 | 9 (V.120 async) | 0 | `+MS=CLEAR;+CBST=0;+B2=9` |
| 2 (V.110 async) | 1 | 0 | `\N0+MS=CLEAR;+CBST=<rate>,0;+B2=7` |
| 3 (V.110 sync) | 0, 1, 9 | 0 | `+MS=CLEAR;+CBST=<rate>,1;` and B2 as above |
| 7 (modem, negotiated) | 7 (V.42, MNP) | 0, 7 | `\N3` and the modem's own `+MS` |
| 7, 8 (modem) | 1 | 0 | `\N0` and the modem's own `+MS` |

- An empty B protocol is CAPI's default, X.75.
- The V.110 rate is the first word of B1's configuration: 300, 1200,
  2400, 4800, 9600, 14400, 19200, 28800, 38400 or 56000, or 0 for the
  caller's rate. Any other rate is refused.
- **B1 0 with B2 1** is what capiplugin asks for. The controller wraps
  each DATA_B3 in a frame with RFC 1662's framing for the modem's sync
  PPP (`+B2=5`), and turns each frame back into a DATA_B3.
- **Refused** with CAPI's "not supported" info: other protocols, such
  as fax, B1 1 (the bit-transparent channel) and X.25.
- **Modem calls (B1 7 or 8)** use the modem's own modulation. The
  controller reads `AT+MS?` at start-up, and sets it again (with
  `+CBST=0`) for each such call.

### AT+CRC

`AT+CRC=1` (from 27.007) is the one addition to the modem for CAPI. With
it, a ring is reported as `+CRING: SYNC` for a call on a clear channel,
and as `+CRING: VOICE` for any other call, instead of `RING`. With an
older D-Modem that refuses `+CRC`, the controller drops it from its
set-up, takes each plain `RING`, and offers every call as an audio call.

### Limits

- One call at a time.
- While online, the data is watched for the modem's `NO CARRIER` line,
  so that line inside the data would end the call.
- No fax (CAPI's B1 4) yet.
- No Windows `capi2032.dll` yet.

libdmodem's `dmodem_ring()` rings a modem, for a program that learns of
calls itself; `frontend/capi/test/capi_test.cpp` uses it.

### Tests

`frontend/capi/test/capi_test.cpp` runs the controller on one of two libdmodem
modems on a simulated line, with an application of the test's own. It
covers:

- the mapping of B protocols to AT commands;
- calls out over X.75, V.120, frames (sync PPP), V.110 at 9600 bit/s,
  and a modem call (V.42 over V.90 or V.91), with data both ways;
- a call in on a clear channel with the caller's number, and a second
  listening application that loses it;
- hangups from either end, a call that nobody answers, a call that
  stops ringing, and a B protocol the controller lacks;
- the TCP server in rcapi's framing, against a modem of the test's own.

### Tried with libcapi20

With libcapi20 from isdn4k-utils (its `rcapi` module):

- `capiinfo` lists the controller and its profile.
- An application of libcapi20's called a libdmodem modem through
  `dmodem-capid` with X.75, frames, V.120, V.110 at 9600 bit/s and a
  modem call (V.42 over V.90). Its data was echoed back byte for byte,
  and it then hung up.

Not yet tried: an incoming call through libcapi20, and the applications
themselves (capi4hylafax, chan_capi, capiplugin).

## What comes next

The feasibility study's later phases build on this line mode:

1. **Done:** X.75 and sync PPP (`AT+B2`), and `codec_clearmode`.
   **Still to come:** V.42bis over X.75, and MSN selection.
2. **Done:** V.110 (`AT+CBST`), V.120 (`AT+B2=9`), and mISDN for BRI
   cards on Linux (`--misdn`), which still needs a test on a real card.
3. **CAPI 2.0:** done for data and modem calls (`dmodem-capid`, above).
   **Still to come:** fax over CAPI, and a Windows `capi2032.dll`.
   Hardware (an S0 USB stick) is being designed.
