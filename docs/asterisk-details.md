# app_dmodem: a data modem on an Asterisk channel: technical details

This page gives the technical data for [Asterisk: app_dmodem](asterisk.md), which is the overview in plain English. Read the overview first. This page gives all the data: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

> **Build it yourself. Do not distribute it.** A built `app_dmodem.so`
> contains libdmodem (AGPL-3.0) in an Asterisk module (GPL-2.0-only).
> You cannot combine these two licences, thus you must not distribute the
> built module ([LICENSING.md](../LICENSING.md)). Thus it is off by
> default. To build it for your own Asterisk, use `-DDMODEM_APP_DMODEM=ON`.
> To distribute D-Modem for Asterisk, use [dmodem-ari](ari.md). It runs
> the same modem and profiles as a separate program next to Asterisk.

## Overview

The module adds three dialplan applications:

| application | what it does |
|---|---|
| `DModem([profile[,options]])` | Answers the call with a software modem and, when the modems connect, sends the data to a server or program. |
| `DModemReceiveFax(file[,ident])` | Answers as a fax machine and receives into a TIFF file. |
| `DModemSendFax(file[,ident])` | Sends a TIFF file as a fax on a call to a fax machine. |

`DModem()` sends the data to one of three destinations, as the profile
in `dmodem.conf` specifies:

- a server through TCP (`mode = tcp`), for example a BBS;
- a server on a unix socket (`mode = unix`), for example a login menu;
- a program on a pty (`mode = exec`), for example `pppd` for dial-up IP,
  a login or a BBS door.

If there is no profile argument, `DModem()` uses the profile `[default]`.
One Asterisk can serve many numbers in different ways:

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

[dmodem.conf.sample](../frontend/asterisk/dmodem.conf.sample) gives all the options.

