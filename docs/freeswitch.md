# FreeSWITCH: mod_dmodem

[FreeSWITCH](https://freeswitch.com/) is an open-source telephony
platform. `mod_dmodem` is a FreeSWITCH module that answers a phone call
with a modem. When the modem of the caller connects, the data goes to a
server or program that you select: a BBS, a login prompt, or `pppd` for
dial-up internet. It also sends and receives faxes. It is the FreeSWITCH
version of the [Asterisk module](asterisk.md).

For each option, the fax details and the tests, refer to
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

Each profile, in `autoload_configs/dmodem.conf.xml`, tells where the
data of the call goes:

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

`dmodem <profile>,o` does the opposite. The modem dials a remote
modem on a call that FreeSWITCH made, for example
`originate sofia/gateway/trunk/5559999 &dmodem(isp,o)`.

## People and modems on one number

A profile with a `voice` setting listens for some seconds before it answers
(`voice-listen`, 3 by default). If the caller is a person, the call goes
to that extension (on a loopback leg), for example an IVR or a
voicemail box:

```xml
<profile name="bbs">
  <param name="mode" value="tcp"/>
  <param name="host" value="127.0.0.1"/>
  <param name="port" value="2323"/>
  <param name="voice" value="7000@default"/>
  <param name="voice-dsvd" value="yes"/>
</profile>
```

The person stays there until the extension hangs up, or until
the modem of the caller asks for data (its calling tone). Then the modem
answers as usual. Some modems send no calling tone (the modems of most
countries wait in silence). A caller with such a modem stays with the extension until the extension
hangs up. Thus, give an IVR a method to stop, for example a menu selection that hangs
up.

A modem can carry voice and data at the same time (V.70 DSVD or V.61 ASVD).
The module can also send the voice channel of such a modem to the extension while the data
continues. To do this, set `voice-dsvd` to `yes` (the caller opens the channel) or
`open` (our modem opens it). The channel closes when the extension hangs up,
and the data continues.

## What the modem can do

It answers all common standards from 300 bit/s up to V.34 (33600
bit/s), with error correction and compression. With a fully digital
call path (G.711 from end to end), it can also be the server end of a V.90
call, up to 56000 bit/s to the caller.

## Fax

`dmodem_receive_fax` answers a call as a fax machine, and
`dmodem_send_fax` sends a fax. The `dmodem` application also finds when a
fax machine calls, and it receives the fax. Faxes go over the audio or, on
SIP calls, over T.38 (fax over the internet).

## Building

You must have an installed FreeSWITCH that `pkg-config` can find:

    cmake -B build
    cmake --build build --target mod_dmodem
    cmake --install build
    fs_cli -x "load mod_dmodem"

To load the module at start-up, add `<load module="mod_dmodem"/>` to `modules.conf.xml`.
