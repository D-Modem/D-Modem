# mod_dmodem: a data modem on a FreeSWITCH channel

The FreeSWITCH version of [Asterisk's app_dmodem](asterisk.md):
the same modem and fax, with FreeSWITCH's applications, configuration and
channel variables.

The `dmodem` application answers a call with a software modem. Once the
two modems are connected, it hands the data to a BBS over TCP or to a
program on a pty (pppd for dial-up IP, a login, a door). The profile in
`dmodem.conf` decides which, so one FreeSWITCH can serve several numbers
in several ways:

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

[dmodem.conf.xml](../freeswitch/dmodem.conf.xml) lists all the options.

The modem answers with `ATA`. A profile's `answer` param gives the
command line it answers with instead, with commands for the answering
call alone before the `A`: `<param name="answer" value="AT+MS=132;A"/>`
answers at V.32bis at most. `init` has the commands for both directions.
A line that leaves the modem not answering ends the call, `dmodem_status`
`ERROR`.

With `dmodem <profile>,o` (or `<profile> o`), our modem is the caller:
it waits for the remote modem's answer tone. This is for a call that
FreeSWITCH places to a remote modem, for example
`originate sofia/gateway/trunk/5559999 &dmodem(isp,o)`. The profile's
server or program then talks to the remote BBS or ISP. In `originate`,
the comma form needs no quoting; an application argument with a space
does, as in `'&dmodem_send_fax(/tmp/doc.tif +1 555 0100)'`.

The modem is `libdmodem` ([dmodemd/dmodem.h](../dmodemd/dmodem.h)):
D-Modem's modem core (AT commands, V.42 error correction, V.44 or V.42bis
compression) with the open data pumps of `dsp`. It supports:
- V.21, V.22, V.22bis, V.23, Bell 103 and 212A, V.32, V.32bis and V.34;
- V.90 as the digital (server) modem: up to 56000 bit/s downstream to a
  V.90 client modem, when the call is G.711 end to end.

The module has no binary-only parts.

## Fax

The module is also a group 3 fax machine:
- T.30 on V.17, V.29 and V.27ter, 2400 to 14400 bit/s;
- pages in MH or MR coding, TIFF files;
- error correction mode (T.30 Annex A) when the other machine has it:
  the page goes in blocks of HDLC frames, and the receiving machine asks
  again for those a line hit spoiled, so the page comes as it went.

It is an alternative to mod_spandsp's `txfax`/`rxfax`:

- `dmodem` receives faxes by itself. When the calling machine's CNG
  tone comes before a modem connection, the call goes to the fax
  receiver, which writes `<fax-dir>/<date>-<time>-<caller>-<uuid>.tif`.
  The profile's `fax` param set to `false` turns this off. `dmodem_status`
  is then `FAX`.
- `dmodem_receive_fax <file> [ident]` answers as a fax machine and
  receives into the file.
- `dmodem_send_fax <file> [ident]` sends a TIFF file on a call placed to
  a fax machine, for example:

  ```
  originate sofia/gateway/trunk/5551234 &dmodem_send_fax(/var/spool/fax/doc.tif)
  ```

  Any TIFF of 1 bit pels goes: uncompressed, G3 1D or 2D, or G4, of any
  width (fitted to the fax's 1728 pels) and any resolution (standard and
  fine; superfine is halved). `gs -sDEVICE=tiffg4 -r204x196 -o doc.tif doc.pdf`
  makes one from a PDF.

The fax applications set these channel variables:

| variable | |
|---|---|
| `dmodem_fax_status` | `SUCCESS` or `FAILED` |
| `dmodem_fax_result` | why |
| `dmodem_fax_pages` | the pages |
| `dmodem_fax_rate` | bit/s |
| `dmodem_fax_remote` | the other machine's number |
| `dmodem_fax_ecm` | `true` when the call had error correction mode |
| `dmodem_fax_file` | the file |

`fax-dir`, `fax-ident` (our number), `fax-max-rate` and `fax-ecm` (`false`
to go without error correction mode) go in `<settings>` or in a profile;
the fax applications take `<settings>`.

## The channel

The modem needs the line's audio exactly as it is:

- **G.711 only, no transcoding.** Use PCMU or PCMA. The `law` param
  picks the law; `auto` takes the channel's codec. If the channel has
  another codec, or the other law, the module asks FreeSWITCH for linear
  audio and FreeSWITCH transcodes. It logs a warning. That costs V.90 and
  lowers V.34. For a SIP trunk, put G.711 first or alone in the profile's
  codec-prefs: `<param name="inbound-codec-prefs" value="PCMU"/>`, and
  the same for outbound.
- **No processing on the way.** No echo cancellation, silence
  suppression or comfort noise (`suppress-cng`), and no media bugs that
  change the audio. Recording is harmless: FreeSWITCH decodes and
  re-encodes G.711 without loss.
