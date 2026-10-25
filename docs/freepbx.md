# FreePBX: the D-Modem module

`freepbx/dmodem/` is a FreePBX module (FreePBX 16 and 17) for
[app_dmodem](asterisk.md) or [dmodem-ari](ari.md): the modem's profiles
edited in FreePBX's web interface, each a destination for inbound routes,
IVRs, time conditions and the like, and a check of the PJSIP trunks and
extensions for modem calls.  It adds Applications > D-Modem.

## Installing

1. The modem itself in Asterisk: `app_dmodem.so` built from `asterisk/`
   (Asterisk's headers installed, `cmake -B build && cmake --build build`)
   and copied to Asterisk's modules directory, then
   `asterisk -rx "module load app_dmodem.so"`.  Or dmodem-ari instead
   (`build/ari/dmodem-ari`), with an ARI user (Settings > Asterisk REST
   Interface Users).
2. The module: `cmake --build build --target freepbx_module` makes
   `build/freepbx/dmodem-<version>.tgz`.  Upload it in Admin > Module Admin
   (Upload modules), or unpack it into FreePBX's modules directory and
   install it from the command line:

   ```
   tar xzf dmodem-16.0.1.tgz -C /var/www/html/admin/modules/
   fwconsole ma installlocal dmodem
   fwconsole reload
   ```

   The module is not signed, so FreePBX shows it as unsigned.

## Using it

- **Profiles**: where a call's data goes once the modems connect: a TCP
  server (a BBS), a unix socket, or a program on a pty (pppd for dial-up
  IP, a login, a door).  Each has its G.711 law, extra AT commands, the
  answer command (`AT+MS=132;A` answers at V.32bis at most), the connect
  timeout and whether fax machines calling are received as faxes.  A
  profile `bbs` is the destination `app-dmodem,bbs,1`, listed under
  "D-Modem" in every destination picker.  A profile keeps its name (it is
  its destination), and one that a route still points at can't be
  deleted.
- **Settings**: the engine (DModem() in Asterisk, or dmodem-ari's
  `Stasis()`), the modem's log level, and the fax settings of `[general]`:
  the directory received faxes go to, our fax number, the fastest rate,
  error correction and T.38.  With "Fax destination" on, "D-Modem: receive
  a fax" (`app-dmodem,receivefax,1`) answers as a fax machine.
- **Line check**: each PJSIP endpoint as Asterisk has it now (through the
  manager), with what matters to a modem call: codecs other than G.711,
  both laws allowed (a call can be converted between them, which costs
  V.90), DTMF in the audio (`inband`, `auto`), fax detection, direct media
  and T.38.  And whether app_dmodem is loaded, or dmodem-ari connected.
- **Generated files**: what Apply Config writes.

Apply Config writes `/etc/asterisk/dmodem.conf` (FreePBX's header first;
don't edit it by hand, the next reload writes it again) and the
`app-dmodem` context into `extensions_additional.conf`:

```
[app-dmodem]
exten => bbs,1,NoOp(D-Modem profile bbs)
exten => bbs,n,DModem(bbs)
exten => bbs,n,NoOp(D-Modem bbs: ${DMODEMSTATUS} ${DMODEMRATE})
exten => bbs,n,Hangup()
exten => receivefax,1,NoOp(D-Modem fax)
exten => receivefax,n,DModemReceiveFax(/var/spool/asterisk/fax/${STRFTIME(${EPOCH},,%Y%m%d-%H%M%S)}-${FILTER(0-9A-Za-z+,${CALLERID(num)})}-${UNIQUEID}.tif)
...
```

app_dmodem reads `dmodem.conf` on each call, so a change counts from the
next call.  dmodem-ari reads it when it starts: restart it after Apply
Config.

## The calls' path

V.90 needs the call's G.711 untouched from the network to the modem
([asterisk.md](asterisk.md#the-channel)).  In FreePBX:

- Settings > Asterisk SIP Settings, the trunk's and the modem extension's
  codecs: one law only, the one your provider uses (ulaw in North America
  and Japan, alaw elsewhere).  Your provider must not transcode either.
- On those endpoints: DTMF RFC 4733, Fax Detect off, T.38 off where no fax
  is meant to use it.
- On the modem's inbound route: Detect Faxes no, no call recording, no
  music on hold or announcement before the answer.

The line check shows what is still wrong.

## The module's code

- `Dmodem.class.php`: the BMO class: the forms (`doConfigPageInit()`), the
  pages, `genConfig()`/`writeConfig()` for `dmodem.conf`, the dialplan hook,
  the destinations and the line check.  The settings and profiles live in
  the module's kvstore.
- `lib/Config.php`: the settings' and profiles' defaults and checks, and
  `dmodem.conf` from them (a `;` in a value written `\;`, as Asterisk and
  dmodem-ari read it; no line breaks get through).
- `lib/Dialplan.php`: the `app-dmodem` context.
- `lib/LineCheck.php`: `pjsip show endpoint(s)`, `module show` and
  `ari show apps` read, and what an endpoint's settings mean for a modem.
- `functions.inc.php`: FreePBX's destination hooks.
- `views/`, `assets/js/dmodem.js`: the pages.

## Tests

- `freepbx/test/unit.php` (ctest `freepbx_unit`, label quick, when PHP is
  installed): the module without FreePBX, on stand-ins for what it uses
  (`fpbx_env.php`: the kvstore with DB_Helper's rules, where storing false
  deletes a key and values come back as strings; `extensions::add()` and
  its output as FreePBX's `extensions.class.php` has them; the manager;
  the destination usage).  The forms with good and bad values,
  `dmodem.conf` read back as dmodem-ari reads it, the dialplan for both
  engines, the destinations, the line check on Asterisk 20's own CLI
  output (`test/fixtures/`), and the pages.
  `FREEPBX_FRAMEWORK=<a checkout of FreePBX's framework>` runs the dialplan
  through FreePBX's own `extensions.class.php` instead of the stand-in.
- `freepbx/test/e2e.sh` (`ARI=1`: dmodem-ari): the module's files in a
  private Asterisk.  `gen.php` makes them the way the page does (its forms
  posted), an inbound route sends the call to `app-dmodem,bbs,1`, and
  dmodemd calls it over SIP and uses the BBS (V.90 at 62666); then the line
  check runs on the live endpoints.
