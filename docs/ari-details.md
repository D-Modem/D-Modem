# dmodem-ari: D-Modem as an Asterisk ARI application: technical details

This is the technical companion to [dmodem-ari: D-Modem beside Asterisk](ari.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

`dmodem-ari` (`ari/`) does the job of the `app_dmodem` module, but as a
separate program beside Asterisk rather than a module inside it:

- It answers (or makes) Asterisk calls with D-Modem's modem, like
  `DModem()`.
- It sends and receives faxes with D-Modem's fax machine, like
  `DModemSendFax()` and `DModemReceiveFax()`, on the audio or over T.38
  (see [T.38](#t38)).

It talks to Asterisk over the Asterisk REST Interface (ARI):

- the events arrive on a WebSocket;
- the requests go over HTTP;
- each call's audio comes over RTP from an ARI *external media* channel,
  in the call's G.711 law.

Nothing of D-Modem is loaded into Asterisk.

### Why a separate program

This keeps the modem's licence apart from Asterisk's. A loadable module
falls under the GPL together with Asterisk (Asterisk's `LICENSE`: "the
GPL applies to all loadable Asterisk modules used on your system").
Applications that control Asterisk through AMI, AGI or ARI need not be
under the GPL, and Sangoma grants a licence for that use in any case.
`dmodem-ari` is under the AGPL-3.0, or a commercial licence (see
[LICENSING.md](../LICENSING.md)), whatever Asterisk's terms are.

## Setting up

### Asterisk's HTTP server and an ARI user

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

### The dialplan

The dialplan hands calls to the app with `Stasis()`, giving the profile
and the options that `DModem()` takes:

```
exten => 5551000,1,Stasis(dmodem,bbs)
 same => n,Verbose(1,${DMODEMSTATUS} ${DMODEMRATE})
 same => n,Hangup()
; the modem calls (DModem(profile,o)): on a call Asterisk has placed
exten => s,1,Stasis(dmodem,isp,o)
```

When the call is over, `dmodem-ari` sets `DMODEMSTATUS` and `DMODEMRATE`
as `DModem()` does. The call then continues in the dialplan after the
`Stasis()`, unless it has hung up.

For faxes, `sendfax` or `receivefax` takes the place of the profile,
followed by the file and our own number (the ident). This matches
`DModemSendFax(file[,ident])` and `DModemReceiveFax(file[,ident])` (see
[Faxes](#faxes)):

```
exten => 5551001,1,Stasis(dmodem,receivefax,/var/spool/fax/${UNIQUEID}.tif)
 same => n,Hangup()
exten => h,1,Verbose(1,${DMODEMFAXSTATUS} ${DMODEMFAXPAGES} ${DMODEMFAXFILE})
```

```
channel originate PJSIP/5551234@trunk application Stasis dmodem,sendfax,/var/spool/fax/doc.tif
```

### dmodem.conf

`dmodem-ari` reads the same `dmodem.conf` as `app_dmodem`: the profiles
and their options (see [asterisk.md](asterisk.md)). It also reads ARI's
address from `[general]`:

```
[general]
ari_url = http://127.0.0.1:8088
ari_user = dmodem
ari_password = secret
ari_app = dmodem        ; the Stasis() app's name
debug = 0               ; the modem's own log (1: the core, 3: the pumps)
```

| Key | Default |
|---|---|
| `ari_url` | `http://127.0.0.1:8088` (it must be an `http://` URL) |
| `ari_user`, `ari_password` | empty |
| `ari_app` | `dmodem` |
| `debug` | `0` |

### Running it

```
dmodem-ari [-c /etc/asterisk/dmodem.conf] [-u http://host:port]
           [-U user:password] [-a app] [-v]...
```

- `-c` names the configuration file; the default is
  `/etc/asterisk/dmodem.conf`.
- `-u`, `-U` and `-a` override `ari_url`, `ari_user`/`ari_password` and
  `ari_app` in `[general]`.
- Each `-v` logs more on stderr. One `-v` logs the calls (answering,
  connected, done); a second adds the bridge's details.

If the WebSocket goes away, `dmodem-ari` reconnects a second later.
Calls already in progress carry on meanwhile.

## How a call goes

For each call that enters the app, `dmodem-ari` does the following:

1. **Answer.** It answers the channel, unless it is up already (as on a
   call Asterisk placed).
2. **Open the audio.** It opens a UDP port of its own on 127.0.0.1, and
   asks for an external media channel (`POST /channels/externalMedia`,
   RTP over UDP) that sends the call's audio there. The audio is in the
   call's law: the profile's `law` if it has one, else the channel's
   own.
3. **Bridge.** It puts the call's channel and the external media channel
   in a bridge of their own (a mixing bridge). With both channels in the
   same law, Asterisk uses its `simple_bridge`, which passes the frames
   as they are. V.90's and V.91's PCM codes therefore arrive unchanged.
4. **Run the modem.** libdmodem's modem (or fax machine) works on the
   packets:
   - Each packet that arrives is a block of the line, and the modem's
     answer goes back as a packet of the same length. The call's own
     clock therefore paces the modem.
   - After 200 ms without a packet, the modem carries on alone, on
     silence.
   - A channel without a clock also works, such as the two halves of a
     Local channel with a `Stasis()` on each. The two sides pace each
     other, faster than real time, as two `DModem()` calls do.
5. **Pass the data.** Once the modems are connected, the data goes to
   the profile's server or program through the PBX modules' bridge
   (`pbx/dmodem_bridge.c`). The call's details go in the program's
   environment or the server's header:
   `DMODEM_CHANNEL`, `DMODEM_PROFILE`, `DMODEM_RATE`, `DMODEM_CALLERID`,
   `DMODEM_DIALED` and `DMODEM_CONNECTED`.
6. **Clean up.** At the end it deletes the external media channel and
   the bridge, sets the variables, and continues the call in the
   dialplan.

## Faxes

The fax machine is the same as `app_dmodem`'s: T.30 on V.17, V.29 and
V.27ter, with error correction mode (ECM) and TIFF files (see
[asterisk.md](asterisk.md#fax)). Its settings come from `dmodem.conf`'s
`[general]` section: `fax_ident`, `fax_max_rate`, `fax_ecm`, `fax_t38`,
and `fax_dir` for the faxes the modem profiles take (default
`/var/spool/asterisk/fax`).

### Three ways to fax

- **A fax machine calling a modem profile.** When the profile has `fax`
  on (the default), the fax is received into
  `<fax_dir>/<date>-<time>-<caller>-<uniqueid>.tif`, as `DModem()` does,
  using the profile's fax settings. The fax machine is recognised by its
  CNG tone, or by a V.8 CM that announces a fax call. `DMODEMSTATUS` is
  set to `FAX` as soon as the fax machine is heard.
- **`Stasis(dmodem,receivefax,<file>[,<ident>])`** answers as a fax
  machine and receives into the file.
- **`Stasis(dmodem,sendfax,<file>[,<ident>])`** sends a TIFF file (any
  TIFF of 1 bit pels, as `DModemSendFax` takes) on a call placed to a
  fax machine.

`<ident>` is our own number, used instead of `fax_ident`.

### The variables

`sendfax` and `receivefax` set the same variables as `DModemSendFax` and
`DModemReceiveFax`. `DMODEMFAXFILE` is set first, then:

| Variable | Value |
|---|---|
| `DMODEMFAXSTATUS` | `SUCCESS` or `FAILED` |
| `DMODEMFAXRESULT` | the result in words |
| `DMODEMFAXPAGES` | pages sent or received |
| `DMODEMFAXRATE` | the bit rate |
| `DMODEMFAXREMOTE` | the other machine's ident |
| `DMODEMFAXECM` | `yes` or `no` |
| `DMODEMFAXT38` | `yes` or `no` |

### One difference from the module

An ARI application cannot keep hold of a channel that has hung up. When
the other machine hangs up, Asterisk ends the `Stasis()` and runs the
`h` extension at once, whereas `DModemReceiveFax` would finish first.

So `dmodem-ari` sets the result as soon as it is final:

- when receiving, once the MCF (message confirmation) for the last page
  has gone out;
- when sending, once the MCF for the last page has come in.

By then the received pages are already in the file. The other machine
hangs up only after that (about a second after the MCF, and after its
DCN), so the `h` extension sees the result of a fax that went through.

If the other machine hangs up before then, only `DMODEMFAXFILE` is set:
treat an empty `DMODEMFAXSTATUS` in `h` as `FAILED`. The same applies to
a modem call that the other side hangs up: `DMODEMSTATUS` is not set in
`h`, where `DModem()` would set `HANGUP`.

## T.38

With `t38 = yes` in `[general]`, each call's line is a SIP leg instead of
an external media channel:

- Asterisk calls `dmodem-ari`'s own SIP side on a PJSIP endpoint of its
  own (`POST /channels` to
  `PJSIP/<sip_endpoint>/sip:<token>@<sip_bind>`).
- The audio arrives in RTP, as before.
- The leg can switch over to T.38 (over UDPTL) by a re-INVITE.

Asterisk passes T.38 between a bridge's PJSIP channels, so T.38 carries
on to the other end of the call when that endpoint has `t38_udptl` too.
External media, AudioSocket and WebSocket channels carry audio only,
which is why T.38 needs the SIP leg.

```
; pjsip.conf: dmodem-ari's SIP side
[dmodem]
type = endpoint
transport = transport-udp
context = default               ; (no calls come from it)
disallow = all
allow = ulaw
allow = alaw
direct_media = no
t38_udptl = yes
t38_udptl_ec = redundancy
aors = dmodem

[dmodem]
type = aor
contact = sip:dmodem@127.0.0.1:5090

; dmodem.conf
[general]
t38 = yes
sip_endpoint = dmodem           ; the endpoint above
sip_bind = 127.0.0.1:5090       ; dmodem-ari's SIP side (its RTP, UDPTL too)
```

| Key | Default |
|---|---|
| `t38` | `no` |
| `sip_endpoint` | `dmodem` |
| `sip_bind` | `127.0.0.1:5090` |

### How the faxes use it

The faxes use T.38 the same way as the module's do
([asterisk.md](asterisk-details.md#t38)). `fax_t38 = no` keeps them on
the audio.

- **`receivefax`, and a modem profile that hears a fax machine,** ask for
  T.38 with a re-INVITE of their own (as Asterisk's `ReceiveFAX` does).
  They wait for the answer for at most 6 seconds, measured by the clock
  or by the line's audio, whichever comes first, with silence on the
  line. If T.38 is refused or nothing answers, the fax goes on over the
  audio.
- **`sendfax`** takes T.38 when the other end asks for it (as
  `SendFAX` does while it sends CNG), at any time before its first page.
  Once a page has gone on the audio, it stays on the audio.
- **A modem profile** takes a T.38 offer that comes before the modems
  connect, treating it as a fax machine calling. Once the modems are
  connected, it refuses T.38 offers with 488 (Not Acceptable Here).

### The T.38 terminal

Over T.38 the fax machine is a T.38 terminal (libdmodem, `dsp/t38.cpp`
and `dsp/t30.cpp`):

- the IFP packets of T.38 versions 0 to 3;
- UDPTL, with earlier packets repeated for redundancy;
- transferred TCF;
- rates up to 14400 bit/s;
- error correction mode;
- packets paced as the modulations would carry them.

At the end of a fax that went over T.38, the log says `over T.38`.

Modem calls use the SIP leg in the same way. V.90's and V.91's G.711
codes come through it unchanged, as they do through external media.

## Limits of the prototype

- **Variables after a hangup:** the variables of a call that the other
  side hangs up are only as complete as they were before the hangup
  (see [Faxes](#faxes)).
- **Plain HTTP:** ARI is reached over plain HTTP, so Asterisk must be on
  the same host or on a trusted network. The WebSocket's
  `Sec-WebSocket-Accept` header is not checked.
- **Same host:** each call has a thread of its own, and the RTP is on
  127.0.0.1, so `dmodem-ari` must run on Asterisk's host.
- **T.38's SIP side** is for Asterisk only: UDP, no authentication, and
  only the calls Asterisk makes to it.
- **UDPTL's FEC:** forward error correction is not sent; an offer of it
  is answered with redundancy instead. Of FEC packets that arrive, only
  the primary packet is used.
- **ISDN:** clear channel (ISDN data) calls are not taken, because ARI's
  external media carries G.711 only (see [isdn.md](isdn.md)).

## Testing

`ARI=1` runs the Asterisk tests ([asterisk.md](asterisk-details.md#testing))
through `dmodem-ari` instead of the module (`asterisk/test/ari_env.sh`).
It turns on Asterisk's HTTP server and ARI, does not load `app_dmodem`,
and rewrites the dialplan's `DModem(...)`, `DModemSendFax(...)` and
`DModemReceiveFax(...)` as `Stasis(dmodem,...)`.

- **`ARI=1 asterisk/test/sip.sh`:** D-Modem's dmodemd calling over SIP.
  The script's variants work the same way (`INCOMING=1` for the modem
  calling, `CALLS=3`, `CODEC=alaw`, and so on). They connect at
  62666/62666 bit/s, V.91, which only exact G.711 codes allow.
- **`ARI=1 asterisk/test/run.sh`:** two `dmodem-ari` modems on the two
  halves of a Local channel (no clock), also at 62666/62666 bit/s.
- **`ARI=1 asterisk/test/fax.sh` and `ARI=1 asterisk/test/fax_spandsp.sh`:**
  the faxes. They run between `dmodem-ari`'s own machines (a modem
  profile receiving, `receivefax`, `sendfax`) and against Asterisk's
  `SendFAX` and `ReceiveFAX` (spandsp). The `h` extensions must have the
  results, and the received files must have the pages sent.
- **`ARI_SIP=1` with any of them:** `t38 = yes`, with the calls on SIP
  legs (a `dmodem` endpoint is added to `pjsip.conf`). The modems still
  connect at 62666/62666. The faxes between `dmodem-ari`'s own machines
  go over T.38, even through a Local channel.
- **`ARI=1 asterisk/test/t38.sh`:** the module's T.38 test
  ([asterisk.md](asterisk-details.md#testing)) through `dmodem-ari`,
  with `t38 = yes` and its SIP side on the `dmodem` endpoint.
