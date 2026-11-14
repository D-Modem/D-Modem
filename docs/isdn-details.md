# ISDN: the clear 64 kbit/s channel: technical details

This page gives the technical details for [ISDN: the clear 64 kbit/s channel](isdn.md), the plain-English overview. Read the overview first. This page keeps the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

An ISDN B-channel (bearer channel) carries 64 kbit/s: 8000 octets a
second in each direction. There are two types of call on it:

- A **voice call** puts G.711 audio in those octets. All D-Modem modes
  already operate on that. The design of V.90 and V.92 is for a server on
  exactly such a line.
- A **data call** puts data directly into the octets, with no audio.
  This is the *clear channel*. This page is about the clear channel.

ISDN support came in three phases:

| Phase | What it added |
|---|---|
| 1 | A clear 64 kbit/s line mode, and two ways for a call to reach it: SIP in CLEARMODE, and an Asterisk channel with a digital bearer. |
| 2 | The B-channel protocols of ISDN terminal adapters: X.75 and synchronous PPP (`AT+B2`), and a CLEARMODE codec for Asterisk (`codec_clearmode`). |
| 3 | V.110 rate adaption (`AT+CBST`), V.120 (`AT+B2=9`), and CAPI 2.0 for ISDN applications (`dmodem-capid`). |

## AT+MS=CLEAR

`AT+MS=CLEAR` (or `AT+MS=64`) selects the clear channel pump,
`dsp/dp_clear.cpp`.

### What the pump does

- **No modulation.** Each octet of the channel carries 8 bits of the link
  layer. The first bit goes in the most significant bit of the octet,
  as ITU-T I.460 and RFC 4040 specify (and as Osmocom and a TDM time
  slot do). The modem reports `CONNECT 64000`, the same rate in the two directions.
- **Bit order changed in phase 3.** Phases 1 and 2 put the first bit in
  the *least* significant bit. Thus, a D-Modem built before phase 3
  cannot use X.75, PPP or V.42 on a clear channel with a newer D-Modem.
- **No call progress and no answer tone.** `ATD` and `ATA` start the
  pump immediately, as on a leased line. Also, the answer delay (S92) does not
  apply.
