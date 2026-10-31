# Asterisk: app_dmodem

[Asterisk](https://www.asterisk.org/) is a popular open-source phone
system. `app_dmodem` is an Asterisk module that lets Asterisk answer a
phone call with a modem. Once the caller's modem connects, the data goes
to a server or a program of your choice: a BBS, a login prompt, or
`pppd` for dial-up internet. The same module also sends and receives
faxes.

For every option, the fax details and the tests, see
[the technical details](asterisk-details.md).

> **Build it yourself; don't share the built module.** The module puts
> D-Modem's code (AGPL-3.0 licence) inside Asterisk (GPL-2.0 licence),
> and those two licences cannot be combined in something you hand to
> others ([LICENSING.md](../LICENSING.md)). So it is off by default, for
> building on your own Asterisk only. To ship D-Modem with Asterisk, use
> [dmodem-ari](ari.md) instead: the same modem as a separate program,
> with the same settings.

## How it works

In the dialplan (`extensions.conf`), send a number to the modem with a
**profile** name:

    exten => 5551000,1,DModem(bbs)
     same => n,Hangup()
    exten => 5552000,1,DModem(ppp)
     same => n,Hangup()

Each profile, in `dmodem.conf`, says where the call's data goes:

    [bbs]
    mode = tcp
    host = 127.0.0.1
    port = 2323

    [ppp]
    mode = exec
    command = exec /usr/sbin/pppd nodetach noauth local lock passive 192.168.77.1:192.168.77.2

So one Asterisk can offer a BBS on one number and dial-up internet on
another. All the options are in
[dmodem.conf.sample](../asterisk/dmodem.conf.sample).

`DModem(profile,o)` does the opposite: the modem dials out to a remote
modem, on a call Asterisk has placed, so your server can reach a remote
BBS or provider.

## What the modem can do

It answers every common standard from 300 bit/s up to V.34 (33600
bit/s), with error correction and compression. With a fully digital
call path (G.711 all the way), it can also be the server end of a V.90
call, up to 56000 bit/s to the caller.

## Fax

The module is also a fax machine:

- `DModem()` notices when a fax machine calls instead of a modem, and
  receives the fax to a file.
- `DModemReceiveFax(file)` answers a call as a fax machine.
- `DModemSendFax(file)` sends a fax.

Faxes can go over the audio or, on SIP calls, over T.38 (fax over the
internet), which is more reliable.

## Building

You need the headers of your installed Asterisk (for example from your
distribution's `asterisk-dev` package):

    cmake -B build -DDMODEM_APP_DMODEM=ON
    cmake --build build --target app_dmodem
    cmake --install build
    asterisk -rx "module load app_dmodem.so"

Each call uses a few percent of one processor core; there is no other
limit on how many calls run at once.

## See also

- [dmodem-ari](ari.md): the same, as a program beside Asterisk.
- [FreePBX](freepbx.md): set it up from FreePBX's web interface.
- [ISDN](isdn.md): ISDN data calls through Asterisk.
