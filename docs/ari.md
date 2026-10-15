# dmodem-ari: D-Modem as an Asterisk ARI application

`dmodem-ari` answers (or makes) Asterisk calls with D-Modem's modem, like
`DModem()`, and sends and receives faxes with its fax machine, like
`DModemSendFax()` and `DModemReceiveFax()`, but as a program of its own
beside Asterisk instead of a module inside it. It talks to Asterisk over the Asterisk REST Interface (ARI): the
events on a WebSocket, the requests over HTTP, and each call's audio over
RTP from an ARI external media channel, in the call's G.711 law. Nothing of
D-Modem is loaded into Asterisk.

This keeps the modem's licence apart from Asterisk's. A loadable module is
under the GPL with Asterisk (Asterisk's `LICENSE`: "the GPL applies to all
loadable Asterisk modules used on your system"); applications that control
Asterisk through AMI, AGI or ARI need not be, and Sangoma grants a licence
for that use in any case. `dmodem-ari` is under the AGPL-3.0, or a
commercial licence (see [LICENSING.md](../LICENSING.md)), whatever
Asterisk's terms are.

## Setting up

Asterisk's HTTP server and an ARI user:

```
; http.conf
[general]
enabled = yes
bindaddr = 127.0.0.1
bindport = 8088

; ari.conf
[general]
enabled = yes
[dmodem]
type = user
read_only = no
password = secret
```

The dialplan hands calls to the app, with the profile and the options of
`DModem()`:

```
exten => 5551000,1,Stasis(dmodem,bbs)
 same => n,Verbose(1,${DMODEMSTATUS} ${DMODEMRATE})
 same => n,Hangup()
; the modem calls (DModem(profile,o)): on a call Asterisk has placed
exten => s,1,Stasis(dmodem,isp,o)
```

When the call is over, `dmodem-ari` sets `DMODEMSTATUS` and `DMODEMRATE`
as `DModem()` does, and the call goes on in the dialplan after the
`Stasis()` (unless it has hung up).

Faxes take `sendfax` or `receivefax` in place of the profile, then the
file and our number, as `DModemSendFax(file[,ident])` and
`DModemReceiveFax(file[,ident])` do (see [Faxes](#faxes)):

```
exten => 5551001,1,Stasis(dmodem,receivefax,/var/spool/fax/${UNIQUEID}.tif)
 same => n,Hangup()
exten => h,1,Verbose(1,${DMODEMFAXSTATUS} ${DMODEMFAXPAGES} ${DMODEMFAXFILE})
```

```
channel originate PJSIP/5551234@trunk application Stasis dmodem,sendfax,/var/spool/fax/doc.tif
```

`dmodem-ari` reads the same `dmodem.conf` as `app_dmodem` (the profiles
and their options, see [asterisk.md](asterisk.md)), and ARI's address
from `[general]`:

```
[general]
ari_url = http://127.0.0.1:8088
ari_user = dmodem
ari_password = secret
ari_app = dmodem        ; the Stasis() app's name
debug = 0               ; the modem's own log (1: the core, 3: the pumps)
```

```
dmodem-ari [-c /etc/asterisk/dmodem.conf] [-u http://host:port]
           [-U user:password] [-a app] [-v]...
```

`-u`, `-U` and `-a` override `[general]`. Each `-v` logs more on stderr:
the calls (answering, connected, done), then the bridge's details. It
reconnects to ARI a second after the WebSocket goes, and calls in progress
go on meanwhile.

## How a call goes

For each call that enters the app:

1. the channel answered (unless it is up already, as on a call Asterisk
   placed);
2. a UDP port of `dmodem-ari`'s on 127.0.0.1, and an external media channel
   (`POST /channels/externalMedia`, RTP over UDP) that sends the call's
   audio there, in the call's law: the profile's `law`, else the channel's
   own;
3. the call's channel and the external media channel in a bridge of their
   own. With both in the same law Asterisk takes its `simple_bridge`, which
   passes the frames as they are: V.90's and V.91's PCM codes arrive
   unchanged;
4. libdmodem's modem (or fax machine) on the packets: each packet that
   comes is a block of the line, and the modem's answer goes back as a
   packet of the same length, so the call's own clock paces the modem
   (200 ms without a packet: the modem goes on, on silence). A channel
   without a clock, such as the two halves of a Local channel with a
   `Stasis()` on each, works too: the two sides pace each other, faster
   than real time, as two `DModem()` do;
5. once the modems are connected, the data to the profile's server or
   program, by the PBX modules' bridge (`pbx/dmodem_bridge.c`), with the
   call's details (`DMODEM_CHANNEL`, `DMODEM_PROFILE`, `DMODEM_RATE`,
   `DMODEM_CALLERID`, `DMODEM_DIALED`, `DMODEM_CONNECTED`) in the program's
   environment or the server's header;
6. at the end the external media channel and the bridge deleted, the
   variables set, and the call continued in the dialplan.

## Faxes

The fax machine is `app_dmodem`'s (T.30 on V.17, V.29 and V.27ter, error
correction mode, TIFF files; see [asterisk.md](asterisk.md#fax)), with its
settings from `dmodem.conf`'s `[general]`: `fax_ident`, `fax_max_rate`,
`fax_ecm`, and `fax_dir` for the faxes the modem profiles take.

- A fax machine that calls a modem profile (`fax` on, the default) is
  received into `<fax_dir>/<date>-<time>-<caller>-<uniqueid>.tif`, as
  `DModem()` does, with the profile's fax settings. `DMODEMSTATUS` is
  `FAX`, set as soon as the fax machine is heard.
- `Stasis(dmodem,receivefax,<file>[,<ident>])` answers as a fax machine and
  receives into the file.
- `Stasis(dmodem,sendfax,<file>[,<ident>])` sends a TIFF file (any TIFF of
  1 bit pels, as `DModemSendFax`), on a call placed to a fax machine.

`<ident>` is our number instead of `fax_ident`. They set the variables of
`DModemSendFax` and `DModemReceiveFax`: `DMODEMFAXFILE` first, then
`DMODEMFAXSTATUS` (`SUCCESS` or `FAILED`), `DMODEMFAXRESULT`,
`DMODEMFAXPAGES`, `DMODEMFAXRATE`, `DMODEMFAXREMOTE` and `DMODEMFAXECM`.

One thing differs from the module. An ARI application cannot keep a
channel that has hung up: when the other machine hangs up, Asterisk ends
the `Stasis()` and runs the `h` extension at once, where
`DModemReceiveFax` would finish first. So `dmodem-ari` sets the result
as soon as it is final: when the MCF to the last page has gone (receiving)
or come (sending), with the received pages already in the file. The other
machine hangs up only after that (after the MCF, about a second, and its
DCN), so the `h` extension has the result of a fax that went through. A
fax that ends with the other machine hanging up before then leaves only
`DMODEMFAXFILE` set: read an empty `DMODEMFAXSTATUS` in `h` as `FAILED`.
The same goes for a modem call that the other side hangs up: no
`DMODEMSTATUS` in `h`, where `DModem()` sets `HANGUP`.

## Limits of the prototype

- The variables of a call that the other side hangs up come only as far
  as they are known before the hangup ([Faxes](#faxes)).
- ARI over plain HTTP: Asterisk on the same host, or a trusted network. The
  WebSocket's `Sec-WebSocket-Accept` is not checked.
- One thread a call, and the RTP on 127.0.0.1: `dmodem-ari` runs on
  Asterisk's host.

## Testing

`ARI=1` runs the Asterisk tests ([asterisk.md](asterisk.md#testing))
through `dmodem-ari` instead of the module (`asterisk/test/ari_env.sh`):
Asterisk's HTTP server and ARI turned on, `app_dmodem` not loaded, and
the dialplan's `DModem(...)`, `DModemSendFax(...)` and
`DModemReceiveFax(...)` as `Stasis(dmodem,...)`.

- `ARI=1 asterisk/test/sip.sh`: D-Modem's dmodemd calling over SIP. Its
  variants work the same way (`INCOMING=1` for the modem calling,
  `CALLS=3`, `CODEC=alaw`, ...); they connect at 62666/62666 bit/s, V.91,
  which only exact G.711 codes allow.
- `ARI=1 asterisk/test/run.sh`: two modems of `dmodem-ari` on the two
  halves of a Local channel (no clock), also at 62666/62666 bit/s.
- `ARI=1 asterisk/test/fax.sh` and `ARI=1 asterisk/test/fax_spandsp.sh`:
  the faxes, between `dmodem-ari`'s own machines (a modem profile
  receiving, `receivefax`, `sendfax`) and against Asterisk's `SendFAX` and
  `ReceiveFAX` (spandsp). The `h` extensions must have the results, and
  the received files the pages sent.
