# dmodem-ari: D-Modem beside Asterisk

`dmodem-ari` does the same job as the [app_dmodem](asterisk.md) module:
it answers (or makes) Asterisk calls with a modem, and sends and
receives faxes. The difference is that it is a **separate program** that
runs next to Asterisk, rather than a module loaded into it. It talks to
Asterisk through the Asterisk REST Interface (ARI).

For every setting and how a call is handled step by step, see
[the technical details](ari-details.md).

> **Status:** this is a prototype. It works and is tested, but has some
> limits (below).

## Why a separate program

Asterisk's licence covers every module loaded into it, and that licence
cannot be combined with D-Modem's. A separate program that only talks to
Asterisk does not have that problem. So `dmodem-ari` is the way to ship
D-Modem for Asterisk. Nothing of D-Modem is loaded into Asterisk.

## Setting up

1. Turn on Asterisk's web server and create an ARI user, in `http.conf`
   and `ari.conf`:

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

2. Tell `dmodem-ari` how to reach it, in the `[general]` section of
   `dmodem.conf`. It uses the same profiles as `app_dmodem`:

       [general]
       ari_url = http://127.0.0.1:8088
       ari_user = dmodem
       ari_password = secret
       ari_app = dmodem

3. Send calls to it from the dialplan with `Stasis()`:

       exten => 5551000,1,Stasis(dmodem,bbs)
        same => n,Hangup()
       exten => 5551001,1,Stasis(dmodem,receivefax,/var/spool/fax/${UNIQUEID}.tif)
        same => n,Hangup()

4. Start it:

       dmodem-ari -c /etc/asterisk/dmodem.conf -v

If the connection to Asterisk drops, it reconnects a second later, and
calls already in progress carry on.

## Faxes

`Stasis(dmodem,receivefax,<file>)` receives a fax and
`Stasis(dmodem,sendfax,<file>)` sends one, on the audio or over T.38
(fax over the internet).

## Limits of the prototype

- It must run on the same machine as Asterisk (or a trusted network):
  it talks to Asterisk over plain HTTP.
- It uses one thread per call.
- It does not take ISDN data calls yet.
- Some of T.38's error-protection options are not sent.