- **A jitter buffer.** FreeSWITCH reads a channel's RTP on a clock
  tick, and without a jitter buffer a packet a few ms late is not there
  yet. The modem then gets a frame of silence, and from then on every
  frame one frame later: a slip that makes V.32bis and V.34 lose their
  training. So the applications put a 60 ms jitter buffer on the channel,
  without packet loss concealment (`rtp_jitter_buffer_plc=false`).
  `jitter-buffer` in `<settings>` or a profile changes its length (ms;
  0: none), and a channel's own `jitterbuffer_msec` takes precedence.
- **A steady RTP stream.** Packet loss or clock slips show as line
  errors. V.42 retransmits them, but they can drop a fast connect.
  A missing packet or a comfort-noise frame reaches the modem as silence.

## Building

The module builds outside the FreeSWITCH tree, with the rest of D-Modem,
against an installed FreeSWITCH. CMake finds it by its `freeswitch.pc`:
pkg-config, or `/usr/local/freeswitch/lib/pkgconfig` for a source install.
Without it, CMake skips the module.

```
cmake -B build                      # in the top directory
cmake --build build --target mod_dmodem
cmake --install build               # the module, and dmodem.conf.xml in autoload_configs
fs_cli -x "load mod_dmodem"
```

Add `<load module="mod_dmodem"/>` to `modules.conf.xml` to load it at
start. Other locations:
- `-DFREESWITCH_PKGCONFIG=<dir>` for a `freeswitch.pc` elsewhere;
- `-DFREESWITCH_MODULES_DIR=...` and `-DFREESWITCH_CONF_DIR=...` for
  where `cmake --install` puts the module and the sample configuration.

It was tested with FreeSWITCH 1.10.12 built from source (sofia-sip, and
the spandsp of FreeSWITCH's own repository at the commit its 1.10.12
build uses, which `mod_spandsp` needs). `test/build_freeswitch.sh <prefix>`
builds that, with the modules the tests need, into `<prefix>` (as CI
does); then `-DFREESWITCH_PKGCONFIG=<prefix>/lib/pkgconfig`, and
`FS_PREFIX=<prefix>` for the test scripts.

Each call runs its modem in the session's own thread. There is no limit
on the number of calls other than CPU: a V.34 or V.90 call takes a few
percent of a core.

## What the server or program sees

Nothing until the modems are connected. Then it gets the bytes from the
remote modem, after V.42 and V.44 or V.42bis.

When the server or program ends its side (closes the socket, or the
program exits), the call hangs up once what it sent last is on the line.
When the remote modem hangs up, the socket closes, or the pty hangs up
(SIGHUP).

A program in `mode` `exec` gets the pty as its stdin, stdout, stderr and
controlling terminal (raw mode), and these in its environment:

| variable | |
|---|---|
| `DMODEM_CHANNEL` | the FreeSWITCH channel's name |
| `DMODEM_UUID` | its uuid |
| `DMODEM_PROFILE` | the profile |
| `DMODEM_RATE` | the connect rate, transmit/receive bit/s |
| `DMODEM_CALLERID` | the caller's number |

When it returns, `dmodem` sets these channel variables:
- `dmodem_status`: `NOCARRIER`, `REMOTE`, `LOCAL`, `HANGUP`, `FAX` or
  `ERROR`;
- `dmodem_rate`.

## Testing

The scripts in `test/` start a private FreeSWITCH. It is the installed
one, with the module built here and the configuration of
`test/freeswitch.xml`: one SIP profile on 127.0.0.1:5080, G.711 only.
The scripts take the binaries from `build/` (`DMODEM_BUILD` for another
build directory) and FreeSWITCH from `FS_PREFIX` (default
`/usr/local/freeswitch`). They use the Asterisk tests' BBS and user
scripts.

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
test/fax.sh                       # faxes, also against mod_spandsp
```

In `run.sh`, FreeSWITCH calls itself. The answering `dmodem` serves the
test BBS. The calling one (`dmodem term,o`) hands its data to a script
playing the user. The user reads the banner, sends 20 lines, checks
their echoes and logs off; the BBS closing then hangs up the call.

`fax.sh` sends a two page fax five ways:
1. `dmodem_send_fax` to `dmodem`, which has to notice the fax machine;
2. `dmodem_send_fax` to `dmodem_receive_fax`;
3. mod_spandsp's `txfax` to `dmodem`;
4. `txfax` to `dmodem_receive_fax`;
5. `dmodem_send_fax` to `rxfax`.

Each time it compares what came with what went. The `rxfax` leg gets a
jitter buffer as well, for the same reason as the module's (see
[The channel](#the-channel)), and mod_spandsp no page header and error
correction mode, which the module then uses.

## Debugging

`<param name="debug" value="3"/>` in `<settings>`, with the console or
log file at the DEBUG level, shows the modem's handshake.
