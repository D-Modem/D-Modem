# mod_dmodem: a data modem on a FreeSWITCH channel: technical details

This is the technical companion to [FreeSWITCH: mod_dmodem](freeswitch.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

`mod_dmodem` is the FreeSWITCH version of [Asterisk's app_dmodem](asterisk.md).
It has the same modem and fax, with FreeSWITCH's applications,
configuration and channel variables. Both modules share the code that
connects the modem to a server or program (`frontend/pbx/dmodem_bridge.c`).

## Overview

The module adds three applications:

| application | what it does |
|---|---|
| `dmodem [profile][,o]` | Answers the call with a software modem and, once the modems connect, hands the data to a server or program. |
| `dmodem_receive_fax <file> [ident]` | Answers as a fax machine and receives into a TIFF file. |
| `dmodem_send_fax <file> [ident]` | Sends a TIFF file as a fax on a call placed to a fax machine. |

`dmodem` hands the data to one of three places, as the profile in
`dmodem.conf` says:

- a server over TCP (`mode` `tcp`), such as a BBS;
- a server on a unix socket (`mode` `unix`), such as a login menu;
- a program on a pty (`mode` `exec`), such as `pppd` for dial-up IP, a
  login or a BBS door.

Without a profile argument, `dmodem` uses the profile `default`. One
FreeSWITCH can serve several numbers in several ways:

```xml
<!-- dialplan -->
<extension name="bbs">
  <condition field="destination_number" expression="^5551000$">
    <action application="dmodem" data="bbs"/>
    <action application="hangup"/>
  </condition>
</extension>
<extension name="ppp">
  <condition field="destination_number" expression="^5552000$">
    <action application="dmodem" data="ppp"/>
    <action application="hangup"/>
  </condition>
</extension>
```

```xml
<!-- autoload_configs/dmodem.conf.xml -->
<configuration name="dmodem.conf">
  <settings/>
  <profiles>
    <profile name="bbs">
      <param name="mode" value="tcp"/>
      <param name="host" value="127.0.0.1"/>
      <param name="port" value="2323"/>
    </profile>
    <profile name="ppp">
      <param name="mode" value="exec"/>
      <param name="command" value="exec /usr/sbin/pppd nodetach noauth local lock passive 192.168.77.1:192.168.77.2"/>
    </profile>
  </profiles>
</configuration>
```

[dmodem.conf.xml](../frontend/freeswitch/dmodem.conf.xml) lists all the options.

### Profile params

| param | default | meaning |
|---|---|---|
| `mode` | `tcp` | `tcp`, `unix` or `exec`. |
| `host`, `port` | none | `mode` `tcp`: the server. Both are required. |
| `path` | none | `mode` `unix`: the server's stream socket. Required. |
| `command` | none | `mode` `exec`: the program, run by `/bin/sh -c`. Required. |
| `header` | `false` | `tcp` and `unix` only: send the call's details first (see [below](#what-the-server-or-program-sees)). |
| `init` | empty | Extra AT commands for the modem, in both directions (for example `+MS=34`). |
| `answer` | `ATA` | The command line the modem answers with. |
| `law` | `auto` | The G.711 law: `ulaw`, `alaw`, or `auto` for the channel's codec. |
| `clear` | `auto` | `true`: every call is a clear 64 kbit/s channel (`AT+MS=CLEAR` on the channel's octets as they are, as for an ISDN data call). FreeSWITCH has no digital-bearer calls, so `auto` here means `false`. |
| `timeout` | `60` | Seconds the modems have to connect. |
| `jitter-buffer` | `60` | The jitter buffer in ms, as in `<settings>` (see [The channel](#the-channel)). |
| `fax` | `true` | Receive a calling fax machine as a fax (see [Fax](#fax)). |

A profile can also hold any of the fax params of `<settings>`. Param
names are matched without regard to case, and `_` and `-` are the same.
A profile that lacks the params its mode requires is refused, and
`dmodem` ends with `dmodem_status` `ERROR`.

### Answering and calling

The modem is first set up with `ATE0Q1S0=0S2=255X3` (no echo, no result
codes, no auto-answer, no `+++` escape, no wait for a dial tone),
followed by the profile's `init`.

By default the modem answers with `ATA`. A profile's `answer` param
gives a different command line. Commands that apply only to the
answering call go before the `A`. For example,
`<param name="answer" value="AT+MS=132;A"/>` answers at V.32bis at most
(no escaping of `;` is needed in XML). If the command line leaves the
modem not answering, the call ends with `dmodem_status` `ERROR`.

With `dmodem <profile>,o` (or `<profile> o`), our modem is the caller: it
waits for the remote modem's answer tone. This is for a call that
FreeSWITCH places to a remote modem, for example:

```
originate sofia/gateway/trunk/5559999 &dmodem(isp,o)
```

The profile's server or program then talks to the remote BBS or ISP. In
`originate`, the comma form needs no quoting. An application argument
with a space does need quotes, as in
`'&dmodem_send_fax(/tmp/doc.tif +1 555 0100)'`.

### The modem

The modem is `libdmodem` ([frontend/dmodemd/dmodem.h](../frontend/dmodemd/dmodem.h)). It
is D-Modem's modem core (AT commands, V.42 error correction, V.44 or
V.42bis compression) with the open data pumps of `dsp`. It supports:

- V.21, V.22, V.22bis, V.23, Bell 103 and 212A, V.32, V.32bis and V.34;
- V.90 as the digital (server) modem: up to 56000 bit/s downstream to a
  V.90 client modem, when the call is G.711 end to end;
- V.91 with another digital modem, such as another D-Modem.

The module has no binary-only parts.

## Fax

The module is also a group 3 fax machine:

- T.30 on V.17, V.29 and V.27ter, at 2400 to 14400 bit/s;
- pages in MH or MR coding, in TIFF files;
- error correction mode (ECM, T.30 Annex A) when the other machine has
  it. A page is sent in blocks of HDLC frames, and the receiving machine
  asks again for any that a line hit spoiled, so the page arrives exactly
  as it was sent.

It is an alternative to mod_spandsp's `txfax` and `rxfax`:

- **`dmodem` receives faxes by itself.** If the calling machine's CNG
  tone, or its V.8 CM of a fax call, comes before a modem connection,
  the call goes to the fax receiver instead. It writes
  `<fax-dir>/<date>-<time>-<caller>-<uuid>.tif`, where the date and time
  are `YYYYMMDD-HHMMSS`, and the caller is the caller ID's digits and `+`
  (or `unknown`). `dmodem_status` is then `FAX`. The profile's `fax`
  param set to `false` turns this off. This applies only when answering,
  not with the `o` option.
- **`dmodem_receive_fax <file> [ident]`** answers as a fax machine and
  receives into the file.
- **`dmodem_send_fax <file> [ident]`** sends a TIFF file on a call placed
  to a fax machine, for example:

  ```
  originate sofia/gateway/trunk/5551234 &dmodem_send_fax(/var/spool/fax/doc.tif)
  ```

  Any TIFF of 1-bit pels can be sent: uncompressed, G3 1D or 2D, or G4.
  It can be of any width (it is fitted to the fax's 1728 pels) and any
  resolution (standard and fine are sent as they are; superfine is
  halved). This makes one from a PDF:

  ```
  gs -sDEVICE=tiffg4 -r204x196 -o doc.tif doc.pdf
  ```

`ident` is our fax number (the CSI when receiving, the TSI when
sending); everything after the file name is taken as the ident, spaces
included. Without it, `fax-ident` is used.

### Fax variables

The fax applications set these channel variables:

| variable | value |
|---|---|
| `dmodem_fax_status` | `SUCCESS` or `FAILED` |
| `dmodem_fax_result` | why (for example `no document` when the file could not be read or written) |
| `dmodem_fax_pages` | the number of pages |
| `dmodem_fax_rate` | the rate, in bit/s |
| `dmodem_fax_remote` | the other machine's number |
| `dmodem_fax_ecm` | `true` when the call used error correction mode, else `false` |
| `dmodem_fax_t38` | `true` when the fax went over T.38, else `false` |
| `dmodem_fax_file` | the file |

### Fax params

These go in `<settings>` or in a profile. The fax applications read only
`<settings>`.

| param | default | meaning |
|---|---|---|
| `fax-dir` | FreeSWITCH's storage directory + `/fax` | Where `dmodem` puts the faxes it receives. |
| `fax-ident` | empty | Our fax number (CSI and TSI). |
| `fax-max-rate` | `14400` | The fastest rate. |
| `fax-ecm` | `true` | Offer error correction mode (`false`: go without). |
| `fax-t38` | `true` | Use T.38 when the channel does (`false`: keep the faxes on the audio). |

### T.38

T.38 carries fax as data packets over IP instead of as audio. On a SIP
channel, the faxes switch to T.38 when the channel does, as
mod_spandsp's `txfax` and `rxfax` do:

- `dmodem_receive_fax` asks for T.38 one second after it starts, that is
  just after the answer. (A re-INVITE sent sooner can cross the call's
  ACK; FreeSWITCH then gets a 491 and hangs up.) It waits up to 6 s for
  the answer, with silence on the line. If T.38 is refused or there is
  no answer, the fax goes on the audio.
- `dmodem_send_fax` accepts T.38 when the other end asks for it, until
  the first page has gone. After that the fax stays on the audio. (The
  same rule applies to a receiver.)
- `dmodem` takes a T.38 offer that arrives before the modems connect as a
  calling fax machine (for example a gateway's). This requires answering
  mode, `fax` and `fax-t38` on. A fax machine that `dmodem` hears on the
  audio asks for T.38 in the same way as `dmodem_receive_fax`.

FreeSWITCH refuses a T.38 offer on a channel without
`fax_enable_t38=true`. So the module sets that variable for the call
when it is not set. A call with `fax_enable_t38=false` keeps its fax on
the audio.

Over T.38 the fax machine is a T.38 terminal (libdmodem's) on sofia's
UDPTL:

- When we ask, we offer version 0, with UDP redundancy. When the other
  end asks, we answer with up to version 3, and with redundancy or none
  as the other end asked.
- We take UDPTL packets of up to 400 octets.
- The packets are paced by a clock of the module's own (a timer thread,
  as in mod_spandsp), in 20 ms steps.

The log says `over T.38` at the end of a fax that went that way.

## The channel

The modem needs the line's audio exactly as it is:

- **G.711 only, with no transcoding.** Use PCMU or PCMA. The `law` param
  picks the law; `auto` takes the channel's codec. If the channel has
  another codec, or the other law, the module asks FreeSWITCH for linear
  (L16) audio and FreeSWITCH transcodes. The module logs a warning
  (`transcoding`). Transcoding costs V.90 and lowers V.34. For a SIP
  trunk, put G.711 first or alone in the profile's codec preferences:
  `<param name="inbound-codec-prefs" value="PCMU"/>`, and the same for
  outbound.
- **No processing on the way.** No echo cancellation, silence
  suppression or comfort noise (`suppress-cng`), and no media bugs that
  change the audio. Recording is harmless: FreeSWITCH decodes and
  re-encodes G.711 without loss.
- **A jitter buffer.** FreeSWITCH reads a channel's RTP on a clock tick.
  Without a jitter buffer, a packet a few ms late is not there yet. The
  modem then gets a frame of silence, and from then on every frame one
  frame late: a slip that makes V.32bis and V.34 lose their training.
  So all three applications put a 60 ms jitter buffer on the channel,
  without packet loss concealment (`rtp_jitter_buffer_plc=false`).
  `jitter-buffer` in `<settings>` or a profile changes its length in ms
  (0: none). A channel's own `jitterbuffer_msec` takes precedence.
- **A steady RTP stream.** Packet loss and clock slips show up as line
  errors. V.42 retransmits them, but they can drop a fast connection. A
  missing packet or a comfort-noise frame reaches the modem as silence.

## Building

The module builds outside the FreeSWITCH tree, with the rest of D-Modem,
against an installed FreeSWITCH. CMake finds FreeSWITCH by its
`freeswitch.pc`, through pkg-config, or in
`/usr/local/freeswitch/lib/pkgconfig` for a source install. Without it,
CMake skips the module (`DMODEM_FREESWITCH`, on by default except on
Windows).

```
cmake -B build                      # in the top directory
cmake --build build --target mod_dmodem
cmake --install build               # the module, and dmodem.conf.xml in autoload_configs
fs_cli -x "load mod_dmodem"
```

Add `<load module="mod_dmodem"/>` to `modules.conf.xml` to load it at
start-up. CMake options for other locations:

- `-DFREESWITCH_PKGCONFIG=<dir>` for a `freeswitch.pc` elsewhere;
- `-DFREESWITCH_MODULES_DIR=...` and `-DFREESWITCH_CONF_DIR=...` for
  where `cmake --install` puts the module and the sample configuration
  (by default, the directories `freeswitch.pc` names).

It was tested with FreeSWITCH 1.11.3 built from source, with sofia-sip
1.13.19 and the spandsp of FreeSWITCH's own repository (3.1.1, which 1.11
asks for). `test/build_freeswitch.sh <prefix>` builds that, with the
modules the tests need, into `<prefix>` (as CI does). Then use
`-DFREESWITCH_PKGCONFIG=<prefix>/lib/pkgconfig` for the build and
`FS_PREFIX=<prefix>` for the test scripts.

The profiles are read on every call. `debug` is read when the module
loads.

Each call runs its modem in the session's own thread. There is no limit
on the number of calls other than CPU: a V.34 or V.90 call takes a few
percent of a core.

## What the server or program sees

The server or program sees nothing until the modems are connected. Then
it gets the bytes from the remote modem, after V.42 and V.44 or V.42bis.
A server in `mode` `unix` (a stream socket at `path`) gets exactly what a
server in `mode` `tcp` gets.

How the call ends:

- When the server or program ends its side (closes the socket, or the
  program exits), the module hangs up once everything it sent last is on
  the line: when nothing is left to send and at least 1 s has passed, or
  after 30 s at most.
- When the remote modem hangs up, the module closes the socket, or hangs
  up the pty (the program gets SIGHUP). A program still running 5 s after
  the SIGHUP is killed with SIGKILL.

A program in `mode` `exec` gets the pty as its stdin, stdout, stderr and
controlling terminal, in raw mode. Its environment has:

| variable | value |
|---|---|
| `DMODEM_CHANNEL` | the FreeSWITCH channel's name |
| `DMODEM_UUID` | its uuid |
| `DMODEM_PROFILE` | the profile |
| `DMODEM_RATE` | the connect rate, transmit/receive bit/s |
| `DMODEM_CALLERID` | the caller's number (`caller_id_number`) |
| `DMODEM_DIALED` | the number dialled (`destination_number`) |
| `DMODEM_CONNECTED` | when the modems connected, in Unix time |

A server has no environment. Instead, with `header` `true` in a `tcp` or
`unix` profile, the same variables are sent first on the connection: one
`NAME=value` per line (a value with a line break has it replaced by a
space), then an empty line, then the caller's bytes:

```
DMODEM_CHANNEL=sofia/internal/5559999@192.0.2.1
DMODEM_UUID=6f1c0b9e-1d2a-4b0e-9a57-3c2d8e4f5a61
DMODEM_PROFILE=menu
DMODEM_RATE=33600/31200
DMODEM_CALLERID=5559999
DMODEM_DIALED=5551000
DMODEM_CONNECTED=1791666309

```

[`modemmenu`](https://github.com/D-Modem/menu) uses these variables. It
offers a login (from a text file, LDAP, SQL or PAM) and a menu of a BBS,
PPP and a shell, each started with the caller's credentials and these
details of the call. It runs either as a `mode` `exec` program for each
call, or as one server for all calls (`modemmenu -serve`) behind a
`mode` `unix` profile with `header` `true`.

When it returns, `dmodem` sets these channel variables:

- `dmodem_status`:
  - `NOCARRIER`: the modems did not connect (including within the
    profile's `timeout`);
  - `REMOTE`: the remote modem went away;
  - `LOCAL`: the server or program ended the session;
  - `HANGUP`: the channel hung up;
  - `FAX`: a fax machine called and the fax receiver took the call;
  - `ERROR`: a configuration or system error (no such profile, an
    `answer` line that did not answer, a server that could not be
    reached, and so on).
- `dmodem_rate`: the connect rate, transmit/receive bit/s.

## Testing

The scripts in `test/` start a private FreeSWITCH. It is the installed
one (`FS_PREFIX`, default `/usr/local/freeswitch`), with the module built
here and the configuration in `test/freeswitch.xml`: one SIP profile on
127.0.0.1:5080, G.711 only. The scripts take the binaries from `build/`
(`DMODEM_BUILD` names another build directory). They reuse the Asterisk
tests' BBS and user scripts (`frontend/asterisk/test/bbs_sim.py`,
`frontend/asterisk/test/sip_call.py`).

```
test/run.sh                       # dmodem calls dmodem over SIP, V.34
test/run.sh AT+MS=122             # V.22bis (also 22, 32, 132, ...)
test/run.sh AT+MS=34 bbsexec      # the BBS as a program on the pty
test/run.sh AT+MS=34 bbsalaw      # the answerer on A-law: transcoded
LAW=alaw test/run.sh              # on PCMA
ANSWER='AT+MS=132;A' EXPECT='ANSWERER .*dmodem_rate=14400/' test/run.sh
                                  # the answer command line: V.32bis
test/sip.sh                       # D-Modem (dmodemd) calls dmodem
INCOMING=1 test/sip.sh            # dmodem calls D-Modem
CODEC=PCMA test/sip.sh            # the same in A-law
test/fax.sh                       # faxes, also against mod_spandsp
T38=yes test/fax.sh               # the same over T.38
```

### run.sh

In `run.sh`, FreeSWITCH calls itself:

- The answering `dmodem` serves the test BBS.
- The calling one (`dmodem term,o`) hands its data to a script playing
  the user.
- The user reads the banner, sends 20 lines, checks their echoes and logs
  off. The BBS then closes, which hangs up the call.

`EXPECT` is a regular expression that a line of the log must match.

### fax.sh

`fax.sh` sends a two-page fax (the test document of `dsp/test/fax_doc`)
six ways, or seven with `T38=yes`:

1. `dmodem_send_fax` to `dmodem`, which has to notice the fax machine;
2. `dmodem_send_fax` to `dmodem_receive_fax`;
3. mod_spandsp's `txfax` to `dmodem`;
4. `txfax` to `dmodem_receive_fax`;
5. `dmodem_send_fax` to `rxfax`;
6. `txfax` with `fax_enable_t38=false` to `dmodem_receive_fax`;
7. with `T38=yes` only: `dmodem_receive_fax` to `dmodem`, which takes its
   T.38 request before the modems connect as a calling fax machine. Both
   ends are receivers, so there is no document; only that is checked.

Each time it compares what arrived with what was sent.

- The `rxfax` leg gets a jitter buffer as well, for the same reason as
  the module's (see [The channel](#the-channel)).
- mod_spandsp is set up with no page header and with error correction
  mode, which the module then uses.
- By default the faxes are on the audio. `T38=yes test/fax.sh` turns T.38
  on for both machines (the module's `fax-t38`, and mod_spandsp's
  `enable-t38` and `enable-t38-request`). It then checks that each of the
  module's faxes went over T.38, except the sixth's (the module's request
  is refused, so it stays on the audio).
- `CASES="2 5"` runs only those cases; `FAX_MAX_RATE=9600` keeps the
  module's fax no faster; `DMODEM_DEBUG=3` puts the DSP traces in
  `freeswitch.log`.

## Debugging

`<param name="debug" value="3"/>` in `<settings>`, with the console or
log file at the DEBUG level, shows the modem's handshake. (The levels, as
`dmodem.conf.xml` describes them: 0 none, 1 the connection, 3 the
handshake, 5 everything.)