- **`AT+B2` selects the protocol on top**: X.75 by default, or
  synchronous PPP, or the link layer of the modem itself (refer to
  [AT+B2](#atb2-x75-ppp-or-the-modems-link)).
- **With V.110** (`AT+CBST`, refer to [below](#atcbst-v110-rate-adaption)),
  the start-up is the start-up of V.110, not the start-up in the subsequent section.
- **It must have a digital line at 8000 Hz**, for example SIP or the channel of a
  PBX. On an analogue line, `+MS=CLEAR` runs V.34 instead.

### The start-up

The 64 kbit/s start-up is a design of D-Modem:

1. Each end sends HDLC flags (`0x7E`) until it has heard 32 flags of the
   other end in sequence (4 ms).
2. Then it sends 64 more flags (8 ms), so that the far end can also hear
   them.
3. It sends 64 octets of ones (`0xFF`, 8 ms).
4. It sends the bits of the link layer and reports `CONNECT`.

All the data that the far end sends from its ones goes to the link
layer. The modem removes the flags before the ones.

Other ISDN equipment does not send the ones. For example, an X.75 terminal adapter
sends flags and then continues immediately with its frames (its
SABM). When D-Modem sees an item that is not a flag or ones after the
flags, it gives the data to the link layer from that point, with a flag
in front for the HDLC framer of the link.

If no flags arrive in S7 seconds, the result is `NO CARRIER`.

### How a call ends

On a clear channel, nothing shows the difference between a carrier and silence. The
call stops when the line stops it (a SIP BYE, the PBX hangs up), or
when the DTE sends `ATH`.

### The line's G.711 law

The line gives 16 bit samples to the modem. Thus, the pump uses each octet
as the equivalent G.711 sample:

- **A-law** gives each octet back exactly as it was sent.
- **mu-law** has one sample for its two zero codes. Thus, `0x7F`
  arrives as `0xFF`.

Thus, hosts that know that a call is clear use A-law for it (refer to the
hosts below). On a mu-law G.711 call where you set `+MS=CLEAR` manually,
the pump still runs, and its start-up log line tells that `0x7F` will
arrive as `0xFF`.

### A nailed-up channel

`AT&L2+MS=CLEAR` is a nailed-up 64 kbit/s channel, such as a leased DS0
or an E1 time slot. It operates as the other leased-line modes do
([leased.md](leased.md)):

- `ATD` with no number makes the call.
- `S0=1` answers without a ring.
- After `NO CARRIER`, the call starts again automatically.

## AT+B2: X.75, PPP or the modem's link

`AT+B2` selects the protocol that runs on the clear channel. Its values
are the numbers of CAPI 2.0 for layer 2 (B2) protocols. Other modes ignore
it.

The isdn4linux commands for the same task (`AT&E`, `ATS14`, `ATS18`)
already have a different meaning in D-Modem (the rate of the CONNECT message,
quiet mode, and the timer of the loop test). Thus, they keep those meanings.

| `AT+B2=` | Protocol on the channel | `CONNECT` (with `\V1`) |
|---|---|---|
| `0` (default) | X.75 | `CONNECT 64000/X.75` |
| `5` | synchronous PPP | `CONNECT 64000/PPP` |
| `7` | the modem's own link: V.42, MNP, or plain characters with `\N0` | as for a modem call |

### X.75

`link/lapb.cpp` implements the X.75 single link procedure (SLP, also
known as LAPB) that isdn4linux and the CAPI cards use. ISDN BBSes and
mailers expected it.

- **Framing:** HDLC frames in basic mode (sequence numbers modulo 8),
  with a window of 7 frames. There is no XID exchange. Nothing is
  negotiated.
- **Addresses:** the calling end sends its commands to address B
  (`0x01`) and answers at address A (`0x03`). The called end does the
  opposite.
- **Link set-up:** the calling end sends SABM. The called end waits for
  the SABM of the caller. If nothing arrives in 4 × T1, it sends its own SABM
  (as isdn4linux does).
- **Frame sizes:** an I frame carries up to 256 characters of the DTE.
  The modem accepts frames of up to 2048 octets of information
  from the far end.
- **Timers and retries:** T1 is 1 second, plus the time to send one of
  our largest frames and two of the largest frames of the far end. T1 is counted in
  transmitted bits. N2 is 4 tries.
- **Recovery:** the modem sends lost frames again after a REJ, or after T1
  expires and the modem polls the link.
- **Release:** `ATH` sends DISC. A DISC or DM from the far end gives
  `NO CARRIER`.
- **Not done yet:** V.42bis compression over X.75, which some ISDN
  adapters offered.

### Synchronous PPP

`link/ppp_sync.cpp` implements PPP over synchronous HDLC (RFC 1618). It
is for a PPP stack on the DTE, for example pppd on the serial port of the modem.

- **Toward the DTE**, PPP uses its asynchronous framing (RFC 1662), the
  same as with an analogue modem.
- **From the DTE to the line:** D-Modem removes the escapes from each
  frame, checks and removes its FCS (frame check sequence), and sends
  the frame in its own HDLC frame.
- **From the line to the DTE:** D-Modem escapes each frame and gives it an FCS.
  It escapes each octet below `0x20`, independently of the ACCM (the
  asynchronous control character map).
- **Bad frames:** D-Modem removes and logs a frame from the DTE with a bad FCS, or a frame longer than
  2048 octets. The retries of PPP itself recover the loss.
- **Testing:** we tried it between two D-Modems in
  `dsp/test/clear_call_test.cpp`. We did not try it yet with pppd at
  each end.

### The modem's link

`AT+B2=7` is the clear channel of phase 1. V.42, V.44, V.42bis and MNP run
over it as over any modulation. With `\N0`, it carries plain
asynchronous characters (start bit, 8 data bits, stop bit) on the bit
stream. A D-Modem from before `AT+B2` uses this.

### MSN selection

An MSN (multiple subscriber number) is one of the numbers on an ISDN
line. The selection of the MSN to answer by AT command (the `AT&E` of isdn4linux,
or the called party number of CAPI) is not done. The modem accepts each number that a line brings
to it as its own number. On an mISDN card, the
`--misdn-msn` option of dmodemd selects the MSN instead (refer to
[below](#dmodemd---misdn-a-bri-card-on-linux)).

## AT+B2=9: V.120

`AT+B2=9` selects V.120 (`link/lapb.cpp`, B2 protocol 9 of CAPI), the
protocol of North American ISDN terminal adapters.

- **Addressing:** the frames of Q.921, with a two octet address for logical
  link 256 (the default of V.120). The calling end takes the user side. Thus,
  its commands have C/R 0 (address `08 01`), and the commands of the called end
  have C/R 1 (address `0A 01`).
- **Extended mode:** SABME, sequence numbers modulo 128 in two control
  octets, and a window of 7 frames. Recovery and timers are the same as
  for X.75.
- **Terminal adaption header:** the information of each I frame starts with
  the one octet header of V.120, `0x83` (asynchronous mode, each frame
  complete). The modem removes each header that the far end sends, with its extension
  octets.
- **Result codes:** `CONNECT 64000/V.120` with `\V1`. `ATH` sends DISC.
- **Not done:**
  - logical links other than 256, or more than one at a time;
  - UI frames;
  - the break and error bits of the header;
  - the parameters of the link from the low layer
    compatibility of the call.

## AT+CBST: V.110 rate adaption

V.110 carries a slower user rate inside a 64 kbit/s channel, in two
steps:

- **RA1:** the bits of the user go into 80 bit frames at an intermediate rate
  of 8, 16, 32 or 64 kbit/s.
- **RA2:** that intermediate rate fills the first 1, 2, 4 or 8 bits of
  each octet. The remaining bits are ones.

ISDN terminal adapters used V.110, and the circuit switched data of GSM still
goes on it through the network (Osmocom). `dsp/v110.cpp` builds and
synchronises the frames, and `dsp/dp_clear.cpp` runs them.

### The command

`AT+CBST=<speed>[,<name>[,<ce>]]` is the bearer service
type command of 3GPP TS 27.007.

| `<speed>` | User rate (bit/s) | Intermediate rate |
|---|---|---|
| `0` (default) | no V.110 (an answerer takes the caller's rate) | |
| `65` | 300 (on 600's frames, each bit twice) | 8 kbit/s |
| `66`, `68`, `70` | 1200, 2400, 4800 | 8 kbit/s |
| `71` | 9600 | 16 kbit/s |
| `75`, `79` | 14400, 19200 | 32 kbit/s |
| `80`, `81` | 28800, 38400 | 64 kbit/s |
| `83` | 56000 (7 bits in each octet, no frames) | 64 kbit/s |

`<name>` selects how the data of the user goes:

- **`0` (default), asynchronous:** the characters of the DTE, with start and
  stop bits, at the user rate. They go through the link of the modem itself,
  independently of `AT+B2`: `\N0` for plain characters, or V.42 if the two
  ends ask for it.
- **`1`, synchronous:** the X.75, V.120 or PPP of `AT+B2`, in HDLC at the
  user rate.

`<ce>` (connection element) has no function here. It accepts 0 or 1.

### The start-up and synchronisation

1. Each end sends frames with the S and X status bits OFF and all its data
   bits ones.
2. It does this until it has frame synchronisation: three good alignment
   patterns in sequence.
3. Then it sends S and X ON.
4. When it is in sync and the X of the other end is ON, the modem reports
   `CONNECT <rate>` (`CONNECT 9600/V.110` with `\V1` and no V.42).

The bits of the user go to the link layer while the S bits of the other end are
ON. After three bad alignment patterns in sequence, the pump loses synchronisation and
looks for it again.

At 56000 bit/s, there are no frames. Thus, the modem connects immediately.

### An answerer that finds the rate itself

An answerer with `AT+CBST=0` looks for the V.110 frames of the caller at each
intermediate rate while it waits for the flags of D-Modem. The first rate at
which it gets frame sync gives the user rate. The answerer reads it from the E1
to E3 bits of the frames.

- It finds all rates from 600 to 38400 bit/s, which include 7200, 12000
  and 24000. These rates have no `<speed>` value of their own.
- It finds 300 bit/s as 600 (300 uses the frames of 600), and it does not find
  56000 (56000 has no frames). For those two rates, also set `+CBST` on
  the answerer.

### Not done

48000 bit/s, and the network independent clocking bits (E4 to E6).

## dmodemd: SIP in CLEARMODE

The CLEARMODE of RFC 4040 carries the 64 kbit/s clear channel as an RTP
payload (`CLEARMODE/8000`, on a dynamic payload type): the octets without changes,
with no codec. dmodemd (`frontend/dmodemd/sip.cpp`) uses it as
follows.

- **Dialling with `AT+MS=CLEAR`:** the offer gives CLEARMODE first (on
  payload type 97), then PCMA and PCMU. An answer in CLEARMODE is a
  clear call. An answer in PCMA also runs the clear channel (the
  `+MS=CLEAR` of the modem asks for it). This operates if nothing on the path
  transcodes.
- **Incoming calls:** dmodemd accepts an offer that gives CLEARMODE before all G.711
  formats as an ISDN data call. dmodemd answers it in CLEARMODE,
  and the modem runs the clear channel pump, independently of `+MS`. An
  offer with G.711 first gets G.711, as before.
- **No telephone-events:** a CLEARMODE call never uses RFC 4733
  telephone-events, because their tones would be data.
- **Turning it off:** `SIP_CLEARMODE=no` (also `0`, `off` or `false`)
  disables CLEARMODE. Then dmodemd does not offer or accept it.
- **SRTP:** CLEARMODE also goes over SRTP (`SIP_SRTP`, refer to
  [Using dmodemd](usage.md#media-v152-and-srtp)).

### Peers

Asterisk has no CLEARMODE codec of its own. `codec_clearmode` (refer to
[below](#codec_clearmode-clearmode-in-asterisk)) adds one. Other peers
that support it include a different dmodemd, an ISDN gateway, and
the media gateway of Osmocom.

RFC 4040 has no recovery for lost packets. Thus, use it on a clean
network. X.75 sends all lost data again.

### Test

`frontend/dmodemd/test/clearmode_test.py` is the far end itself: a SIP peer,
and the far end of the clear channel pump on the RTP of the call. It checks
the two directions:

- **Out:** dmodemd dials; its offer must list CLEARMODE, PCMA and PCMU
  in that order. The script answers in CLEARMODE, and dmodemd must
  report `CONNECT 64000` and pass a line of text in each direction.
- **In:** the script calls dmodemd with CLEARMODE first in the offer.
  dmodemd must answer in CLEARMODE with no telephone-event, report
  `CONNECT 64000`, and pass a line of text in each direction.

With `--srtp`, the script runs the same two calls over SRTP.

## dmodemd --misdn: a BRI card on Linux

With `--misdn <dev>`, the line of dmodemd is an ISDN basic rate interface
(BRI) through mISDN, the ISDN stack of the Linux kernel. Applicable cards
include an HFC-S PCI or USB card, an AVM Fritz!Card PCI, or any other
card with an mISDN driver. dmodemd takes the terminal side of the S0
bus, and accepts one call at a time.

```
modprobe hfcpci            # or hfcsusb, avmfritz, ...
dmodemd --misdn 0 --misdn-msn 4711
```

`<dev>` is the number or the name of the device (for example `hfc-pci.1`).

dmodemd must start as root, because the list of the devices of mISDN must have
`CAP_NET_RAW`. It opens the D-channel (the signalling channel) and then
continues as `nobody`, as it does for its pty.

### Options

| Option | Effect |
|---|---|
| `--misdn <dev>` | the line is this mISDN device, not SIP |
| `--misdn-msn <msn>` | take calls for this MSN only, and send it as our number on calls out (default: any number) |
| `--misdn-noflip` | take the B-channel's octets as the card gives them, without bit reversal |

### Layers

- **Layer 2** (LAPD, with the TEI, the terminal endpoint
  identifier) comes from the kernel.
- **Call control** (Q.931) on top of it is the code of dmodemd:
  `frontend/dmodemd/q931.cpp`, the user side of the Euro-ISDN of ETSI.

### Calls out

- The SETUP carries the full number, with *sending complete*.
- With `AT+MS=CLEAR`, the bearer is *unrestricted digital information*,
  and the clear channel modes above run on it: X.75, V.110, V.120 and
  the others.
- All other `+MS` modes make a 3.1 kHz audio call, and the modem runs as
  it does on any digital line.
- The network selects the B-channel.
- The release cause becomes the result code:

  | Q.931 cause | Result |
  |---|---|
  | 17 (user busy) | `BUSY` |
  | 18 (no user responding), 19 (no answer) | `NO ANSWER` |
  | anything else | `NO CARRIER` |

### Calls in

- A call rings when it is for our MSN (the called number must end with
  the `--misdn-msn` value), or for any number when no MSN is set. A call
  whose SETUP has no called number also rings.
- It must have a bearer of speech, 3.1 kHz audio or unrestricted digital
  information, and a B-channel. dmodemd leaves other calls to the other
  terminals on the bus.
- `RING` repeats each 5 seconds. The calling number is the caller ID
  (`AT+VCID=1`).
- A call with an unrestricted digital bearer is answered on the clear
  channel.
- While a call is up, another call for us gets `RELEASE COMPLETE` with
  cause 17 (user busy).

### The B-channel

- The B-channel runs in the transparent mode of the card (`ISDN_P_B_RAW`).
- Its octets are A-law, **bit reversed**: the cards send the least
  significant bit of an octet first, but G.711 and the clear channel send
  the most significant bit first. `--misdn-noflip` disables the reversal,
  for a card that does not need it.
- The modem answers each block of octets with a block of the same size.
  Thus, the clock of the B-channel paces the modem.
- The bearer dmodemd sends is always A-law.

### Not supported

Overlap dialling, supplementary services, and PRI (primary rate) cards.

### Testing

**Untested on real hardware.**

- `frontend/dmodemd/test/misdn_test.py` runs two dmodemd on fake cards
  (`frontend/dmodemd/test/fake_misdn.cpp`, loaded with `LD_PRELOAD`), connected
  through the ISDN network of the script. It covers:
  - a clear channel call (X.75, caller ID, text in each direction);
  - a 3.1 kHz audio call with V.22bis;
  - `BUSY` (cause 17) and `NO ANSWER` (cause 19);
  - the bit-reversed idle A-law octets of the audio call.
- `frontend/dmodemd/test/q931_test.cpp` covers Q.931's states, timers and
  messages.

The bit reversal is the same as in the chan_misdn of Asterisk and in LCR, but no real card
has confirmed it yet.

## app_dmodem: an ISDN data call through Asterisk

You build app_dmodem only for your own Asterisk
(`-DDMODEM_APP_DMODEM=ON`). Do not give a built copy to other persons: refer to
[LICENSING.md](../LICENSING.md). dmodem-ari does not accept clear channel
calls yet.

A call that comes to `DModem()` with an unrestricted digital bearer runs
on the clear channel. This occurs when
`${CHANNEL(transfercapability)}` is `DIGITAL`, as chan_dahdi sets it for
an ISDN data call on a BRI or PRI span. The module reads and writes the
channel in the G.711 format of the channel itself. Thus, the octets come through
without changes, and the modem accepts them as clear.

The `clear` option of the profile (in `dmodem.conf`) sets the behavior:

| `clear` | Effect |
|---|---|
| `auto` (default) | clear for a call with a digital bearer |
| `yes` | every call is clear |
| `no` | never: digital calls run as G.711 audio, as before |

The `mod_dmodem` of FreeSWITCH has the same option through the shared bridge
(`frontend/pbx/dmodem_bridge.c`). There, `auto` operates as `no`, because
FreeSWITCH does not tell the bearer capability to the module.

**Not yet tried on real ISDN hardware.** We expect that chan_dahdi gives the
octets of a digital call to the application without changes (with its audio mode
disabled, and no echo canceller or gain), but we must confirm this on a
card. The module does not accept the restricted digital bearer (56 kbit/s, North America)
as clear.

## codec_clearmode: CLEARMODE in Asterisk

`frontend/asterisk/codec_clearmode.c` adds RFC 4040's codec to Asterisk.

- **Build:** the build builds and installs it each time it finds the headers of Asterisk.
  It contains no D-Modem code and is under the
  GPL-2.0-only licence of Asterisk. Thus, you can supply it as any other module.
- **What it registers:** the `clearmode` codec (8000 octets each second,
  one sample for each octet, frames of 10 to 150 ms) and its format, mapped
  to `CLEARMODE/8000` in RTP. It uses only the public API of Asterisk.
- **No translator:** the octets of a clear call are data. Thus, a CLEARMODE
  channel bridges only with a different CLEARMODE channel, or goes to
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

A call that comes to `DModem()` in CLEARMODE is clear, independently of the
`clear` option of the profile (with `clear = no`, the module logs a notice).
The modem runs the protocol of `AT+B2` on it.

`frontend/asterisk/test/sip.sh` with `CODEC=clearmode` runs the X.75 call of dmodemd
through Asterisk in CLEARMODE, and makes sure that it connected at
64000/64000 bit/s.

The idea comes from a gist by 7marcus9. We wrote this module again
for D-Modem. dmodem-ari does not accept CLEARMODE, because the external
media of ARI supports only G.711.

## libdmodem

For a program that embeds the modem:

- The `clear` field of `dmodem_config` makes the line clear for all calls.
  Then all calls run the clear channel pump, and
  `dmodem_process_g711()` accepts the octets without changes.
- `dmodem_set_clear()`, called before each call, does the same for each call,
  for a program that learns the bearer call by call.

### Tests

`dsp/test/clear_call_test.cpp` runs two modems on a simulated clear
channel. It checks:

- `CONNECT 64000`, then 200 kB in each direction through X.75 (the default
  `+B2`);
- all octet values, without V.42 (`+B2=7`, `\N0`);
- the clear line set by the program, and for each call;
- a late answerer, no answerer, and a nailed-up channel;
- X.75, V.120 and PPP, each checked frame by frame against a decoder of
  the test, with lost audio and a release for X.75 and V.120;
- V.110 at each `+CBST` rate, with its frames checked in the same method, and
  the rate found by the answerer;
- V.42 over V.110, and X.75 and V.120 over synchronous V.110;
- the HDLC start of a different device, with no ones after its flags.

`dsp/test/v110_osmo_test.cpp` runs the V.110 of D-Modem against
the V.110 of libosmocore, the V.110 code that the circuit switched data of GSM uses in
Osmocom:

- **Osmocom calls:** libosmocore sends at each of its user rates.
  D-Modem answers with `+CBST=0`, and must find the rate and exchange
  text.
- **D-Modem calls:** D-Modem calls at each `+CBST` rate that
  libosmocore also has. libosmocore must accept its frames (it
  refuses frames with incorrect E1 to E3 bits).

The test is built when the `OSMOCORE_SRC` of CMake gives a libosmocore source
tree (1.14 or later). Only its `src/isdn/v110.c` is compiled, into the
test. The build job of CI clones libosmocore 1.14.2 for it. All twelve
rates of libosmocore, 600 to 38400 bit/s, pass when Osmocom calls. The
eight of them that `+CBST` has pass when D-Modem calls.

## CAPI 2.0: dmodem-capid

`dmodem-capid` (`frontend/capi/`) makes a D-Modem a CAPI 2.0 controller. CAPI
(Common ISDN API) is the programming interface of much ISDN software:
capi4hylafax, chan_capi, the capiplugin of pppd, and the BBS
and mailer software of the isdn4linux years.

It is a separate program that operates the modem through its DTE with AT
commands. Thus, it operates with all lines that D-Modem has: SIP (with
CLEARMODE for data calls), an Asterisk channel, an mISDN card, and any
line that we add later.

Applications connect to it over TCP with the remote CAPI module of libcapi20
(`rcapi`, the protocol of the `rcapid` of isdn4k-utils). They run without changes,
on the same machine or on a different machine.

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

- `<modem>` is the DTE of the modem: the pty of dmodemd, a serial port, or
  `tcp:host:port`.
- `-l` is the address where the applications connect. The default is port 6000 on
  all addresses.
- `-i` adds AT commands after the set-up of the controller itself, which is
  `ATE0V1Q0X3+CRC=1;+VCID=1S0=0`.

### Messages

It is one controller (number 1) with one B-channel. Thus, it accepts one call at a
time. The PLCI (physical link connection identifier) of the call is `0x0101`,
and its NCCI (network control connection identifier) is `0x00010101`.

- **LISTEN_REQ:** the controller keeps the CIP mask of the application (the call types that it wants).
  It offers each ring (CONNECT_IND) to each application
  whose mask accepts it:
  - It offers `+CRING: SYNC`, the ring of the modem for a clear channel call (refer to
    `AT+CRC` below), as CIP 2 (unrestricted digital
    information).
  - It offers all other rings as the first of CIP 4 (3.1 kHz audio),
    16 (telephony) and 1 (speech) that the mask has.

  The number of the caller comes from `AT+VCID`. The called number is empty,
  because there is no MSN selection yet.
- **CONNECT_REQ:** the controller sets the B protocol with AT commands (refer to the table
  below). Then `ATD` dials the called number. The `CONNECT` of the modem
  becomes CONNECT_ACTIVE_IND. Then the CONNECT_B3_REQ of the application
  gives CONNECT_B3_ACTIVE_IND.
- **CONNECT_RESP:** the first application that accepts the call gets it.
  The others get DISCONNECT_IND with reason `0x3304`. The controller sets the B protocol,
  then `ATA` answers. The `CONNECT` of the modem becomes
  CONNECT_ACTIVE_IND and CONNECT_B3_IND.
  - A call that nobody accepts continues to ring.
  - When the rings stop for 10 seconds, each application to which the controller
    offered the call gets DISCONNECT_IND.
- **DATA_B3:** the data of the application goes to the modem without changes, and
  its DATA_B3_CONF comes when the modem has accepted it. The data of the modem
  arrives in DATA_B3_INDs of up to the maxBDataLen of the application, with
  as many outstanding as its maxBDataBlocks (7 at most).
- **DISCONNECT_REQ:** the controller sends `+++` between guard times
  (the guard time of D-Modem is 500 ms), then `ATH`. DISCONNECT_B3_IND and
  DISCONNECT_IND (reason 0) follow.
- **The modem's result codes:**

  | Modem | DISCONNECT_IND reason |
  |---|---|
  | `NO CARRIER` | `0x3490` (normal clearing), once the data before it has reached the application |
  | `BUSY` | `0x3491` |
  | `NO ANSWER` | `0x3493` |

- **Other messages:**
  - FACILITY_REQ answers the GetSupportedServices of the supplementary
    services (with none), and all other requests with "facility
    not supported".
  - The controller confirms ALERT_REQ and INFO_REQ, and does nothing for them.
  - The controller refuses SELECT_B_PROTOCOL and RESET_B3.

### B protocols

CAPI describes the protocol stack of a call as B1 (layer 1), B2 (layer 2)
and B3 (layer 3). The controller changes each supported combination into
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

- An empty B protocol is the default of CAPI, X.75.
- The V.110 rate is the first word of the configuration of B1: 300, 1200,
  2400, 4800, 9600, 14400, 19200, 28800, 38400 or 56000, or 0 for the
  rate of the caller. The controller refuses all other rates.
- **B1 0 with B2 1** is what capiplugin asks for. The controller puts
  each DATA_B3 in a frame with the framing of RFC 1662 for the sync
  PPP of the modem (`+B2=5`), and changes each frame back into a DATA_B3.
- **Refused** with the "not supported" info of CAPI: other protocols, for example
  fax, B1 1 (the bit-transparent channel) and X.25.
- **Modem calls (B1 7 or 8)** use the modulation of the modem itself. The
  controller reads `AT+MS?` at start-up, and sets it again (with
  `+CBST=0`) for each such call.

### AT+CRC

`AT+CRC=1` (from 27.007) is the only addition to the modem for CAPI. With
it, the modem reports a ring as `+CRING: SYNC` for a call on a clear channel,
and as `+CRING: VOICE` for all other calls, instead of `RING`. With an
older D-Modem that refuses `+CRC`, the controller removes it from its
set-up, accepts each plain `RING`, and offers all calls as audio calls.

### Limits

- One call at a time.
- While online, the controller monitors the data for the `NO CARRIER` line of the modem.
  Thus, that line in the data would stop the call.
- No fax (B1 4 of CAPI) yet.
- No Windows `capi2032.dll` yet.

The `dmodem_ring()` of libdmodem rings a modem, for a program that finds
calls itself. `frontend/capi/test/capi_test.cpp` uses it.

### Tests

`frontend/capi/test/capi_test.cpp` runs the controller on one of two libdmodem
modems on a simulated line, with an application of the test. It
covers:

- the mapping of B protocols to AT commands;
- calls out over X.75, V.120, frames (sync PPP), V.110 at 9600 bit/s,
  and a modem call (V.42 over V.90 or V.91), with data in the two directions;
- a call in on a clear channel with the number of the caller, and a second
  listening application that does not get it;
- hangups from the two ends, a call that nobody answers, a call that
  stops ringing, and a B protocol that the controller does not have;
- the TCP server in the framing of rcapi, against a modem of the test.

### Tried with libcapi20

With libcapi20 from isdn4k-utils (its `rcapi` module):

- `capiinfo` lists the controller and its profile.
- An application of libcapi20 called a libdmodem modem through
  `dmodem-capid` with X.75, frames, V.120, V.110 at 9600 bit/s and a
  modem call (V.42 over V.90). Its data came back byte for byte,
  and then it hung up.

Not yet tried: an incoming call through libcapi20, and the applications
themselves (capi4hylafax, chan_capi, capiplugin).

## What comes next

The later phases of the feasibility study use this line mode:

1. **Done:** X.75 and sync PPP (`AT+B2`), and `codec_clearmode`.
   **Still to come:** V.42bis over X.75, and MSN selection.
2. **Done:** V.110 (`AT+CBST`), V.120 (`AT+B2=9`), and mISDN for BRI
   cards on Linux (`--misdn`), which still needs a test on a real card.
3. **CAPI 2.0:** done for data and modem calls (`dmodem-capid`, above).
   **Still to come:** fax over CAPI, and a Windows `capi2032.dll`.
   The design of hardware (an S0 USB stick) is in progress.