You can also send the same calls and faxes to [dmodem-ari](ari.md).
This is D-Modem as an ARI (Asterisk REST Interface) application next to
Asterisk. It uses the same profiles, through `Stasis(dmodem,bbs)` and
`Stasis(dmodem,receivefax,<file>)`, and does not need a module in
Asterisk. Its faxes use T.38 in the same way as the faxes of the module
([ari.md](ari-details.md#t38)).

### Profile options

| option | default | meaning |
|---|---|---|
| `mode` | `tcp` | `tcp`, `unix` or `exec`. |
| `host`, `port` | none | `mode = tcp`: the server. The two are necessary. |
| `path` | none | `mode = unix`: the stream socket of the server. Necessary. |
| `command` | none | `mode = exec`: the program, which `/bin/sh -c` runs. Necessary. |
| `header` | `no` | `tcp` and `unix` only: send the details of the call first (refer to [below](#what-the-server-or-program-sees)). |
| `handoff` | `no` | `unix` with `header` only: give the line of the modem directly to the server (refer to [below](#what-the-server-or-program-sees)). |
| `init` | empty | More AT commands for the modem, in the two directions (for example `AT+MS=34` to stop at V.34). |
| `answer` | `ATA` | The command line that the modem uses to answer. |
| `law` | `auto` | The G.711 law of the network: `ulaw`, `alaw`, or `auto` for the law of the channel. |
| `clear` | `auto` | ISDN data calls as a clear 64 kbit/s channel: `auto`, `yes` or `no` (refer to [The channel](#the-channel)). |
| `timeout` | `60` | The seconds that the modems have to connect. Then (`tcp`, `unix`) the seconds that the server has to accept the call. If not, the call ends with `ERROR`. |
| `fax` | `yes` | Receive a calling fax machine as a fax (refer to [Fax](#fax)). |

Option names are not case-sensitive, and `_` and `-` are the same
(`fax_dir` and `fax-dir` are both correct). If a profile does not have
the fields that its mode needs, the module refuses it, and `DModem()`
ends with `DMODEMSTATUS` `ERROR`.

### Answering and calling

First, the module sets up the modem with `ATE0Q1S0=0S2=255X3` (no echo,
no result codes, no auto-answer, no `+++` escape, no wait for a dial
tone), and then with the `init` of the profile.

By default, the modem answers with `ATA`. The `answer` of a profile
gives a different command line. Put the commands that apply only to the
answering call before the `A`. For example, `answer = AT+MS=132\;A`
answers at V.32bis maximum. In `dmodem.conf`, write a `;` as `\;`,
because a bare `;` starts a comment. If the modem does not answer after
the command line, the call ends with `DMODEMSTATUS` `ERROR`.

`DModem(profile,o)` makes the modem the caller: it waits for the answer
tone of the remote modem. Use it on a call that Asterisk makes to a
remote modem, for example:

```
channel originate PJSIP/5559999@trunk application DModem(isp,o)
```

Then the server or program of the profile communicates with the remote
BBS or ISP.

### The modem

The modem is `libdmodem` ([frontend/dmodemd/dmodem.h](../frontend/dmodemd/dmodem.h)). It
is the modem core of D-Modem (AT commands, V.42 error correction, V.44
or V.42bis compression) with the open data pumps of `dsp`. It supports:

- V.21, V.22, V.22bis, V.23, Bell 103 and 212A, V.32, V.32bis and V.34;
- V.90 as the digital (server) modem: up to 56000 bit/s downstream to a
  V.90 client modem, when the call is G.711 from end to end;
- V.91 with a different digital modem, for example a second D-Modem
  (the SIP tests connect at 62666/62666 bit/s, refer to [V.91](dsp/v91-details.md)).

The module has no binary-only parts.

## Fax

The module is also a group 3 fax machine. It is an alternative to the
`res_fax` of Asterisk with spandsp:

- T.30 on V.17, V.29 and V.27ter, at 2400 to 14400 bit/s;
- pages in MH or MR coding, in TIFF files;
- error correction mode (ECM) when the other machine has it.

You can use it in three ways:

- **`DModem()` receives faxes automatically.** The modem answers
  adaptively. If the CNG tone of the calling machine, or its V.8 CM of a
  fax call, comes before a modem connection (`fax_caller` of
  `dmodem_status()`), the call goes to the fax receiver. It writes
  `<fax_dir>/<date>-<time>-<caller>-<uniqueid>.tif`. The date and time
  are `YYYYMMDD-HHMMSS`, and the caller is the digits and `+` of the
  caller ID (or `unknown`). Then `DMODEMSTATUS` is `FAX`. Set `fax = no`
  in the profile to turn this off. This applies only to answering, not
  to the `o` option.
- **`DModemReceiveFax(file[,ident])`** answers as a fax machine and
  receives into the file. If no page came, it writes no file.
- **`DModemSendFax(file[,ident])`** sends a TIFF file on a call to a fax
  machine (it waits for the DIS of the machine), for example:

  ```
  channel originate PJSIP/5551234@trunk application DModemSendFax /var/spool/fax/doc.tif
  ```

  It can send all TIFFs of 1-bit pels: uncompressed, G3 1D or 2D, or G4.
  The width can be any value (the module fits it to the 1728 pels of the
  fax), and the resolution can be any value (it sends standard and fine
  as they are, and divides superfine by two). This command makes a TIFF
  from a PDF:

  ```
  gs -sDEVICE=tiffg4 -r204x196 -o doc.tif doc.pdf
  ```

`ident` is our fax number (the CSI when we receive, the TSI when we
send). If there is no `ident`, the module uses `fax_ident` of
`[general]`.

### Fax variables

After a fax, all three applications set these channel variables:

| variable | value |
|---|---|
| `DMODEMFAXSTATUS` | `SUCCESS` or `FAILED` |
| `DMODEMFAXRESULT` | the reason (for example `no document` when the module could not read or write the file) |
| `DMODEMFAXPAGES` | the number of pages |
| `DMODEMFAXRATE` | the rate, in bit/s |
| `DMODEMFAXREMOTE` | the number of the other machine |
| `DMODEMFAXECM` | `yes` when the call used error correction mode, if not `no` |
| `DMODEMFAXT38` | `yes` when the fax went through T.38, if not `no` |
| `DMODEMFAXFILE` | the file |

### Fax options

Put these in `[general]` or in a profile. `DModemSendFax` and
`DModemReceiveFax` read only `[general]`.

| option | default | meaning |
|---|---|---|
| `fax_dir` | `/var/spool/asterisk/fax` (the spool directory of Asterisk + `/fax`) | The directory where `DModem()` puts the faxes that it receives. |
| `fax_ident` | empty | Our fax number (CSI and TSI). |
| `fax_max_rate` | `14400` | The highest rate: 14400, 12000, 9600, 7200, 4800 or 2400. |
| `fax_ecm` | `yes` | Offer error correction mode. |
| `fax_t38` | `yes` | Use T.38 when the channel has it. |

In error correction mode (T.30 Annex A), a page goes in blocks of HDLC
frames. The receiving machine asks again for each frame that a line hit
damaged. Thus the page arrives exactly as it was sent. Without ECM, the
rows that a hit damaged arrive damaged. The two machines must offer ECM.
`fax_ecm = no` turns it off.

### T.38

T.38 sends fax as data packets through IP instead of as audio. On a
channel that supports T.38 (PJSIP, an endpoint with `t38_udptl = yes`),
the faxes change to T.38 when the channel changes, as the `SendFAX` and
`ReceiveFAX` of Asterisk do. `fax_t38 = no` keeps them on the audio.

- `DModemReceiveFax` asks for T.38 immediately (Asterisk sends a
  re-INVITE). It waits up to 6 s for the answer, with silence on the
  line. If the other end refuses T.38 or does not answer, the fax goes
  on the audio.
- `DModemSendFax` accepts T.38 when the other end asks for it, until the
  first page has gone. After that, the fax stays on the audio. (The same
  rule applies to a receiver.)
- `DModem()` accepts a T.38 offer that arrives before the modems connect
  as a calling fax machine (for example from a gateway). This needs
  answering mode, `fax = yes` and `fax_t38 = yes`. If not, the module
  refuses the offer. When `DModem()` receives a fax machine on the
  audio, it asks for T.38 in the same way as `DModemReceiveFax`.

Through T.38, the fax machine is a T.38 terminal (from libdmodem):

- It sends and receives the IFP packets of T.38 versions 0 to 3 as
  `AST_FRAME_MODEM` frames. When we ask, we offer version 0. When the
  other end asks, we answer with version 3. The two sides use the lower
  of their two versions.
- We accept IFP packets of up to 400 octets.
- The UDPTL transport and its redundancy are from Asterisk.
- The clock sets the rate of the packets.

At the end of the fax, the module asks the channel to go back to audio
(Asterisk sends a re-INVITE again) and waits up to 5 s for that.

The T.38 gateway of Asterisk (`FAXOPT(gateway)=yes`) puts a fax machine
on the audio at the other end. At the end of a fax that went through
T.38, the console shows `over T.38`.

## The channel

The modem must get the audio of the line exactly as it is:

- **G.711 only (ulaw or alaw), with no transcoding.** A codec change,
  also between the two laws, prevents V.90 and decreases the V.34 rate.
  The `law` of the profile selects the law. `auto` uses the law of the
  channel (alaw if the channel has alaw and not ulaw, if not ulaw).
- **No processing in the path.** No echo cancellation, jitter buffer,
  silence suppression or comfort noise between the modem and the
  network, and no DSP functions (fax or DTMF detection) on the channel.
  For a SIP trunk, use a `pjsip.conf` endpoint similar to this:

  ```
  [modem-trunk]
  type = endpoint
  disallow = all
  allow = ulaw          ; or alaw: the law of the PSTN side
  direct_media = no
  ```

- **A stable RTP stream.** Packet loss and clock slips cause line
  errors. V.42 sends the data again, but these errors can stop a fast
  connection. When no frame arrives for 200 ms (a channel with no clock,
  or silence suppression), the module gives silence to the modem and
  continues.

### Clear channel (ISDN data calls)

- **An ISDN data call** (an unrestricted digital bearer, for example
  from chan_dahdi on a BRI or PRI span) is a clear 64 kbit/s channel.
  The modem runs `AT+MS=CLEAR` on the octets of the channel exactly as
  they are, to communicate with the clear channel of a second D-Modem.
  The link layer on it is X.75, unless the `init` of the profile selects
  a different one with `AT+B2` (`AT+B2=5` sync PPP, `AT+B2=7` the modem
  link). The `clear` of the profile selects:
  - `auto` (the default): a clear channel for a call with a digital
    bearer;
  - `yes`: all calls;
  - `no`: never.

  Refer to [isdn.md](isdn.md).
- **A SIP call in the CLEARMODE of RFC 4040** is also clear, with all
  values of `clear`. `codec_clearmode.so` gives Asterisk the CLEARMODE
  codec, and `allow = clearmode` on an endpoint offers and accepts it.
  The build makes and installs `codec_clearmode.so` when it finds the
  headers of Asterisk (`DMODEM_ASTERISK`, on by default), with or without
  app_dmodem. It contains no D-Modem code, and you can distribute it.

## Building

The module builds outside the Asterisk tree, with the remaining parts of
D-Modem. It needs the headers of an installed Asterisk of the same
version. Run `make install-headers` after you build Asterisk, or install
the `asterisk-dev` package of your distribution. If there are no
headers, CMake does not build it. It builds only when you ask for it,
for your own Asterisk (refer to the top of this page):

```
cmake -B build -DDMODEM_APP_DMODEM=ON   # in the top directory
cmake --build build --target app_dmodem
cmake --install build                   # the module, and dmodem.conf.sample
asterisk -rx "module load app_dmodem.so"
```

CMake options for other locations:

- `-DASTERISK_INCLUDE_DIR=/opt/asterisk/include` for headers in a
  different location;
- `-DASTERISK_MODULES_DIR=...` for the directory where `cmake --install`
  puts the module (default: the modules directory of Asterisk, for
  example `/usr/lib/asterisk/modules`);
- `-DASTERISK_CONF_DIR=...` for the directory where it puts the sample
  configuration (default `/etc/asterisk`).

The test scripts in `test/` use the binaries from `build/`
(`DMODEM_BUILD` gives a different build directory).

`DModem()` reads `dmodem.conf` on each call. Thus a change applies from
the next call. The module reads `debug` when it loads and on
`module reload`.

Each call runs its modem in the thread of its channel. A separate
short-lived thread makes the connection to a TCP or unix-socket server.
Thus the audio continues during the connection. The only limit on the
number of calls is the CPU: a V.34 or V.90 call uses a few percent of a
core.

## What the server or program sees

The server or program receives nothing until the modems are connected.
Then it receives the bytes from the remote modem, after V.42 and V.44 or
V.42bis. A server in `mode = unix` (a stream socket at `path`) receives
exactly the same data as a server in `mode = tcp`.

How the call ends:

- When the server or program ends its side (it closes the socket, or the
  program stops), the module hangs up when all the data that it sent
  last is on the line. This occurs when no data remains to send and a
  minimum of 1 s has passed, or after 30 s maximum.
- When the remote modem hangs up, the module closes the socket, or hangs
  up the pty (the program gets SIGHUP). If a program continues to run
  5 s after the SIGHUP, the module stops it with SIGKILL.

A program in `mode = exec` gets the pty as its stdin, stdout, stderr and
controlling terminal, in raw mode. Its environment has:

| variable | value |
|---|---|
| `DMODEM_CHANNEL` | the Asterisk channel |
| `DMODEM_PROFILE` | the profile |
| `DMODEM_RATE` | the connect rate, transmit/receive bit/s |
| `DMODEM_CALLERID` | the number of the caller |
| `DMODEM_DIALED` | the dialled number (the DNID, or if not, the extension) |
| `DMODEM_CONNECTED` | the time when the modems connected, in Unix time |
| `DMODEM_PEER` | the address from which the call came through SIP (the `remote_addr` of PJSIP, the `peerip` of chan_sip), or empty |

A server has no environment. Instead, with `header = yes` in a `tcp` or
`unix` profile, the module first sends the same variables on the
connection: one `NAME=value` on each line (a line break in a value
changes to a space), then an empty line, then the bytes of the caller:

```
DMODEM_CHANNEL=PJSIP/trunk-00000001
DMODEM_PROFILE=menu
DMODEM_RATE=33600/31200
DMODEM_CALLERID=5559999
DMODEM_DIALED=5551000
DMODEM_CONNECTED=1791666309
DMODEM_PEER=192.0.2.1:5060

```

[`modemmenu`](https://github.com/D-Modem/menu) uses these variables. It
gives a login (from a text file, LDAP, SQL or PAM) and a menu of a BBS,
PPP and a shell. Each of these starts with the credentials of the
caller and these details of the call. It runs as a `mode = exec` program
for each call, or as one server for all calls (`modemmenu -serve`)
behind a `mode = unix` profile with `header = yes`.

Each call is a separate connection to the server of the profile. Many
callers at the same time on one `modemmenu -serve` (or BBS) socket are
many connections, each with its own header.

With `handoff = yes` also (`unix` and `header` only), the module puts
the modem on a pty and gives the terminal of the pty to the server. It
does not copy bytes to the server. The header goes on the connection
with the descriptor attached (`SCM_RIGHTS`). From then on, the server,
and all that it runs on that terminal (a login session, a shell, pppd),
is directly on the line of the modem, as on a serial port. The connection carries no more data. It stays open
while the session continues. When the server closes it, the module
waits until the last data from the server goes out, then hangs up
(`LOCAL`). When the remote modem goes, the server sees that its end of
the line closes. The server must expect the descriptor
(`modemmenu -serve` and the `handoff` listeners of the BBS do). A server that does
not expect it gets the header and nothing after it.

When it returns, `DModem()` sets:

- `DMODEMSTATUS`:
  - `NOCARRIER`: the modems did not connect (this includes no connection
    in the `timeout` of the profile);
  - `REMOTE`: the remote modem disconnected;
  - `LOCAL`: the server or program ended the session;
  - `HANGUP`: the channel hung up;
  - `FAX`: a fax machine called and the fax receiver accepted the call;
  - `ERROR`: a configuration or system error (for example: no such
    profile, an `answer` line that did not answer, or no connection to
    a server).
- `DMODEMRATE`: the connect rate, transmit/receive bit/s.

## Testing

The tests start a private Asterisk: the installed one, with the module
built here. They do not change the Asterisk of the system.

### run.sh: a modem call

`test/run.sh` calls one `DModem()` from a second one through a Local
channel:

- The answering `DModem()` serves a test BBS (`test/bbs_sim.py`).
- The calling one (`DModem(term,o)`) sends its data to a script that
  acts as the user.
- The user reads the banner, sends 20 lines, checks their echoes and
  logs off. Then the BBS closes, and this hangs up the call.

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

The first argument goes into the `init` of the calling profile. `EXPECT`
is a regular expression. A line of the Asterisk console must match it,
or the test fails.

A Local channel has no clock. Thus the two modems run sample by sample,
faster than real time. This causes no problem for them.

### fax.sh and fax_spandsp.sh: faxes on the audio

`test/fax.sh` sends a two-page fax (the test document of
`dsp/test/fax_doc`) from `DModemSendFax`:

1. to `DModem()`, which must identify the fax machine automatically;
2. to `DModemReceiveFax`.

It compares the received data with the sent data.

`test/fax_spandsp.sh` does the same with the fax of Asterisk
(`res_fax_spandsp`):

1. `SendFAX` to `DModem()`;
2. `SendFAX` to `DModemReceiveFax`;
3. `DModemSendFax` to `ReceiveFAX`.

They run at the default highest rate of 14400 bit/s. Variables:
`CASES` (for example, `CASES="2 3"` runs only those cases),
`FAX_MAX_RATE` (for example `9600`: the fax of the module goes no
faster), and `DMODEM_DEBUG=3` (the DSP traces in `console.log`).

### t38.sh: faxes over T.38

`test/t38.sh` makes Asterisk call itself through PJSIP: out on
`loopout`, in on `loopin`, both with `t38_udptl`. The cases:

1. `SendFAX` to `DModemReceiveFax`, through T.38;
2. `DModemSendFax` to `ReceiveFAX`, through T.38;
3. `DModemSendFax` to `DModemReceiveFax`, through T.38;
4. `SendFAX` to `DModem()`, through T.38;
5. `SendFAX` from an endpoint without T.38 (`loopaudio`) to
   `DModemReceiveFax`: the endpoint refuses T.38, thus the fax goes on
   the audio;
6. through the T.38 gateway of Asterisk, `SendFAX` on the audio to
   `DModem()` through T.38;
7. through the gateway, `DModemSendFax` to `ReceiveFAX` on the audio
   (the gateway asks for T.38 when it receives the DIS);
8. `DModemReceiveFax` to `DModem()`, which accepts its T.38 request
   before the modems connect as a calling fax machine. The two ends are
   receivers, thus there is no document. The test checks only this.

Each document received in cases 1 to 7 must be the same as the sent
document. Variables: `CASES`, `DMODEM_DEBUG=3`, `SIP_LOG=1` (the SIP
messages in the console) and `FAX_DEBUG=1` (the debug output of
spandsp).

Before the two gateway cases, `t38.sh` tries the gateway of Asterisk
alone (`SendFAX` on the audio through it to `ReceiveFAX` through T.38).
Asterisk 20.6 of Ubuntu 24.04 sends no fax there. Thus on it, the script
skips cases 6 and 7, and tells you (`GATEWAY=yes` runs them). CI builds
Asterisk 22 (LTS) from source with `test/build_asterisk.sh
<prefix>`, where all the cases run. The header of the script gives the
packages that it needs and how to make the tests use it: `PATH`,
`AST_MODULES`, `AST_DATA`, and `-DASTERISK_INCLUDE_DIR` for the build.

`ARI=1` runs these tests (and `test/sip.sh`) through
[dmodem-ari](ari-details.md#testing) instead of the module.

### sip.sh: dmodemd over SIP

`test/sip.sh` makes the `dmodemd` of D-Modem call `DModem(bbs)` through
SIP (`INCOMING=1`: answer a call from `DModem(term,o)`). It uses the
same test BBS. `CODEC=alaw` or `ulaw` limits the call to one law.
`CODEC=clearmode` runs it in the clear channel of RFC 4040 and checks
for `DMODEMRATE=64000/64000`.

**Voice, incoming (`VOICE=1 test/sip.sh`).** Asterisk calls dmodemd,
which answers in voice mode with `AT+FCLASS=8` and `AT+VLS=1`. It sets
`AT+VTD=10` and plays `AT+VTS=1,!,{!,30},2`, while the dialplan sends
it the DTMF digit 5. The test checks that:

- the RTP debug of Asterisk shows a minimum of two end packets of RFC
  4733 event 16 (the hook flashes);
- the log of dmodemd shows the two sent flashes, of 100 and 300 ms, and
  the digit 5 received as an RFC 4733 event and played as its tone;
- the voice engine sends the digit to the pty as `<DLE>5` ...
  `<DLE>~`.

**Voice, outgoing (`VOICE_DIAL=1 test/sip.sh`).** dmodemd dials in
voice mode (`AT+FCLASS=8`, `ATD`) to the extensions of the dialplan in
`modem-in`. First with `S7=6` (6 s) and no early media:

| extension | what it does | expected result |
|---|---|---|
| 7002 | busy | `BUSY` |
| 7003 | rings, never answers | `NO ANSWER` after S7 (6 s) |
| 7001 | rings 2 s, answers, sends the digit 7, hangs up approximately 4 s later | `OK` on the answer, then `<DLE>7` ... `<DLE>~`, `OK` to `AT+VTS=1,2`, and only `<DLE>l` at the hang-up (no `NO CARRIER`) |

Then with `S7=15` and early media (183 Session Progress):

| extension | what it does | expected result |
|---|---|---|
| 7004 | ringing tone for 7 s, then a 200 OK | a minimum of two rings (`<DLE>r`), then `OK` at the 200 OK |
| 7005 | busy tone, its 486 only after 15 s | `BUSY` from the tone, before the 486 |
| 7007 | answer tone (2100 Hz), never answers | `<DLE>a` and `OK` |
| 7006 | no such number (404) | `ERROR` |

After that, `ATH` must stop voice mode (`+FCLASS: 0`), and a data call
to 7002 must give `BUSY`.

### efax.sh: dmodemd's class 1 fax

`test/efax.sh` runs efax on the pty of dmodemd (fax class 1) with the
fax of Asterisk through PJSIP (`t38_udptl`):

1. efax sends: dmodemd calls `ReceiveFAX`, which asks for T.38.
2. efax receives: `SendFAX` calls dmodemd, which answers and asks for
   T.38 one second after it answers.

The two go through T.38. Options:

- `T38=no`: the endpoint without T.38 (the endpoint refuses the request
  of dmodemd, and the faxes go on the audio);
- `DMODEMD_T38=no`: dmodemd without T.38 (dmodemd refuses the request of
  Asterisk).

The pages that efax receives must be the pages of the document. The
pages that it sends have a header from efax, and must arrive with the
rows that it sent. The test needs efax and libtiff-tools (`tiffcp`).
Other variables: `CASES`, `DMODEMD_DEBUG`, `SIP_LOG=1`, `FAX_DEBUG=1`,
`EFAX_V` and `EFAX_STRACE=1`.

## Debugging

`debug = 3` in `[general]` with `core set verbose 5` shows the
handshake of the modem on the console (`debug`: 0 errors only, 1 the
modem core, 3 the data pumps also; the lines of libdmodem show at
verbose level 5, the progress of the call at level 4 and higher). The
same modem also runs outside Asterisk:

- in `dsp/test/dmodem_test` (two modems of the library back to back);
- with the original binary library of Smart Link in
  `dsp/test/smartlink_test` (`interop_call`).
