# dmodem-ari: D-Modem as an Asterisk ARI application: technical details

This page gives the technical details for [dmodem-ari: D-Modem beside Asterisk](ari.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

`dmodem-ari` (`frontend/ari/`) does the functions of the `app_dmodem`
module. But it is a separate program next to Asterisk, not a module in
Asterisk:

- It answers (or makes) Asterisk calls with the modem of D-Modem, as
  `DModem()` does.
- It sends and receives faxes with the fax machine of D-Modem, as
  `DModemSendFax()` and `DModemReceiveFax()` do, on the audio or through
  T.38 (refer to [T.38](#t38)).

It communicates with Asterisk through the Asterisk REST Interface (ARI):

- The events arrive on a WebSocket.
- The requests go through HTTP.
- The audio of each call comes through RTP from an ARI *external media*
  channel, in the G.711 law of the call.

Asterisk does not load parts of D-Modem.

### Why a separate program

This keeps the licence of the modem separate from the licence of
Asterisk. A loadable module is under the GPL together with Asterisk (the
`LICENSE` of Asterisk: "the GPL applies to all loadable Asterisk modules
used on your system"). Applications that control Asterisk through AMI,
AGI or ARI do not have to be under the GPL. Also, Sangoma grants a
licence for that use. `dmodem-ari` is under the AGPL-3.0, or a commercial
licence (refer to [LICENSING.md](../LICENSING.md)), whatever the terms of
Asterisk are.

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

The dialplan sends calls to the app with `Stasis()`. It gives the
profile and the options that `DModem()` accepts:

```
exten => 5551000,1,Stasis(dmodem,bbs)
 same => n,Verbose(1,${DMODEMSTATUS} ${DMODEMRATE})
 same => n,Hangup()
; the modem calls (DModem(profile,o)): on a call Asterisk has placed
exten => s,1,Stasis(dmodem,isp,o)
```

When the call ends, `dmodem-ari` sets `DMODEMSTATUS` and `DMODEMRATE`
as `DModem()` does. Then the call continues in the dialplan after the
`Stasis()`, unless it has hung up.

For faxes, `sendfax` or `receivefax` replaces the profile. The file and
our own number (the ident) follow it. This agrees with
`DModemSendFax(file[,ident])` and `DModemReceiveFax(file[,ident])` (refer
to [Faxes](#faxes)):

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
and their options (refer to [asterisk.md](asterisk.md)). It also reads
the address of ARI from `[general]`:

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

- `-c` gives the configuration file. The default is
  `/etc/asterisk/dmodem.conf`.
- `-u`, `-U` and `-a` override `ari_url`, `ari_user`/`ari_password` and
  `ari_app` in `[general]`.
- Each `-v` logs more on stderr. One `-v` logs the calls (answering,
  connected, done). A second `-v` adds the details of the bridge.

If the WebSocket stops, `dmodem-ari` connects again a second later.
Calls that are already in progress continue during this time.

## How a call goes

For each call that comes into the app, `dmodem-ari` does these steps:

1. **Answer.** It answers the channel, unless the channel is already up
   (as on a call that Asterisk made).
2. **Open the audio.** It opens its own UDP port on 127.0.0.1. It asks
   for an external media channel (`POST /channels/externalMedia`, RTP
   over UDP) that sends the audio of the call to that port. The audio is
   in the law of the call: the `law` of the profile if it has one, if not
   the law of the channel.
3. **Bridge.** It puts the channel of the call and the external media
   channel in their own bridge (a mixing bridge). If the two channels
   have the same law, Asterisk uses its `simple_bridge`, which sends the
   frames with no changes. Thus, the PCM codes of V.90 and V.91 arrive
   with no changes.
4. **Run the modem.** The modem (or fax machine) of libdmodem operates on
   the packets:
   - Each packet that arrives is a block of the line. The answer of the
     modem goes back as a packet of the same length. Thus, the clock of
     the call controls the speed of the modem.
   - After 200 ms without a packet, the modem continues alone, on
     silence.
   - A channel without a clock also operates, for example the two halves
     of a Local channel with a `Stasis()` on each. The two sides control
     the speed of each other, faster than real time, as two `DModem()`
     calls do.
5. **Send the data.** When the modems are connected, the data goes to the
   server or program of the profile through the bridge of the PBX modules
   (`frontend/pbx/dmodem_bridge.c`). The details of the call go in the
   environment of the program or the header of the server:
   `DMODEM_CHANNEL`, `DMODEM_PROFILE`, `DMODEM_RATE`, `DMODEM_CALLERID`,
   `DMODEM_DIALED` and `DMODEM_CONNECTED`.
6. **Clean up.** At the end, it deletes the external media channel and
   the bridge, sets the variables, and continues the call in the
   dialplan.

## Faxes

The fax machine is the same as that of `app_dmodem`: T.30 on V.17, V.29
and V.27ter, with error correction mode (ECM) and TIFF files (refer to
[asterisk.md](asterisk.md#fax)). Its settings come from the `[general]`
section of `dmodem.conf`: `fax_ident`, `fax_max_rate`, `fax_ecm`,
`fax_t38`, and `fax_dir` for the faxes that the modem profiles receive
(default `/var/spool/asterisk/fax`).

### Three ways to fax

- **A fax machine calls a modem profile.** When the profile has `fax`
  enabled (the default), the fax goes into
  `<fax_dir>/<date>-<time>-<caller>-<uniqueid>.tif`, as `DModem()` does,
  with the fax settings of the profile. The modem identifies the fax
  machine by its CNG tone, or by a V.8 CM that tells of a fax call.
  `DMODEMSTATUS` is set to `FAX` immediately when the modem hears the fax
  machine.
- **`Stasis(dmodem,receivefax,<file>[,<ident>])`** answers as a fax
  machine and receives into the file.
- **`Stasis(dmodem,sendfax,<file>[,<ident>])`** sends a TIFF file (all
  TIFFs of 1 bit pels, as `DModemSendFax` accepts) on a call to a fax
  machine.

`<ident>` is our own number. It replaces `fax_ident`.

### The variables

`sendfax` and `receivefax` set the same variables as `DModemSendFax` and
`DModemReceiveFax`. First, `DMODEMFAXFILE` is set. Then these are set:

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

An ARI application cannot keep a channel that has hung up. When the
other machine hangs up, Asterisk ends the `Stasis()` and runs the `h`
extension immediately. `DModemReceiveFax` completes first.

Thus, `dmodem-ari` sets the result immediately when it is final:

- When it receives, after the MCF (message confirmation) for the last
  page has gone out.
- When it sends, after the MCF for the last page has come in.

At that time, the received pages are already in the file. The other
machine hangs up only after that (about a second after the MCF, and
after its DCN). Thus, the `h` extension sees the result of a fax that
went through.

If the other machine hangs up before then, only `DMODEMFAXFILE` is set.
Treat an empty `DMODEMFAXSTATUS` in `h` as `FAILED`. The same applies to
a modem call that the other side hangs up. `DMODEMSTATUS` is not set in
`h`, where `DModem()` would set `HANGUP`.

## T.38

With `t38 = yes` in `[general]`, the line of each call is a SIP leg, not
an external media channel:

- Asterisk calls the own SIP side of `dmodem-ari` on its own PJSIP
  endpoint (`POST /channels` to
  `PJSIP/<sip_endpoint>/sip:<token>@<sip_bind>`).
- The audio arrives in RTP, as before.
- The leg can change to T.38 (over UDPTL) by a re-INVITE.

Asterisk sends T.38 between the PJSIP channels of a bridge. Thus, T.38
continues to the other end of the call when that endpoint also has
`t38_udptl`. External media, AudioSocket and WebSocket channels send only
audio. This is why T.38 needs the SIP leg.

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

The faxes use T.38 in the same way as the faxes of the module
([asterisk.md](asterisk-details.md#t38)). `fax_t38 = no` keeps them on
the audio.

- **`receivefax`, and a modem profile that hears a fax machine,** ask for
  T.38 with their own re-INVITE (as the `ReceiveFAX` of Asterisk does).
  They wait for the answer for a maximum of 6 seconds, measured by the
  clock or by the audio of the line (the first of the two), with silence
  on the line. If T.38 is refused or nothing answers, the fax continues
  on the audio.
- **`sendfax`** accepts T.38 when the other end asks for it (as `SendFAX`
  does while it sends CNG), at any time before its first page. After a
  page has gone on the audio, it stays on the audio.
- **A modem profile** accepts a T.38 offer that comes before the modems
  connect. It operates it as a call from a fax machine. After the modems
  are connected, it refuses T.38 offers with 488 (Not Acceptable Here).

### The T.38 terminal

Over T.38, the fax machine is a T.38 terminal (libdmodem, `dsp/t38.cpp`
and `dsp/t30.cpp`). It has these items:

- The IFP packets of T.38 versions 0 to 3.
- UDPTL, with earlier packets repeated for redundancy.
- Transferred TCF.
- Rates up to 14400 bit/s.
- Error correction mode.
- Packets sent at the speed at which the modulations would send them.

At the end of a fax that went over T.38, the log shows `over T.38`.

Modem calls use the SIP leg in the same way. The G.711 codes of V.90 and
V.91 come through it with no changes, as through external media.

## Limits of the prototype

- **Variables after a hangup:** if the other side hangs up a call, the
  variables of the call are only as complete as they were before the
  hangup (refer to [Faxes](#faxes)).
- **Plain HTTP:** `dmodem-ari` connects to ARI through plain HTTP. Thus,
  Asterisk must be on the same host or on a trusted network. The program
  does not do a check of the `Sec-WebSocket-Accept` header of the
  WebSocket.
- **Same host:** each call has its own thread, and the RTP is on
  127.0.0.1. Thus, `dmodem-ari` must run on the host of Asterisk.
- **The SIP side of T.38** is only for Asterisk: UDP, no authentication,
  and only the calls that Asterisk makes to it.
- **The FEC of UDPTL:** the program does not send forward error
  correction. It answers an offer of FEC with redundancy. Of FEC packets
  that arrive, it uses only the primary packet.
- **ISDN:** the program does not accept clear channel (ISDN data) calls,
  because the external media of ARI sends only G.711 (refer to
  [isdn.md](isdn.md)).

## Testing

`ARI=1` runs the Asterisk tests ([asterisk.md](asterisk-details.md#testing))
through `dmodem-ari` in place of the module (`frontend/asterisk/test/ari_env.sh`).
It enables the HTTP server and ARI of Asterisk, does not load
`app_dmodem`, and writes the `DModem(...)`, `DModemSendFax(...)` and
`DModemReceiveFax(...)` of the dialplan again as `Stasis(dmodem,...)`.

- **`ARI=1 frontend/asterisk/test/sip.sh`:** the dmodemd of D-Modem calls
  through SIP. The variants of the script operate in the same way
  (`INCOMING=1` for calls from the modem, `CALLS=3`, `CODEC=alaw`, and
  other variants). They connect at 62666/62666 bit/s, V.91, which is
  possible only with exact G.711 codes.
- **`ARI=1 frontend/asterisk/test/run.sh`:** two `dmodem-ari` modems on the
  two halves of a Local channel (no clock), also at 62666/62666 bit/s.
- **`ARI=1 frontend/asterisk/test/fax.sh` and `ARI=1 frontend/asterisk/test/fax_spandsp.sh`:**
  the faxes. They run between the own machines of `dmodem-ari` (a modem
  profile that receives, `receivefax`, `sendfax`) and against the
  `SendFAX` and `ReceiveFAX` of Asterisk (spandsp). The `h` extensions
  must have the results, and the received files must have the pages that
  were sent.
- **`ARI_SIP=1` with each of them:** `t38 = yes`, with the calls on SIP
  legs (the test adds a `dmodem` endpoint to `pjsip.conf`). The modems
  still connect at 62666/62666. The faxes between the own machines of
  `dmodem-ari` go through T.38, also through a Local channel.
- **`ARI=1 frontend/asterisk/test/t38.sh`:** the T.38 test of the module
  ([asterisk.md](asterisk-details.md#testing)) through `dmodem-ari`, with
  `t38 = yes` and its SIP side on the `dmodem` endpoint.
