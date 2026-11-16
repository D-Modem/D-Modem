# mod_dmodem: a data modem on a FreeSWITCH channel: technical details

This page gives the technical details for [FreeSWITCH: mod_dmodem](freeswitch.md), the plain-English overview. Read the overview first. This page keeps the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

`mod_dmodem` is the FreeSWITCH version of [Asterisk's app_dmodem](asterisk.md).
It has the same modem and fax, with the applications,
configuration and channel variables of FreeSWITCH. The two modules share the code that
connects the modem to a server or program (`frontend/pbx/dmodem_bridge.c`).

## Overview

The module adds three applications:

| application | what it does |
|---|---|
| `dmodem [profile][,o]` | Answers the call with a software modem. When the modems connect, it gives the data to a server or program. |
| `dmodem_receive_fax <file> [ident]` | Answers as a fax machine and receives into a TIFF file. |
| `dmodem_send_fax <file> [ident]` | Sends a TIFF file as a fax on a call made to a fax machine. |

`dmodem` gives the data to one of three destinations, as the profile in
`dmodem.conf` tells:

- a server over TCP (`mode` `tcp`), for example a BBS;
- a server on a unix socket (`mode` `unix`), for example a login menu;
- a program on a pty (`mode` `exec`), for example `pppd` for dial-up IP, a
  login or a BBS door.

Without a profile argument, `dmodem` uses the profile `default`. One
FreeSWITCH can serve many numbers in many methods:

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

[dmodem.conf.xml](../frontend/freeswitch/dmodem.conf.xml) gives all the options.

### Profile params

| param | default | meaning |
|---|---|---|
| `mode` | `tcp` | `tcp`, `unix` or `exec`. |
| `host`, `port` | none | `mode` `tcp`: the server. The two params are necessary. |
| `path` | none | `mode` `unix`: the stream socket of the server. Necessary. |
| `command` | none | `mode` `exec`: the program, run by `/bin/sh -c`. Necessary. |
| `header` | `false` | `tcp` and `unix` only: send the details of the call first (refer to [below](#what-the-server-or-program-sees)). |
| `handoff` | `false` | `unix` with `header` only: give the line of the modem itself to the server (refer to [below](#what-the-server-or-program-sees)). |
| `init` | empty | More AT commands for the modem, in the two directions (for example `+MS=34`). |
| `answer` | `ATA` | The command line with which the modem answers. |
| `law` | `auto` | The G.711 law: `ulaw`, `alaw`, or `auto` for the codec of the channel. |
| `clear` | `auto` | `true`: each call is a clear 64 kbit/s channel (`AT+MS=CLEAR` on the octets of the channel without changes, as for an ISDN data call). FreeSWITCH has no digital-bearer calls. Thus, here `auto` means `false`. |
| `timeout` | `60` | The time in seconds in which the modems must connect. Then (`tcp`, `unix`), the time in seconds in which the server must accept the call. If not, the call stops with `ERROR`. |
| `jitter-buffer` | `60` | The jitter buffer in ms, as in `<settings>` (refer to [The channel](#the-channel)). |
| `fax` | `true` | Receive a calling fax machine as a fax (refer to [Fax](#fax)). |

A profile can also hold any of the fax params of `<settings>`. The module compares param
names without case, and `_` and `-` are the same.
The module refuses a profile that does not have the necessary params of its mode. Then
`dmodem` stops with `dmodem_status` `ERROR`.

### Answering and calling

The modem is first configured with `ATE0Q1S0=0S2=255X3` (no echo, no result
codes, no auto-answer, no `+++` escape, no wait for a dial tone).
Then the `init` of the profile follows.

By default, the modem answers with `ATA`. The `answer` param of a profile
gives a different command line. Commands that apply only to the
answering call go before the `A`. For example,
`<param name="answer" value="AT+MS=132;A"/>` answers at V.32bis at most
(in XML, you do not have to escape the `;`). If the modem does not answer after the
command line, the call stops with `dmodem_status` `ERROR`.

With `dmodem <profile>,o` (or `<profile> o`), our modem is the caller. It
waits for the answer tone of the remote modem. This is for a call that
FreeSWITCH makes to a remote modem, for example:

```
originate sofia/gateway/trunk/5559999 &dmodem(isp,o)
```

Then the server or program of the profile communicates with the remote BBS or ISP. In
`originate`, the comma form does not need quotes. An application argument
with a space must have quotes, as in
`'&dmodem_send_fax(/tmp/doc.tif +1 555 0100)'`.

### The modem

The modem is `libdmodem` ([frontend/dmodemd/dmodem.h](../frontend/dmodemd/dmodem.h)). It
is the modem core of D-Modem (AT commands, V.42 error correction, V.44 or
V.42bis compression) with the open data pumps of `dsp`. It supports:

- V.21, V.22, V.22bis, V.23, Bell 103 and 212A, V.32, V.32bis and V.34;
- V.90 as the digital (server) modem: up to 56000 bit/s downstream to a
  V.90 client modem, when the call is G.711 from end to end;
- V.91 with a different digital modem, for example a different D-Modem.

The module has no binary-only parts.

## Fax

The module is also a group 3 fax machine:

- T.30 on V.17, V.29 and V.27ter, at 2400 to 14400 bit/s;
- pages in MH or MR coding, in TIFF files;
- error correction mode (ECM, T.30 Annex A) when the other machine has
  it. A page goes in blocks of HDLC frames. The receiving machine
  asks again for each frame that a line hit damaged. Thus, the page arrives exactly
  as it was sent.

It is an alternative to the `txfax` and `rxfax` of mod_spandsp:

- **`dmodem` receives faxes automatically.** If the CNG tone of the calling
  machine, or its V.8 CM of a fax call, comes before a modem connection,
  the call goes to the fax receiver instead. The receiver writes
  `<fax-dir>/<date>-<time>-<caller>-<uuid>.tif`. The date and time
  are `YYYYMMDD-HHMMSS`, and the caller is the digits and `+` of the caller ID
  (or `unknown`). Then `dmodem_status` is `FAX`. If you set the `fax`
  param of the profile to `false`, this is disabled. This applies only when the module answers,
  not with the `o` option.
- **`dmodem_receive_fax <file> [ident]`** answers as a fax machine and
  receives into the file.
- **`dmodem_send_fax <file> [ident]`** sends a TIFF file on a call made
  to a fax machine, for example:

  ```
  originate sofia/gateway/trunk/5551234 &dmodem_send_fax(/var/spool/fax/doc.tif)
  ```

  It can send any TIFF of 1-bit pels: uncompressed, G3 1D or 2D, or G4.
  The TIFF can have any width (the module fits it to the 1728 pels of the fax) and any
  resolution (standard and fine go without changes; superfine is
  halved). This command makes a TIFF from a PDF:

  ```
  gs -sDEVICE=tiffg4 -r204x196 -o doc.tif doc.pdf
  ```

`ident` is our fax number (the CSI when the module receives, the TSI when it
sends). All the text after the file name is the ident, with the spaces.
Without it, the module uses `fax-ident`.

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

These params go in `<settings>` or in a profile. The fax applications read only
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
channel, the faxes change to T.38 when the channel changes, as
the `txfax` and `rxfax` of mod_spandsp do:

- `dmodem_receive_fax` asks for T.38 one second after it starts, that is,
  immediately after the answer. (A re-INVITE sent sooner can cross the
  ACK of the call. Then FreeSWITCH gets a 491 and hangs up.) It waits up to 6 s for
  the answer, with silence on the line. If T.38 is refused or there is
  no answer, the fax goes on the audio.
- `dmodem_send_fax` accepts T.38 when the other end asks for it, until
  the first page has gone. After that, the fax stays on the audio. (The
  same rule applies to a receiver.)
- `dmodem` accepts a T.38 offer that arrives before the modems connect as a
  calling fax machine (for example, the offer of a gateway). For this, the module must be in answering
  mode, with `fax` and `fax-t38` enabled. When `dmodem` hears a fax machine on the
  audio, it asks for T.38 in the same method as `dmodem_receive_fax`.

FreeSWITCH refuses a T.38 offer on a channel without
`fax_enable_t38=true`. Thus, the module sets that variable for the call
when it is not set. A call with `fax_enable_t38=false` keeps its fax on
the audio.

Over T.38, the fax machine is a T.38 terminal (the terminal of libdmodem) on the
UDPTL of sofia:

- When we ask, we offer version 0, with UDP redundancy. When the other
  end asks, we answer with up to version 3, and with redundancy or none,
  as the other end asked.
- We accept UDPTL packets of up to 400 octets.
- A clock of the module itself (a timer thread,
  as in mod_spandsp) paces the packets, in 20 ms steps.

At the end of a fax that went over T.38, the log shows `over T.38`.

## The channel

The modem must have the audio of the line without changes:

- **G.711 only, with no transcoding.** Use PCMU or PCMA. The `law` param
  selects the law. `auto` uses the codec of the channel. If the channel has
  a different codec, or the other law, the module asks FreeSWITCH for linear
  (L16) audio, and FreeSWITCH transcodes. The module logs a warning
  (`transcoding`). With transcoding, V.90 is not possible and V.34 is slower. For a SIP
  trunk, put G.711 first or alone in the codec preferences of the profile:
  `<param name="inbound-codec-prefs" value="PCMU"/>`, and the same for
  outbound.
- **No processing on the path.** No echo cancellation, silence
  suppression or comfort noise (`suppress-cng`), and no media bugs that
  change the audio. Recording has no bad effect, because FreeSWITCH decodes and
  encodes G.711 again without loss.
- **A jitter buffer.** FreeSWITCH reads the RTP of a channel on a clock tick.
  Without a jitter buffer, a packet that is some ms late is not available yet. Then the
  modem gets a frame of silence, and from that time each frame is one
  frame late. This slip makes V.32bis and V.34 lose their training.
  Thus, the three applications put a 60 ms jitter buffer on the channel,
  without packet loss concealment (`rtp_jitter_buffer_plc=false`).
  `jitter-buffer` in `<settings>` or a profile changes its length in ms
  (0: none). The `jitterbuffer_msec` of a channel has priority.
- **A steady RTP stream.** Packet loss and clock slips cause line
  errors. V.42 sends the data again, but these errors can cause a fast connection to drop. A
  missing packet or a comfort-noise frame goes to the modem as silence.

## Building

The module builds outside the FreeSWITCH tree, with the remaining parts of D-Modem,
against an installed FreeSWITCH. CMake finds FreeSWITCH by its
`freeswitch.pc`, through pkg-config, or in
`/usr/local/freeswitch/lib/pkgconfig` for a source install. Without it,
CMake does not build the module (`DMODEM_FREESWITCH`, enabled by default, but not on
Windows).

```
cmake -B build                      # in the top directory
cmake --build build --target mod_dmodem
cmake --install build               # the module, and dmodem.conf.xml in autoload_configs
fs_cli -x "load mod_dmodem"
```

To load the module at start-up, add `<load module="mod_dmodem"/>` to `modules.conf.xml`.
CMake options for other locations:

- `-DFREESWITCH_PKGCONFIG=<dir>` for a `freeswitch.pc` in a different location;
- `-DFREESWITCH_MODULES_DIR=...` and `-DFREESWITCH_CONF_DIR=...` for
  the locations where `cmake --install` puts the module and the sample configuration
  (by default, the directories that `freeswitch.pc` gives).

We tested it with FreeSWITCH 1.11.3 built from source, with sofia-sip
1.13.19 and the spandsp of the repository of FreeSWITCH (3.1.1, which 1.11
asks for). `test/build_freeswitch.sh <prefix>` builds that, with the
modules that the tests must have, into `<prefix>` (as CI does). Then use
`-DFREESWITCH_PKGCONFIG=<prefix>/lib/pkgconfig` for the build and
`FS_PREFIX=<prefix>` for the test scripts.

The module reads the profiles on each call. It reads `debug` when the module
loads.

Each call runs its modem in the thread of the session. The only limit
on the number of calls is the CPU: a V.34 or V.90 call uses some
percent of a core.

## What the server or program sees

The server or program sees nothing until the modems are connected. Then
it gets the bytes from the remote modem, after V.42 and V.44 or V.42bis.
A server in `mode` `unix` (a stream socket at `path`) gets exactly the same data as a
server in `mode` `tcp`.

How the call stops:

- When the server or program stops its side (closes the socket, or the
  program exits), the module hangs up when all the data that it sent last is on
  the line: when nothing is left to send and a minimum of 1 s has passed, or
  after 30 s at most.
- When the remote modem hangs up, the module closes the socket, or hangs
  up the pty (the program gets SIGHUP). If a program still runs 5 s after
  the SIGHUP, the module stops it with SIGKILL.

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
| `DMODEM_PEER` | the address the call came from by SIP (`sip_network_ip`), or empty |

A server has no environment. Instead, with `header` `true` in a `tcp` or
`unix` profile, the module sends the same variables first on the connection:

1. One `NAME=value` on each line (a line break in a value becomes a
   space).
2. An empty line.
3. The bytes of the caller.

```
DMODEM_CHANNEL=sofia/internal/5559999@192.0.2.1
DMODEM_UUID=6f1c0b9e-1d2a-4b0e-9a57-3c2d8e4f5a61
DMODEM_PROFILE=menu
DMODEM_RATE=33600/31200
DMODEM_CALLERID=5559999
DMODEM_DIALED=5551000
DMODEM_CONNECTED=1791666309
DMODEM_PEER=192.0.2.1

```

[`modemmenu`](https://github.com/D-Modem/menu) uses these variables. It
gives a login (from a text file, LDAP, SQL or PAM) and a menu of a BBS,
PPP and a shell. It starts each of these items with the credentials of the caller and these
details of the call. It runs as a `mode` `exec` program for each
call, or as one server for all calls (`modemmenu -serve`) behind a
`mode` `unix` profile with `header` `true`.

Each call is a separate connection to the server of the profile. Many
callers at the same time on one `modemmenu -serve` (or a BBS) socket are many
connections, each with its own header.

With `handoff = yes` also (`unix` and `header` only), the module
puts the modem on a pty. It gives the terminal of the pty to the server
and does not copy bytes to it. The header goes over the connection with
the descriptor attached (`SCM_RIGHTS`). From then, the server, and
all that it runs on that terminal (a login session, a shell, pppd), is
on the line of the modem itself, as on a serial port. The connection carries nothing more. It stays open
while the session continues. When the server closes it, the module
waits for the last data that the server wrote to go out, then hangs up (`LOCAL`).
When the remote modem goes, the server sees its end of the line close.
The server must expect the descriptor (`modemmenu -serve` and the
`handoff` listeners of the BBS do). A server that does not expect it gets the header and
nothing after it.

When it returns, `dmodem` sets these channel variables:

- `dmodem_status`:
  - `NOCARRIER`: the modems did not connect (also in the
    `timeout` of the profile);
  - `REMOTE`: the remote modem went away;
  - `LOCAL`: the server or program stopped the session;
  - `HANGUP`: the channel hung up;
  - `FAX`: a fax machine called and the fax receiver took the call;
  - `ERROR`: a configuration or system error (no such profile, an
    `answer` line that did not answer, a server that the module could not
    reach, and other errors).
- `dmodem_rate`: the connect rate, transmit/receive bit/s.

## Testing

The scripts in `test/` start a private FreeSWITCH. It is the installed
FreeSWITCH (`FS_PREFIX`, default `/usr/local/freeswitch`), with the module built
here and the configuration in `test/freeswitch.xml`: one SIP profile on
127.0.0.1:5080, G.711 only. The scripts take the binaries from `build/`
(`DMODEM_BUILD` gives a different build directory). They use again the BBS and user scripts of
the Asterisk tests (`frontend/asterisk/test/bbs_sim.py`,
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
- The calling `dmodem` (`dmodem term,o`) gives its data to a script that operates as
  the user.
- The user reads the banner, sends 20 lines, checks their echoes and logs
  out. Then the BBS closes, and this hangs up the call.

`EXPECT` is a regular expression that a line of the log must match.

### fax.sh

`fax.sh` sends a two-page fax (the test document of `dsp/test/fax_doc`)
in six ways, or seven with `T38=yes`:

1. `dmodem_send_fax` to `dmodem`, which must find the fax machine;
2. `dmodem_send_fax` to `dmodem_receive_fax`;
3. the `txfax` of mod_spandsp to `dmodem`;
4. `txfax` to `dmodem_receive_fax`;
5. `dmodem_send_fax` to `rxfax`;
6. `txfax` with `fax_enable_t38=false` to `dmodem_receive_fax`;
7. with `T38=yes` only: `dmodem_receive_fax` to `dmodem`, which accepts its
   T.38 request before the modems connect as a calling fax machine. The two
   ends are receivers. Thus, there is no document, and the test checks only that.

Each time, it compares the received fax with the sent fax.

- The `rxfax` leg also gets a jitter buffer, for the same reason as
  the module (refer to [The channel](#the-channel)).
- mod_spandsp is configured with no page header and with error correction
  mode. Then the module uses error correction mode.
- By default, the faxes are on the audio. `T38=yes test/fax.sh` enables T.38
  for the two machines (the `fax-t38` of the module, and the
  `enable-t38` and `enable-t38-request` of mod_spandsp). Then it makes sure that each of the
  faxes of the module went over T.38, but not the sixth fax (the request of the module
  is refused, thus that fax stays on the audio).
- `CASES="2 5"` runs only those cases. `FAX_MAX_RATE=9600` keeps the
  fax of the module at that rate or slower. `DMODEM_DEBUG=3` puts the DSP traces in
  `freeswitch.log`.

## Debugging

`<param name="debug" value="3"/>` in `<settings>`, with the console or
log file at the DEBUG level, shows the handshake of the modem. (The levels, as
`dmodem.conf.xml` describes them: 0 none, 1 the connection, 3 the
handshake, 5 everything.)
