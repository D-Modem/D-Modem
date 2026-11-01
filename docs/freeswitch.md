# FreeSWITCH: mod_dmodem

[FreeSWITCH](https://freeswitch.com/) is an open-source telephony
platform. `mod_dmodem` is a FreeSWITCH module that answers a phone call
with a modem. Once the caller's modem connects, the data goes to a
server or program of your choice: a BBS, a login prompt, or `pppd` for
dial-up internet. It also sends and receives faxes. It is the FreeSWITCH
version of the [Asterisk module](asterisk.md).

For every option, the fax details and the tests, see
[the technical details](freeswitch-details.md).

## How it works

In the dialplan, send a number to the `dmodem` application with a
**profile** name:

```xml
<extension name="bbs">
  <condition field="destination_number" expression="^5551000$">
    <action application="dmodem" data="bbs"/>
    <action application="hangup"/>
  </condition>
</extension>
```

Each profile, in `autoload_configs/dmodem.conf.xml`, says where the
call's data goes:

```xml
<profile name="bbs">
  <param name="mode" value="tcp"/>
  <param name="host" value="127.0.0.1"/>
  <param name="port" value="2323"/>
</profile>
<profile name="ppp">
  <param name="mode" value="exec"/>
  <param name="command" value="exec /usr/sbin/pppd nodetach noauth local lock passive 192.168.77.1:192.168.77.2"/>
</profile>
```

All the options are in [dmodem.conf.xml](../frontend/freeswitch/dmodem.conf.xml).

`dmodem <profile>,o` does the opposite: the modem dials out to a remote
modem on a call FreeSWITCH has placed, for example
`originate sofia/gateway/trunk/5559999 &dmodem(isp,o)`.

## What the modem can do

It answers every common standard from 300 bit/s up to V.34 (33600
bit/s), with error correction and compression. With a fully digital
call path (G.711 all the way), it can also be the server end of a V.90
call, up to 56000 bit/s to the caller.

## Fax

`dmodem_receive_fax` answers a call as a fax machine, and
`dmodem_send_fax` sends a fax. The `dmodem` application also notices a
fax machine calling and receives the fax. Faxes go over the audio or, on
SIP calls, over T.38 (fax over the internet).

## Building

You need an installed FreeSWITCH that `pkg-config` can find:

    cmake -B build
    cmake --build build --target mod_dmodem
    cmake --install build
    fs_cli -x "load mod_dmodem"

Add `<load module="mod_dmodem"/>` to `modules.conf.xml` to load it at
start-up.
