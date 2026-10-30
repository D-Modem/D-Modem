# app_dmodem: a data modem on an Asterisk channel: technical details

This is the technical companion to [Asterisk: app_dmodem](asterisk.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

> **Build it yourself; don't ship it.** A built `app_dmodem.so` has
> libdmodem (AGPL-3.0) inside a module of Asterisk (GPL-2.0-only), two
> licences that can't be combined, so it may not be distributed
> ([LICENSING.md](../LICENSING.md)). It is off by default: build it with
> `-DDMODEM_APP_DMODEM=ON` for your own Asterisk. [dmodem-ari](ari.md),
> the same modem and profiles beside Asterisk, is the way to ship D-Modem
> for Asterisk.

`DModem()` answers a call with a software modem and, once the modems are
connected, hands the data to a server over TCP or a unix socket (a BBS,
a login menu) or to a program on a pty (pppd for dial-up IP, a login, a
door).  The profile in `dmodem.conf`
decides which, so one Asterisk can serve several numbers in several ways:

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

See [dmodem.conf.sample](../asterisk/dmodem.conf.sample) for all the options.

The same calls and faxes can go to [dmodem-ari](ari.md) instead, D-Modem
as an ARI application beside Asterisk (`Stasis(dmodem,bbs)`,
`Stasis(dmodem,receivefax,<file>)`), with the same profiles: no module in
Asterisk. Its faxes go over T.38 as the module's do ([ari.md](ari-details.md#t38)).

The modem answers with `ATA`. A profile's `answer` gives the command line
it answers with instead, with commands for the answering call alone
before the `A`: `answer = AT+MS=132\;A` answers at V.32bis at most (in
`dmodem.conf` a `;` is written `\;`). `init` has the commands for both
directions. A line that leaves the modem not answering ends the call,
`DMODEMSTATUS` `ERROR`.

`DModem(profile,o)` makes the modem the caller instead (it waits for the
remote modem's answer tone), on a call Asterisk has placed to a remote
modem, e.g. `channel originate PJSIP/5559999@trunk application
DModem(isp,o)`; the profile's server or program then talks to the remote
BBS or ISP.

The modem is `libdmodem` ([dmodemd/dmodem.h](../dmodemd/dmodem.h)):
D-Modem's modem core (AT commands, V.42 error correction, V.44 or V.42bis
compression) with the open data pumps of `dsp`.  It answers
V.21, V.22, V.22bis, V.23, Bell 103/212A, V.32, V.32bis and V.34, and
V.90 as the digital (server) modem: up to 56000 bit/s downstream to a
V.90 client modem when the call is G.711 end to end.  The module has no
binary-only parts.

## Fax

The module is also a group 3 fax machine of its own (T.30 on V.17, V.29
and V.27ter, 2400 to 14400 bit/s, pages in MH or MR coding, TIFF files,
error correction mode when the other machine has it), an alternative to
Asterisk's `res_fax` with spandsp:

- `DModem()` receives faxes by itself: when the calling machine's CNG
  tone, or its V.8 CM of a fax call, comes before a modem connection
  (the modem answers adaptively: `dmodem_status()`'s `fax_caller`), the
  call goes to the fax receiver, which writes `<fax_dir>/<date>-<time>-<caller>-<uniqueid>.tif`
  (`fax = no` in the profile turns this off).  `DMODEMSTATUS` is then
  `FAX`.
- `DModemReceiveFax(file[,ident])` answers as a fax machine and receives
  into the file.
- `DModemSendFax(file[,ident])` sends a TIFF file on a call placed to a
  fax machine, e.g.

  ```
  channel originate PJSIP/5551234@trunk application DModemSendFax /var/spool/fax/doc.tif
  ```

  Any TIFF of 1 bit pels goes: uncompressed, G3 1D/2D or G4, of any width
  (fitted to the fax's 1728 pels) and resolution (standard, fine;
  superfine is halved).  `gs -sDEVICE=tiffg4 -r204x196 -o doc.tif doc.pdf`
  makes one from a PDF.

They set `DMODEMFAXSTATUS` (`SUCCESS` or `FAILED`), `DMODEMFAXRESULT`
(why), `DMODEMFAXPAGES`, `DMODEMFAXRATE`, `DMODEMFAXREMOTE` (the other
machine's number), `DMODEMFAXECM` (`yes` when the call had error
correction mode), `DMODEMFAXT38` (`yes` when the fax went over T.38) and
`DMODEMFAXFILE`.  `fax_dir`, `fax_ident` (our number), `fax_max_rate`,
`fax_ecm` and `fax_t38` are set in `[general]` or per profile.

In error correction mode (T.30 Annex A) the page goes in blocks of HDLC
frames, and the receiving machine asks again for the frames a line hit
spoiled: the page comes as it went, where without it the rows hit come
damaged.  Both machines offer it (`fax_ecm = no` to go without).

### T.38

On a channel that has T.38 (PJSIP, an endpoint with `t38_udptl = yes`)
the faxes go over T.38 when the channel does, as `SendFAX` and
`ReceiveFAX` have it (`fax_t38 = no` to keep them on the audio):

- `DModemReceiveFax` asks for T.38 at once (Asterisk re-INVITEs) and
  waits for the answer, 6 s at most, with silence on the line; refused,
  the fax goes on the audio.
- `DModemSendFax` takes T.38 when the other end asks for it, until a
  page has gone.
- `DModem()` takes a T.38 offer that comes before the modems connect as
  a fax machine calling (a gateway's), and a fax machine it hears on the
  audio asks for T.38 as `DModemReceiveFax` does.

The fax machine is then a T.38 terminal (libdmodem's): the IFP packets
of T.38 versions 0 to 3 as `AST_FRAME_MODEM` frames, the UDPTL and its
redundancy Asterisk's, the packets paced by the clock.  At the end the
channel goes back to the audio (Asterisk re-INVITEs again).  Asterisk's
T.38 gateway (`FAXOPT(gateway)=yes`) puts a fax machine on the audio
at the other end; the console says `over T.38` at the end of a fax
that went so.

## The channel

The modem needs the line's audio as it is, so:

- G.711 (ulaw or alaw) only, no transcoding: a codec change, even
  between the two laws, costs V.90 and lowers V.34.  `law` in the
  profile picks the law, `auto` takes the channel's.
- No echo cancellation, jitter buffer, silence suppression or comfort
  noise between the modem and the network, and no DSP features (fax or
  DTMF detection) on the channel.  For a SIP trunk, a `pjsip.conf`
  endpoint along the lines of:

  ```
  [modem-trunk]
  type = endpoint
  disallow = all
  allow = ulaw          ; or alaw: the law of the PSTN side
  direct_media = no
  ```

- The RTP stream should be steady: packet loss or clock slips show as
  line errors, which V.42 retransmits but which can drop a fast connect.
- An ISDN data call (an unrestricted digital bearer, chan_dahdi's on a
  BRI or PRI span) is a clear 64 kbit/s channel: the modem runs
  `AT+MS=CLEAR` on the channel's octets as they are, for another
  D-Modem's clear channel.  The profile's `clear` (`auto`, `yes` or
  `no`) chooses; see [isdn.md](isdn.md).
- A SIP call in RFC 4040's CLEARMODE is one too: `codec_clearmode.so`
  (built and installed with the module) gives Asterisk the codec, and
  `allow = clearmode` on an endpoint offers and takes it.

## Building

The module builds outside the Asterisk tree, with the rest of D-Modem,
against the headers of an installed Asterisk of the same version (`make
install-headers` after building Asterisk, or a distribution's
`asterisk-dev` package); without them CMake skips it.  It is built only
when asked for, for your own Asterisk (see the top of this page):

```
cmake -B build -DDMODEM_APP_DMODEM=ON   # in the top directory
cmake --build build --target app_dmodem
cmake --install build               # the module, and dmodem.conf.sample
asterisk -rx "module load app_dmodem.so"
```

`-DASTERISK_INCLUDE_DIR=/opt/asterisk/include` for headers elsewhere,
`-DASTERISK_MODULES_DIR=...` and `-DASTERISK_CONF_DIR=...` for where
`cmake --install` puts the module and the sample configuration.  The
test scripts in `test/` take the binaries from `build/` (`DMODEM_BUILD`
for another build directory).  Each call
runs its modem in the channel's own thread; there is no limit on the
number of calls other than CPU (a V.34 or V.90 call takes a few percent
of a core).

## What the server or program sees

Nothing until the modems are connected; then the bytes from the remote
modem, after V.42 and V.44 or V.42bis.  A server in `mode = unix` (a
stream socket at `path`) sees the same as one in `mode = tcp`.  The server or program ending its side
(closing the socket, the program exiting) hangs up the call once what it
sent last is on the line; the remote modem hanging up closes the socket
or hangs up the pty (SIGHUP).

A program in `mode = exec` gets the pty as its stdin, stdout, stderr and
controlling terminal (raw mode), and in its environment:

| variable | |
|---|---|
| `DMODEM_CHANNEL` | the Asterisk channel |
| `DMODEM_PROFILE` | the profile |
| `DMODEM_RATE` | the connect rate, transmit/receive bit/s |
| `DMODEM_CALLERID` | the caller's number |
| `DMODEM_DIALED` | the number dialed (the DNID, or the extension) |
| `DMODEM_CONNECTED` | when the modems connected, Unix time |

A server has no environment: with `header = yes` in a `tcp` or `unix`
profile, the same variables come first on the connection, one
`NAME=value` per line (values on one line), then an empty line, then the
caller's bytes:

```
DMODEM_CHANNEL=PJSIP/trunk-00000001
DMODEM_PROFILE=menu
DMODEM_RATE=33600/31200
DMODEM_CALLERID=5559999
DMODEM_DIALED=5551000
DMODEM_CONNECTED=1791666309

```

[`modemmenu`](https://github.com/D-Modem/menu) uses these: a
login (a text file, LDAP, SQL or PAM) and a menu of a BBS, PPP and a
shell, started with the caller's credentials and these details of the
call.  It runs as a `mode = exec` program for each call, or as one
server for all of them (`modemmenu -serve`) behind a `mode = unix`
profile with `header = yes`.

On exit `DModem()` sets `DMODEMSTATUS` (`NOCARRIER`, `REMOTE`, `LOCAL`,
`HANGUP` or `ERROR`) and `DMODEMRATE`.

## Testing

`test/run.sh` starts a private Asterisk (the installed one, with the
module built here) and calls one `DModem()` from another over a Local
channel: the answering one serves a test BBS, the calling one
(`DModem(term,o)`) hands its data to a script playing the user, who
reads the banner, sends 20 lines, checks their echoes and logs off;
the BBS closing then hangs up the call.

```
test/run.sh                       # V.34
test/run.sh AT+MS=122             # V.22bis (also 22, 32, 132)
test/run.sh AT+MS=34 bbsexec      # the BBS as a program on the pty
test/run.sh "" bbsunix            # the BBS on a unix socket, the call's
                                  # details first (header = yes)
LAW=alaw test/run.sh              # on A-law
ANSWER='AT+MS=132\;A' EXPECT='ANSWERER .*DMODEMRATE=14400/' test/run.sh
                                  # the answer command line: V.32bis
```

`EXPECT` is a regular expression that a line of Asterisk's console must
match, or the test fails.

A Local channel has no clock: the two modems run sample for sample
faster than real time, which they don't mind.

`test/fax.sh` sends a two page fax from `DModemSendFax` to `DModem()`
(which has to notice the fax machine) and to `DModemReceiveFax`, and
compares what came with what went.  `test/fax_spandsp.sh` does the same
against Asterisk's own fax (`res_fax_spandsp`): `SendFAX` to `DModem()`
and to `DModemReceiveFax`, and `DModemSendFax` to `ReceiveFAX`, all at
14400 bit/s (`CASES`, `FAX_MAX_RATE`, `DMODEM_DEBUG` in the script).

`test/t38.sh` has the faxes over T.38, with Asterisk calling itself over
PJSIP (out on `loopout`, in on `loopin`, both with `t38_udptl`):
`SendFAX` to `DModemReceiveFax`, `DModemSendFax` to `ReceiveFAX` and to
`DModemReceiveFax`, `SendFAX` to `DModem()`, all of them over T.38;
`SendFAX` from an endpoint without T.38 to `DModemReceiveFax` (T.38
refused, on the audio); through Asterisk's T.38 gateway, `SendFAX` on
the audio to `DModem()` and `DModemSendFax` to `ReceiveFAX` on the audio
(the gateway asks for T.38 when it hears the DIS); and `DModemReceiveFax`
to `DModem()`, which takes its T.38 request before the modems connect as
a fax machine calling (two receivers: no document, only that is
checked).  `CASES`, `SIP_LOG=1` (the SIP messages in the console),
`FAX_DEBUG=1` (spandsp's).

Before the two gateway cases `t38.sh` tries Asterisk's gateway by
itself (`SendFAX` on the audio through it to `ReceiveFAX` over T.38).
Ubuntu 24.04's Asterisk 20.6 passes no fax there, so on it those two
cases are skipped and say so (`GATEWAY=yes` runs them anyway).  CI
builds Asterisk 22 (LTS) from source with `test/build_asterisk.sh
<prefix>`, where all of them run; the script's header lists the
packages it needs and how to point the tests at it (`PATH`,
`AST_MODULES`, `AST_DATA`, and `-DASTERISK_INCLUDE_DIR` for the build).

`ARI=1` runs these tests (and `test/sip.sh`) through
[dmodem-ari](ari-details.md#testing) instead of the module.

`VOICE=1 test/sip.sh` is dmodemd in voice mode: Asterisk calls it, it
answers with `AT+FCLASS=8` and `AT+VLS=1`, and plays
`AT+VTS=1,!,{!,30},2` (with `AT+VTD=10`) while the dialplan sends it the
DTMF digit 5.  Asterisk's RTP debug must show the two hook flashes as
RFC 4733 event 16 (their end packets: 100 and 300 ms), dmodemd's log
the flashes sent and the digit received as an event and played as its
tone, and the voice engine must report the digit to the pty as
`<DLE>5` ... `<DLE>~`.

`VOICE_DIAL=1 test/sip.sh` has dmodemd dial in voice mode
(`AT+FCLASS=8`, `ATD`) to the dialplan's extensions in `modem-in`: 7002
(busy) must give BUSY, 7003 (ringing, never answered) NO ANSWER after
S7 (6 s), and 7001 (rings for 2 s, answers, sends the digit 7 and hangs
up 4 s on) OK when it answers, then `<DLE>7` ... `<DLE>~`, OK to
`AT+VTS=1,2`, and `<DLE>l` alone at the hang-up (no NO CARRIER).  With
early media (183, S7 15 s): 7004's ringing tone must come as two rings
(`<DLE>r`) and OK at its 200 OK 7 s on, 7005's busy tone give BUSY
before its 486 (15 s on), and 7007's answer tone (2100 Hz) `<DLE>a` and
OK although it never answers.  7006 (no such number, 404) must give
ERROR.  `ATH` must then leave voice mode (`+FCLASS: 0`), and a data
call to 7002 give BUSY.

`test/efax.sh` is dmodemd's fax (class 1) with efax on its pty against
Asterisk's own over PJSIP (`t38_udptl`): efax sends, dmodemd calling
`ReceiveFAX` (which asks for T.38), and efax receives, `SendFAX` calling
dmodemd (which asks for it a second after it answers); both over T.38.
`T38=no` has the endpoint without T.38 (dmodemd's ask refused, the faxes
on the audio), `DMODEMD_T38=no` dmodemd without it (Asterisk's refused).
The pages efax receives must be the document's; those it sends carry a
header of its own, and must have come with the rows it sent.  Needs efax
(and libtiff-tools); `CASES`, `DMODEMD_DEBUG`, `SIP_LOG=1`, `FAX_DEBUG=1`.

## Debugging

`debug = 3` in `[general]` and `core set verbose 5` show the modem's
handshake on the console.  The same modem runs outside Asterisk in
`dsp/test/dmodem_test` (two modems of the library back to back)
and against Smart Link's original binary library in
`dsp/test/smartlink_test` (`interop_call`).
