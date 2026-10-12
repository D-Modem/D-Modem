# app_dmodem: a data modem on an Asterisk channel

`DModem()` answers a call with a software modem and, once the modems are
connected, hands the data to a BBS over TCP or to a program on a pty
(pppd for dial-up IP, a login, a door).  The profile in `dmodem.conf`
decides which, so one Asterisk can serve several numbers in several ways:

```
; extensions.conf
exten => 5551000,1,DModem(bbs)
 same => n,Hangup()
exten => 5552000,1,DModem(ppp)
 same => n,Hangup()
```

```
; dmodem.conf
[bbs]
mode = tcp
host = 127.0.0.1
port = 2323

[ppp]
mode = exec
command = exec /usr/sbin/pppd nodetach noauth local lock passive 192.168.77.1:192.168.77.2
```

See [dmodem.conf.sample](dmodem.conf.sample) for all the options.

`DModem(profile,o)` makes the modem the caller instead (it waits for the
remote modem's answer tone), on a call Asterisk has placed to a remote
modem, e.g. `channel originate PJSIP/5559999@trunk application
DModem(isp,o)`; the profile's server or program then talks to the remote
BBS or ISP.

The modem is `libdmodem` ([../slmodemd/dmodem.h](../slmodemd/dmodem.h)):
the slmodemd modem core (AT commands, V.42 error correction, V.44 or V.42bis
compression) with the open data pumps of `dsp`.  It answers
V.21, V.22, V.22bis, V.23, Bell 103/212A, V.32, V.32bis and V.34, and
V.90 as the digital (server) modem: up to 56000 bit/s downstream to a
V.90 client modem when the call is G.711 end to end.  The module has no
binary-only parts.

## Fax

The module is also a group 3 fax machine of its own (T.30 on V.17, V.29
and V.27ter, 2400 to 14400 bit/s, pages in MH or MR coding, TIFF files,
error correction mode when the other machine has it), an alternative to
Asterisk's `res_fax` with spandsp:

- `DModem()` receives faxes by itself: when the calling machine's CNG
  tone comes before a modem connection, the call goes to the fax
  receiver, which writes `<fax_dir>/<date>-<time>-<caller>-<uniqueid>.tif`
  (`fax = no` in the profile turns this off).  `DMODEMSTATUS` is then
  `FAX`.
- `DModemReceiveFax(file[,ident])` answers as a fax machine and receives
  into the file.
- `DModemSendFax(file[,ident])` sends a TIFF file on a call placed to a
  fax machine, e.g.

  ```
  channel originate PJSIP/5551234@trunk application DModemSendFax /var/spool/fax/doc.tif
  ```

  Any TIFF of 1 bit pels goes: uncompressed, G3 1D/2D or G4, of any width
  (fitted to the fax's 1728 pels) and resolution (standard, fine;
  superfine is halved).  `gs -sDEVICE=tiffg4 -r204x196 -o doc.tif doc.pdf`
  makes one from a PDF.

They set `DMODEMFAXSTATUS` (`SUCCESS` or `FAILED`), `DMODEMFAXRESULT`
(why), `DMODEMFAXPAGES`, `DMODEMFAXRATE`, `DMODEMFAXREMOTE` (the other
machine's number), `DMODEMFAXECM` (`yes` when the call had error
correction mode) and `DMODEMFAXFILE`.  `fax_dir`, `fax_ident` (our
number), `fax_max_rate` and `fax_ecm` are set in `[general]` or per
profile.

In error correction mode (T.30 Annex A) the page goes in blocks of HDLC
frames, and the receiving machine asks again for the frames a line hit
spoiled: the page comes as it went, where without it the rows hit come
damaged.  Both machines offer it (`fax_ecm = no` to go without).

## The channel

The modem needs the line's audio as it is, so:

- G.711 (ulaw or alaw) only, no transcoding: a codec change, even
  between the two laws, costs V.90 and lowers V.34.  `law` in the
  profile picks the law, `auto` takes the channel's.
- No echo cancellation, jitter buffer, silence suppression or comfort
  noise between the modem and the network, and no DSP features (fax or
  DTMF detection) on the channel.  For a SIP trunk, a `pjsip.conf`
  endpoint along the lines of:

  ```
  [modem-trunk]
  type = endpoint
  disallow = all
  allow = ulaw          ; or alaw: the law of the PSTN side
  direct_media = no
  ```

- The RTP stream should be steady: packet loss or clock slips show as
  line errors, which V.42 retransmits but which can drop a fast connect.

## Building

The module builds outside the Asterisk tree, with the rest of D-Modem,
against the headers of an installed Asterisk of the same version (`make
install-headers` after building Asterisk, or a distribution's
`asterisk-dev` package); without them CMake skips it:

```
cmake -B build                      # in the top directory
cmake --build build --target app_dmodem
cmake --install build               # the module, and dmodem.conf.sample
asterisk -rx "module load app_dmodem.so"
```

`-DASTERISK_INCLUDE_DIR=/opt/asterisk/include` for headers elsewhere,
`-DASTERISK_MODULES_DIR=...` and `-DASTERISK_CONF_DIR=...` for where
`cmake --install` puts the module and the sample configuration.  The
test scripts in `test/` take the binaries from `build/` (`DMODEM_BUILD`
for another build directory).  Each call
runs its modem in the channel's own thread; there is no limit on the
number of calls other than CPU (a V.34 or V.90 call takes a few percent
of a core).

## What the server or program sees

Nothing until the modems are connected; then the bytes from the remote
modem, after V.42 and V.44 or V.42bis.  The server or program ending its side
(closing the socket, the program exiting) hangs up the call once what it
sent last is on the line; the remote modem hanging up closes the socket
or hangs up the pty (SIGHUP).

A program in `mode = exec` gets the pty as its stdin, stdout, stderr and
controlling terminal (raw mode), and in its environment:

| variable | |
|---|---|
| `DMODEM_CHANNEL` | the Asterisk channel |
| `DMODEM_PROFILE` | the profile |
| `DMODEM_RATE` | the connect rate, transmit/receive bit/s |
| `DMODEM_CALLERID` | the caller's number |

On exit `DModem()` sets `DMODEMSTATUS` (`NOCARRIER`, `REMOTE`, `LOCAL`,
`HANGUP` or `ERROR`) and `DMODEMRATE`.

## Testing

`test/run.sh` starts a private Asterisk (the installed one, with the
module built here) and calls one `DModem()` from another over a Local
channel: the answering one serves a test BBS, the calling one
(`DModem(term,o)`) hands its data to a script playing the user, who
reads the banner, sends 20 lines, checks their echoes and logs off;
the BBS closing then hangs up the call.

```
test/run.sh                       # V.34
test/run.sh AT+MS=122             # V.22bis (also 22, 32, 132)
test/run.sh AT+MS=34 bbsexec      # the BBS as a program on the pty
LAW=alaw test/run.sh              # on A-law
```

A Local channel has no clock: the two modems run sample for sample
faster than real time, which they don't mind.

`test/fax.sh` sends a two page fax from `DModemSendFax` to `DModem()`
(which has to notice the fax machine) and to `DModemReceiveFax`, and
compares what came with what went.  `test/fax_spandsp.sh` does the same
against Asterisk's own fax (`res_fax_spandsp`): `SendFAX` to `DModem()`
and to `DModemReceiveFax`, and `DModemSendFax` to `ReceiveFAX`, all at
14400 bit/s (`CASES`, `FAX_MAX_RATE`, `DMODEM_DEBUG` in the script).

## Debugging

`debug = 3` in `[general]` and `core set verbose 5` show the modem's
handshake on the console.  The same modem runs outside Asterisk in
`dsp/test/dmodem_test` (two modems of the library back to back)
and, as slmodemd, against the original binary library in
`dsp/test/interop.py`.
