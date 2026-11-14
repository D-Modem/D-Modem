# dmodem-ari: D-Modem beside Asterisk

`dmodem-ari` does the same work as the [app_dmodem](asterisk.md) module.
It answers (or makes) Asterisk calls with a modem, and it sends and
receives faxes. The difference is that `dmodem-ari` is a **separate program** that
runs together with Asterisk. It is not a module that Asterisk loads. It communicates with
Asterisk through the Asterisk REST Interface (ARI).

For each setting, and for the steps of a call, refer to
[the technical details](ari-details.md).

> **Status:** this is a prototype. It operates and we tested it, but it has some
> limits (refer to the end of this page).

## Why a separate program

The licence of Asterisk applies to each module that Asterisk loads. You cannot
combine that licence with the licence of D-Modem. A separate program that only communicates with
Asterisk does not have that problem. Thus, `dmodem-ari` is the method to supply
D-Modem for Asterisk. Asterisk does not load a part of D-Modem.

## Setting up

1. Enable the web server of Asterisk and make an ARI user, in `http.conf`
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

2. Give `dmodem-ari` the address of the web server, in the `[general]` section of
   `dmodem.conf`. `dmodem-ari` uses the same profiles as `app_dmodem`:

       [general]
       ari_url = http://127.0.0.1:8088
       ari_user = dmodem
       ari_password = secret
       ari_app = dmodem

3. Send calls to `dmodem-ari` from the dialplan with `Stasis()`:

       exten => 5551000,1,Stasis(dmodem,bbs)
        same => n,Hangup()
       exten => 5551001,1,Stasis(dmodem,receivefax,/var/spool/fax/${UNIQUEID}.tif)
        same => n,Hangup()

4. Start `dmodem-ari`:

       dmodem-ari -c /etc/asterisk/dmodem.conf -v

If the connection to Asterisk drops, `dmodem-ari` connects again one second later.
The calls that are in progress continue.

## People and modems on one number

A profile with `voice = exten@context` (and `voice_listen`,
`voice_dsvd`) operates as it does with `DModem()`. Refer to
[asterisk.md](asterisk.md). The leg of the extension joins the bridge of the call
while dmodem-ari listens only to the caller. The leg leaves the bridge when
the extension hangs up or when the modem of the caller asks for data.

## Faxes

`Stasis(dmodem,receivefax,<file>)` receives a fax, and
`Stasis(dmodem,sendfax,<file>)` sends a fax. The fax goes on the audio or over T.38
(fax over the internet).

## Limits of the prototype

- It must run on the same machine as Asterisk (or on a trusted network),
  because it communicates with Asterisk over plain HTTP.
- It uses one thread for each call.
- It does not accept ISDN data calls yet.
- It does not send some of the error-protection options of T.38.
