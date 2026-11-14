# Asterisk: app_dmodem

[Asterisk](https://www.asterisk.org/) is a popular open-source telephone
system. `app_dmodem` is an Asterisk module. With it, Asterisk answers a
telephone call with a modem. After the modem of the caller connects, the
data goes to a server or a program that you select: a BBS, a login
prompt, or `pppd` for dial-up internet. The same module also sends and
receives faxes.

For all options, the fax details and the tests, refer to
[the technical details](asterisk-details.md).

> **CAUTION: BUILD THE MODULE YOURSELF. DO NOT SHARE THE BUILT MODULE.**
> The module puts the code of D-Modem (AGPL-3.0 licence) in Asterisk
> (GPL-2.0 licence). You cannot combine these two licences in an item
> that you give to other persons ([LICENSING.md](../LICENSING.md)). Thus,
> the module is disabled by default. It is only for a build on your own
> Asterisk. To distribute D-Modem with Asterisk, use
> [dmodem-ari](ari.md). It is the same modem as a separate program, with
> the same settings.

## How it works

In the dialplan (`extensions.conf`), send a number to the modem with the
name of a **profile**:

    exten => 5551000,1,DModem(bbs)
     same => n,Hangup()
    exten => 5552000,1,DModem(ppp)
     same => n,Hangup()

Each profile, in `dmodem.conf`, tells where the data of the call goes:

    [bbs]
    mode = tcp
    host = 127.0.0.1
    port = 2323

    [ppp]
    mode = exec
    command = exec /usr/sbin/pppd nodetach noauth local lock passive 192.168.77.1:192.168.77.2

Thus, one Asterisk can give a BBS on one number and dial-up internet on
a different number. All of the options are in
[dmodem.conf.sample](../frontend/asterisk/dmodem.conf.sample).

`DModem(profile,o)` does the opposite. The modem dials a remote modem, on
a call that Asterisk made. Thus, your server can connect to a remote BBS
or provider.

## People and modems on one number

A profile with `voice` set listens for a few seconds before it answers
(`voice_listen`, 3 by default). If a person calls, the call goes to that
extension, for example an IVR or a voicemail box:

    [bbs]
    mode = tcp
    host = 127.0.0.1
    port = 2323
    voice = 7000@from-internal
    voice_dsvd = yes

The person stays there until the extension hangs up, or until the modem
of the caller asks for data (its calling tone). Then the modem answers as
usual. Some modems send no calling tone (in most countries, modems wait
in silence). A caller with such a modem stays with the extension until it
hangs up. Thus, give an IVR a method to stop, for example a menu
selection that hangs up.

Some modems send voice and data at the same time (V.70 DSVD or V.61
ASVD). For these modems, the module can also send the voice channel to
the extension while the data continues. Set `voice_dsvd` to `yes` (the
caller opens the channel) or `open` (our modem opens it). The channel
closes when the extension hangs up, and the data continues.

## What the modem can do

It answers all common standards from 300 bit/s up to V.34 (33600
bit/s), with error correction and compression. With a fully digital
call path (G.711 on the full path), it can also be the server end of a
V.90 call, up to 56000 bit/s to the caller.

## Fax

The module is also a fax machine:

- `DModem()` finds when a fax machine calls in place of a modem. Then it
  receives the fax to a file.
- `DModemReceiveFax(file)` answers a call as a fax machine.
- `DModemSendFax(file)` sends a fax.

Faxes can go through the audio. On SIP calls, they can go through T.38
(fax over the internet), which is more reliable.

## Building

You must have the headers of your installed Asterisk (for example from
the `asterisk-dev` package of your distribution):

    cmake -B build -DDMODEM_APP_DMODEM=ON
    cmake --build build --target app_dmodem
    cmake --install build
    asterisk -rx "module load app_dmodem.so"

Each call uses a few percent of one processor core. There is no other
limit on the number of calls at the same time.

## See also

- [dmodem-ari](ari.md): the same, as a program next to Asterisk.
- [FreePBX](freepbx.md): configure it from the web interface of FreePBX.
- [ISDN](isdn.md): ISDN data calls through Asterisk.
