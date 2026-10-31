# app_dmodem: a data modem on an Asterisk channel: technical details

This is the technical companion to [Asterisk: app_dmodem](asterisk.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

> **Build it yourself; don't ship it.** A built `app_dmodem.so` contains
> libdmodem (AGPL-3.0) inside an Asterisk module (GPL-2.0-only). Those
> two licences cannot be combined, so the built module may not be
> distributed ([LICENSING.md](../LICENSING.md)). It is therefore off by
> default: build it with `-DDMODEM_APP_DMODEM=ON` for your own Asterisk.
> To ship D-Modem for Asterisk, use [dmodem-ari](ari.md), which runs the
> same modem and profiles as a separate program beside Asterisk.

## Overview

The module adds three dialplan applications:

| application | what it does |
|---|---|
| `DModem([profile[,options]])` | Answers the call with a software modem and, once the modems connect, hands the data to a server or program. |
| `DModemReceiveFax(file[,ident])` | Answers as a fax machine and receives into a TIFF file. |
| `DModemSendFax(file[,ident])` | Sends a TIFF file as a fax on a call placed to a fax machine. |

`DModem()` hands the data to one of three places, as the profile in
`dmodem.conf` says:

- a server over TCP (`mode = tcp`), such as a BBS;
- a server on a unix socket (`mode = unix`), such as a login menu;
- a program on a pty (`mode = exec`), such as `pppd` for dial-up IP, a
  login or a BBS door.

Without a profile argument, `DModem()` uses the profile `[default]`. One
Asterisk can serve several numbers in several ways:

```
; extensions.conf
exten => 5551000,1,DModem(bbs)
 same => n,Hangup()
exten => 5552000,1,DModem(ppp)
 same => n,Hangup()
```

```
; dmodem.conf
[bbs]
mode = tcp
host = 127.0.0.1
port = 2323

[ppp]
mode = exec
command = exec /usr/sbin/pppd nodetach noauth local lock passive 192.168.77.1:192.168.77.2
```

[dmodem.conf.sample](../asterisk/dmodem.conf.sample) lists every option.

The same calls and faxes can instead go to [dmodem-ari](ari.md), which
is D-Modem as an ARI (Asterisk REST Interface) application beside
Asterisk. It uses the same profiles, through `Stasis(dmodem,bbs)` and
`Stasis(dmodem,receivefax,<file>)`, and needs no module inside Asterisk.
Its faxes use T.38 in the same way as the module's do
([ari.md](ari-details.md#t38)).

### Profile options

| option | default | meaning |
|---|---|---|
| `mode` | `tcp` | `tcp`, `unix` or `exec`. |
| `host`, `port` | none | `mode = tcp`: the server. Both are required. |
| `path` | none | `mode = unix`: the server's stream socket. Required. |
| `command` | none | `mode = exec`: the program, run by `/bin/sh -c`. Required. |
| `header` | `no` | `tcp` and `unix` only: send the call's details first (see [below](#what-the-server-or-program-sees)). |
| `init` | empty | Extra AT commands for the modem, in both directions (for example `AT+MS=34` to stop at V.34). |
| `answer` | `ATA` | The command line the modem answers with. |
| `law` | `auto` | The network's G.711 law: `ulaw`, `alaw`, or `auto` for the channel's. |
| `clear` | `auto` | ISDN data calls as a clear 64 kbit/s channel: `auto`, `yes` or `no` (see [The channel](#the-channel)). |
| `timeout` | `60` | Seconds the modems have to connect. |
| `fax` | `yes` | Receive a calling fax machine as a fax (see [Fax](#fax)). |

Option names are matched without regard to case, and `_` and `-` are
the same (`fax_dir` and `fax-dir` both work). A profile that lacks the
fields its mode requires is refused, and `DModem()` ends with
`DMODEMSTATUS` `ERROR`.

### Answering and calling

The modem is first set up with `ATE0Q1S0=0S2=255X3` (no echo, no result
codes, no auto-answer, no `+++` escape, no wait for a dial tone),
followed by the profile's `init`.

By default the modem answers with `ATA`. A profile's `answer` gives a
different command line. Commands that apply only to the answering call
go before the `A`. For example, `answer = AT+MS=132\;A` answers at
V.32bis at most. In `dmodem.conf` a `;` must be written `\;`, because a
bare `;` starts a comment. If the command line leaves the modem not
answering, the call ends with `DMODEMSTATUS` `ERROR`.

`DModem(profile,o)` makes the modem the caller instead: it waits for the
remote modem's answer tone. Use it on a call that Asterisk has placed to
a remote modem, for example:

```
channel originate PJSIP/5559999@trunk application DModem(isp,o)
```

The profile's server or program then talks to the remote BBS or ISP.

### The modem

The modem is `libdmodem` ([dmodemd/dmodem.h](../dmodemd/dmodem.h)). It
is D-Modem's modem core (AT commands, V.42 error correction, V.44 or
V.42bis compression) with the open data pumps of `dsp`. It supports:

- V.21, V.22, V.22bis, V.23, Bell 103 and 212A, V.32, V.32bis and V.34;
- V.90 as the digital (server) modem: up to 56000 bit/s downstream to a
  V.90 client modem, when the call is G.711 end to end;
- V.91 with another digital modem such as another D-Modem (the SIP
  tests connect at 62666/62666 bit/s, see [V.91](dsp/v91-details.md)).

The module has no binary-only parts.

## Fax

The module is also a group 3 fax machine of its own, an alternative to
Asterisk's `res_fax` with spandsp:

- T.30 on V.17, V.29 and V.27ter, at 2400 to 14400 bit/s;
- pages in MH or MR coding, in TIFF files;
- error correction mode (ECM) when the other machine has it.

It is used in three ways:

- **`DModem()` receives faxes by itself.** The modem answers adaptively.
  If the calling machine's CNG tone, or its V.8 CM of a fax call, comes
  before a modem connection (`dmodem_status()`'s `fax_caller`), the call
  goes to the fax receiver instead. It writes
  `<fax_dir>/<date>-<time>-<caller>-<uniqueid>.tif`, where the date and
  time are `YYYYMMDD-HHMMSS`, and the caller is the caller ID's digits
  and `+` (or `unknown`). `DMODEMSTATUS` is then `FAX`. The profile's
  `fax = no` turns this off. This applies only when answering, not with
  the `o` option.
- **`DModemReceiveFax(file[,ident])`** answers as a fax machine and
  receives into the file. No file is written if no page came.
- **`DModemSendFax(file[,ident])`** sends a TIFF file on a call placed to
  a fax machine (it waits for the machine's DIS), for example:

  ```
  channel originate PJSIP/5551234@trunk application DModemSendFax /var/spool/fax/doc.tif
  ```

  Any TIFF of 1-bit pels can be sent: uncompressed, G3 1D or 2D, or G4.
  It can be of any width (it is fitted to the fax's 1728 pels) and any
  resolution (standard and fine are sent as they are; superfine is
  halved). This makes one from a PDF:

  ```
  gs -sDEVICE=tiffg4 -r204x196 -o doc.tif doc.pdf
  ```

`ident` is our fax number (the CSI when receiving, the TSI when
sending). Without it, `fax_ident` of `[general]` is used.

### Fax variables

All three set these channel variables after a fax:

| variable | value |
|---|---|
| `DMODEMFAXSTATUS` | `SUCCESS` or `FAILED` |
| `DMODEMFAXRESULT` | why (for example `no document` when the file could not be read or written) |
| `DMODEMFAXPAGES` | the number of pages |
| `DMODEMFAXRATE` | the rate, in bit/s |
| `DMODEMFAXREMOTE` | the other machine's number |
| `DMODEMFAXECM` | `yes` when the call used error correction mode, else `no` |
| `DMODEMFAXT38` | `yes` when the fax went over T.38, else `no` |
| `DMODEMFAXFILE` | the file |

### Fax options

These go in `[general]` or in a profile. `DModemSendFax` and
`DModemReceiveFax` read only `[general]`.

| option | default | meaning |
|---|---|---|
| `fax_dir` | `/var/spool/asterisk/fax` (Asterisk's spool directory + `/fax`) | Where `DModem()` puts the faxes it receives. |
| `fax_ident` | empty | Our fax number (CSI and TSI). |
| `fax_max_rate` | `14400` | The fastest rate: 14400, 12000, 9600, 7200, 4800 or 2400. |
| `fax_ecm` | `yes` | Offer error correction mode. |
| `fax_t38` | `yes` | Use T.38 when the channel has it. |

In error correction mode (T.30 Annex A), a page is sent in blocks of HDLC
frames. The receiving machine asks again for any frames that a line hit
spoiled, so the page arrives exactly as it was sent. Without ECM, the
rows that were hit arrive damaged. Both machines must offer ECM;
`fax_ecm = no` turns it off.

### T.38

T.38 carries fax as data packets over IP instead of as audio. On a
channel that supports T.38 (PJSIP, an endpoint with `t38_udptl = yes`),
the faxes switch to T.38 when the channel does, as Asterisk's own
`SendFAX` and `ReceiveFAX` do. `fax_t38 = no` keeps them on the audio.

- `DModemReceiveFax` asks for T.38 at once (Asterisk sends a re-INVITE).
  It waits up to 6 s for the answer, with silence on the line. If T.38 is
  refused or there is no answer, the fax goes on the audio.
- `DModemSendFax` accepts T.38 when the other end asks for it, until the
  first page has gone. After that the fax stays on the audio. (The same
  rule applies to a receiver.)
- `DModem()` takes a T.38 offer that arrives before the modems connect as
  a calling fax machine (for example a gateway's). This requires
  answering mode, `fax = yes` and `fax_t38 = yes`; otherwise the offer is
  refused. A fax machine that `DModem()` hears on the audio asks for T.38
  in the same way as `DModemReceiveFax`.

Over T.38 the fax machine is a T.38 terminal (libdmodem's):

- It sends and receives the IFP packets of T.38 versions 0 to 3 as
  `AST_FRAME_MODEM` frames. When we ask, we offer version 0; when the
  other end asks, we answer with version 3; the lower of the two sides'
  versions is used.
- We take IFP packets of up to 400 octets.
- The UDPTL transport and its redundancy are Asterisk's.
- The packets are paced by the clock.

At the end of the fax, the module asks for the channel to go back to
audio (Asterisk re-INVITEs again) and waits up to 5 s for that.

Asterisk's T.38 gateway (`FAXOPT(gateway)=yes`) puts a fax machine on the
audio at the other end. The console says `over T.38` at the end of a fax
that went that way.

## The channel

The modem needs the line's audio exactly as it is:

- **G.711 only (ulaw or alaw), with no transcoding.** A codec change,
  even between the two laws, costs V.90 and lowers V.34. The profile's
  `law` picks the law; `auto` takes the channel's (alaw if the channel
  has alaw and not ulaw, otherwise ulaw).
- **No processing on the way.** No echo cancellation, jitter buffer,
  silence suppression or comfort noise between the modem and the
  network, and no DSP features (fax or DTMF detection) on the channel.
  For a SIP trunk, a `pjsip.conf` endpoint along these lines:

  ```
  [modem-trunk]
  type = endpoint
  disallow = all
  allow = ulaw          ; or alaw: the law of the PSTN side
  direct_media = no
  ```

- **A steady RTP stream.** Packet loss and clock slips show up as line
  errors. V.42 retransmits them, but they can drop a fast connection.
  When no frame arrives for 200 ms (a channel with no clock, or silence
  suppression), the module feeds the modem silence and carries on.

### Clear channel (ISDN data calls)

- **An ISDN data call** (an unrestricted digital bearer, such as
  chan_dahdi's on a BRI or PRI span) is a clear 64 kbit/s channel. The
  modem runs `AT+MS=CLEAR` on the channel's octets exactly as they are,
  to talk to another D-Modem's clear channel. The link layer on it is
  X.75 unless the profile's `init` chooses another with `AT+B2`
  (`AT+B2=5` sync PPP, `AT+B2=7` the modem link). The profile's `clear`
  decides:
  - `auto` (the default): a clear channel for a call with a digital
    bearer;
  - `yes`: every call;
  - `no`: never.

  See [isdn.md](isdn.md).
- **A SIP call in RFC 4040's CLEARMODE** is clear too, whatever `clear`
  says. `codec_clearmode.so` gives Asterisk the CLEARMODE codec, and
  `allow = clearmode` on an endpoint offers and accepts it.
  `codec_clearmode.so` is built and installed whenever Asterisk's headers
  are found (`DMODEM_ASTERISK`, on by default), with or without
  app_dmodem. It contains no D-Modem code and may be shipped.

## Building

The module builds outside the Asterisk tree, with the rest of D-Modem.
It needs the headers of an installed Asterisk of the same version: run
`make install-headers` after building Asterisk, or install your
distribution's `asterisk-dev` package. Without the headers, CMake skips
it. It is built only when asked for, for your own Asterisk (see the top
of this page):

```
cmake -B build -DDMODEM_APP_DMODEM=ON   # in the top directory
cmake --build build --target app_dmodem
cmake --install build                   # the module, and dmodem.conf.sample
asterisk -rx "module load app_dmodem.so"
```

CMake options for other locations:

- `-DASTERISK_INCLUDE_DIR=/opt/asterisk/include` for headers elsewhere;
- `-DASTERISK_MODULES_DIR=...` for where `cmake --install` puts the
  module (default: Asterisk's modules directory, such as
  `/usr/lib/asterisk/modules`);
- `-DASTERISK_CONF_DIR=...` for where it puts the sample configuration
  (default `/etc/asterisk`).

The test scripts in `test/` take the binaries from `build/`
(`DMODEM_BUILD` names another build directory).

`DModem()` reads `dmodem.conf` on every call, so a change applies from
the next call. `debug` is read when the module loads and on
`module reload`.

Each call runs its modem in the channel's own thread; the connection to
a TCP or unix-socket server is made in a separate short-lived thread, so
the audio keeps flowing while it connects. There is no limit on the
number of calls other than CPU: a V.34 or V.90 call takes a few percent
of a core.

## What the server or program sees

The server or program sees nothing until the modems are connected. Then
it gets the bytes from the remote modem, after V.42 and V.44 or V.42bis.
A server in `mode = unix` (a stream socket at `path`) gets exactly what a
server in `mode = tcp` gets.

How the call ends:

- When the server or program ends its side (closes the socket, or the
  program exits), the module hangs up once everything it sent last is on
  the line: when nothing is left to send and at least 1 s has passed, or
  after 30 s at most.
- When the remote modem hangs up, the module closes the socket, or hangs
  up the pty (the program gets SIGHUP). A program still running 5 s after
  the SIGHUP is killed with SIGKILL.

A program in `mode = exec` gets the pty as its stdin, stdout, stderr and
controlling terminal, in raw mode. Its environment has:

| variable | value |
|---|---|
| `DMODEM_CHANNEL` | the Asterisk channel |
| `DMODEM_PROFILE` | the profile |
| `DMODEM_RATE` | the connect rate, transmit/receive bit/s |
| `DMODEM_CALLERID` | the caller's number |
| `DMODEM_DIALED` | the number dialled (the DNID, or else the extension) |
| `DMODEM_CONNECTED` | when the modems connected, in Unix time |

A server has no environment. Instead, with `header = yes` in a `tcp` or
`unix` profile, the same variables are sent first on the connection: one
`NAME=value` per line (a value with a line break has it replaced by a
space), then an empty line, then the caller's bytes:

```
DMODEM_CHANNEL=PJSIP/trunk-00000001
DMODEM_PROFILE=menu
DMODEM_RATE=33600/31200
DMODEM_CALLERID=5559999
DMODEM_DIALED=5551000
DMODEM_CONNECTED=1791666309

```

[`modemmenu`](https://github.com/D-Modem/menu) uses these variables. It
offers a login (from a text file, LDAP, SQL or PAM) and a menu of a BBS,
PPP and a shell, each started with the caller's credentials and these
details of the call. It runs either as a `mode = exec` program for each
call, or as one server for all calls (`modemmenu -serve`) behind a
`mode = unix` profile with `header = yes`.

When it returns, `DModem()` sets:

- `DMODEMSTATUS`:
  - `NOCARRIER`: the modems did not connect (including within the
    profile's `timeout`);
  - `REMOTE`: the remote modem went away;
  - `LOCAL`: the server or program ended the session;
  - `HANGUP`: the channel hung up;
  - `FAX`: a fax machine called and the fax receiver took the call;
  - `ERROR`: a configuration or system error (no such profile, an
    `answer` line that did not answer, a server that could not be
    reached, and so on).
- `DMODEMRATE`: the connect rate, transmit/receive bit/s.

## Testing

The tests start a private Asterisk: the installed one, with the module
built here. They need no changes to the system's Asterisk.

### run.sh: a modem call

`test/run.sh` calls one `DModem()` from another over a Local channel:

- The answering `DModem()` serves a test BBS (`test/bbs_sim.py`).
- The calling one (`DModem(term,o)`) hands its data to a script playing
  the user.
- The user reads the banner, sends 20 lines, checks their echoes and logs
  off. The BBS then closes, which hangs up the call.

```
test/run.sh                       # V.34
test/run.sh AT+MS=122             # V.22bis (also 22, 32, 132)
test/run.sh AT+MS=34 bbsexec      # the BBS as a program on the pty
test/run.sh "" bbsunix            # the BBS on a unix socket, with the
                                  # call's details first (header = yes)
LAW=alaw test/run.sh              # on A-law
ANSWER='AT+MS=132\;A' EXPECT='ANSWERER .*DMODEMRATE=14400/' test/run.sh
                                  # the answer command line: V.32bis
```

The first argument goes into the calling profile's `init`. `EXPECT` is a
regular expression that some line of Asterisk's console must match, or
the test fails.

A Local channel has no clock, so the two modems run sample for sample,
faster than real time. They don't mind.

### fax.sh and fax_spandsp.sh: faxes on the audio

`test/fax.sh` sends a two-page fax (the test document of
`dsp/test/fax_doc`) from `DModemSendFax`:

1. to `DModem()`, which has to notice the fax machine by itself;
2. to `DModemReceiveFax`.

It compares what arrived with what was sent.

`test/fax_spandsp.sh` does the same against Asterisk's own fax
(`res_fax_spandsp`):

1. `SendFAX` to `DModem()`;
2. `SendFAX` to `DModemReceiveFax`;
3. `DModemSendFax` to `ReceiveFAX`.

They run at the default top rate of 14400 bit/s. Variables:
`CASES` (for example `CASES="2 3"` runs only those), `FAX_MAX_RATE`
(for example `9600`: the module's fax no faster), and `DMODEM_DEBUG=3`
(the DSP traces in `console.log`).

### t38.sh: faxes over T.38

`test/t38.sh` has Asterisk call itself over PJSIP: out on `loopout`, in
on `loopin`, both with `t38_udptl`. The cases:

1. `SendFAX` to `DModemReceiveFax`, over T.38;
2. `DModemSendFax` to `ReceiveFAX`, over T.38;
3. `DModemSendFax` to `DModemReceiveFax`, over T.38;
4. `SendFAX` to `DModem()`, over T.38;
5. `SendFAX` from an endpoint without T.38 (`loopaudio`) to
   `DModemReceiveFax`: T.38 is refused, so the fax goes on the audio;
6. through Asterisk's T.38 gateway, `SendFAX` on the audio to `DModem()`
   over T.38;
7. through the gateway, `DModemSendFax` to `ReceiveFAX` on the audio (the
   gateway asks for T.38 when it hears the DIS);
8. `DModemReceiveFax` to `DModem()`, which takes its T.38 request before
   the modems connect as a calling fax machine. Both ends are receivers,
   so there is no document; only that is checked.

Each document received in cases 1 to 7 must match the one sent. Variables:
`CASES`, `DMODEM_DEBUG=3`, `SIP_LOG=1` (the SIP messages in the console)
and `FAX_DEBUG=1` (spandsp's own debug output).

Before the two gateway cases, `t38.sh` tries Asterisk's gateway by itself
(`SendFAX` on the audio through it to `ReceiveFAX` over T.38). Ubuntu
24.04's Asterisk 20.6 passes no fax there, so on it cases 6 and 7 are
skipped, and the script says so (`GATEWAY=yes` runs them anyway). CI
builds Asterisk 22 (LTS) from source with `test/build_asterisk.sh
<prefix>`, where all the cases run. The script's header lists the
packages it needs and how to point the tests at it: `PATH`,
`AST_MODULES`, `AST_DATA`, and `-DASTERISK_INCLUDE_DIR` for the build.

`ARI=1` runs these tests (and `test/sip.sh`) through
[dmodem-ari](ari-details.md#testing) instead of the module.

### sip.sh: dmodemd over SIP

`test/sip.sh` has D-Modem's own `dmodemd` call `DModem(bbs)` over SIP
(`INCOMING=1`: answer a call from `DModem(term,o)`), using the same test
BBS. `CODEC=alaw` or `ulaw` limits the call to one law. `CODEC=clearmode`
runs it in RFC 4040's clear channel and checks for
`DMODEMRATE=64000/64000`.

**Voice, incoming (`VOICE=1 test/sip.sh`).** Asterisk calls dmodemd,
which answers in voice mode with `AT+FCLASS=8` and `AT+VLS=1`. It sets
`AT+VTD=10` and plays `AT+VTS=1,!,{!,30},2`, while the dialplan sends it
the DTMF digit 5. The test checks:

- Asterisk's RTP debug shows at least two end packets of RFC 4733 event
  16 (the hook flashes);
- dmodemd's log shows the two flashes sent, of 100 and 300 ms, and the
  digit 5 received as an RFC 4733 event and played as its tone;
- the voice engine reports the digit to the pty as `<DLE>5` ...
  `<DLE>~`.

**Voice, outgoing (`VOICE_DIAL=1 test/sip.sh`).** dmodemd dials in voice
mode (`AT+FCLASS=8`, `ATD`) to the dialplan's extensions in `modem-in`.
First with `S7=6` (6 s) and no early media:

| extension | what it does | expected result |
|---|---|---|
| 7002 | busy | `BUSY` |
| 7003 | rings, never answers | `NO ANSWER` after S7 (6 s) |
| 7001 | rings 2 s, answers, sends the digit 7, hangs up about 4 s later | `OK` on the answer, then `<DLE>7` ... `<DLE>~`, `OK` to `AT+VTS=1,2`, and `<DLE>l` alone at the hang-up (no `NO CARRIER`) |

Then with `S7=15` and early media (183 Session Progress):

| extension | what it does | expected result |
|---|---|---|
| 7004 | ringing tone for 7 s, then a 200 OK | at least two rings (`<DLE>r`), then `OK` at the 200 OK |
| 7005 | busy tone, its 486 only after 15 s | `BUSY` from the tone, before the 486 |
| 7007 | answer tone (2100 Hz), never answers | `<DLE>a` and `OK` |
| 7006 | no such number (404) | `ERROR` |

After that, `ATH` must leave voice mode (`+FCLASS: 0`), and a data call to
7002 must give `BUSY`.

### efax.sh: dmodemd's class 1 fax

`test/efax.sh` runs efax on dmodemd's pty (fax class 1) against
Asterisk's own fax over PJSIP (`t38_udptl`):

1. efax sends: dmodemd calls `ReceiveFAX`, which asks for T.38.
2. efax receives: `SendFAX` calls dmodemd, which answers and asks for
   T.38 a second after it answers.

Both go over T.38. Options:

- `T38=no`: the endpoint without T.38 (dmodemd's request is refused, and
  the faxes go on the audio);
- `DMODEMD_T38=no`: dmodemd without T.38 (Asterisk's request is refused).

The pages efax receives must be the document's. The pages it sends carry
a header of efax's own, and must arrive with the rows it sent. The test
needs efax and libtiff-tools (`tiffcp`). Other variables: `CASES`,
`DMODEMD_DEBUG`, `SIP_LOG=1`, `FAX_DEBUG=1`, `EFAX_V` and `EFAX_STRACE=1`.

## Debugging

`debug = 3` in `[general]` with `core set verbose 5` shows the modem's
handshake on the console (`debug`: 0 errors only, 1 the modem core, 3
the data pumps as well; libdmodem's lines appear at verbose level 5, the
call's progress at level 4 and above). The same modem also runs outside
Asterisk:

- in `dsp/test/dmodem_test` (two modems of the library back to back);
- against Smart Link's original binary library in
  `dsp/test/smartlink_test` (`interop_call`).
