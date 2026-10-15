# dmodem-ari: D-Modem as an Asterisk ARI application

`dmodem-ari` answers (or makes) Asterisk calls with D-Modem's modem, like
`DModem()`, but as a program of its own beside Asterisk instead of a module
inside it. It talks to Asterisk over the Asterisk REST Interface (ARI): the
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
4. libdmodem's modem on the packets: each packet that comes is a block of
   the line, and the modem's answer goes back as a packet of the same
   length, so the call's own clock paces the modem (200 ms without a
   packet: the modem goes on, on silence);
5. once the modems are connected, the data to the profile's server or
   program, by the PBX modules' bridge (`pbx/dmodem_bridge.c`), with the
   call's details (`DMODEM_CHANNEL`, `DMODEM_PROFILE`, `DMODEM_RATE`,
   `DMODEM_CALLERID`, `DMODEM_DIALED`, `DMODEM_CONNECTED`) in the program's
   environment or the server's header;
6. at the end the external media channel and the bridge deleted, the
   variables set, and the call continued in the dialplan.

## Limits of the prototype

- A fax machine calling a modem profile ends the call (`DMODEMSTATUS`
  `FAX`); `DModemSendFax` and `DModemReceiveFax` have no counterpart yet.
- The channel needs a clock: a SIP or PSTN call has one. Two `Stasis()`
  calls on the two halves of a Local channel do not (each side waits for
  the other's packets).
- ARI over plain HTTP: Asterisk on the same host, or a trusted network. The
  WebSocket's `Sec-WebSocket-Accept` is not checked.
- One thread a call, and the RTP on 127.0.0.1: `dmodem-ari` runs on
  Asterisk's host.

## Testing

`ARI=1 asterisk/test/sip.sh` runs the SIP test (D-Modem's dmodemd calling
over SIP, see [asterisk.md](asterisk.md#testing)) through `dmodem-ari`
instead of `DModem()`: Asterisk's HTTP server and ARI turned on, the
dialplan's `DModem(...)` as `Stasis(dmodem,...)`. Its variants work the same
way (`INCOMING=1` for the modem calling, `CALLS=3`, `CODEC=alaw`, ...); they
connect at 62666/62666 bit/s, V.91, which only exact G.711 codes allow.
