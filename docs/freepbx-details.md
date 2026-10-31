# FreePBX: the D-Modem module: technical details

This is the technical companion to [FreePBX: the D-Modem module](freepbx.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

`freepbx/dmodem/` is a FreePBX module (version 16.0.2, for FreePBX 16
and 17) for [app_dmodem](asterisk.md) or [dmodem-ari](ari.md). It
provides:

- the modem's profiles, edited in FreePBX's web interface, each a
  destination for inbound routes, IVRs, time conditions and the like;
- calls out to a remote modem with a profile;
- the settings and profiles in FreePBX's Backup & Restore;
- a check of the PJSIP trunks and extensions for modem calls.

It adds **Applications > D-Modem** to the menu and the `fwconsole dmodem`
command. The module is PHP only; nothing in it is compiled.

## Installing

1. **The modem.** Choose one engine:
   - **dmodem-ari** beside Asterisk (`build/ari/dmodem-ari`), with an ARI
     (Asterisk REST Interface) user in Settings > Asterisk REST
     Interface Users. This is the default engine.
   - **app_dmodem**, on an Asterisk you build it for yourself. With
     Asterisk's headers installed, run
     `cmake -B build -DDMODEM_APP_DMODEM=ON && cmake --build build`, copy
     `app_dmodem.so` to Asterisk's modules directory, then run
     `asterisk -rx "module load app_dmodem.so"`. A built app_dmodem may
     not be passed on to anyone ([LICENSING.md](../LICENSING.md)).
2. **The module.** `cmake --build build --target freepbx_module` makes
   `build/freepbx/dmodem-<version>.tgz`. Upload it in Admin > Module
   Admin (Upload modules), or unpack it into FreePBX's modules directory
   and install it from the command line:

   ```
   tar xzf dmodem-16.0.2.tgz -C /var/www/html/admin/modules/
   fwconsole ma installlocal dmodem
   fwconsole reload
   ```

   The module is not signed, so FreePBX shows it as unsigned.

## Using it

The module's page has five parts.

### Profiles

A profile says where a call's data goes once the modems connect:

- a TCP server (a BBS);
- a server on a unix socket;
- a program on a pty (`pppd` for dial-up IP, a login, a door).

Each profile also has:

| field | default | notes |
|---|---|---|
| G.711 law | `auto` | `auto`, `ulaw` or `alaw`. |
| ISDN data calls (`clear`) | `auto` | `auto`: a clear channel for a call with a digital bearer; `yes`: every call; `no`: never. A CLEARMODE call is clear all the same. See [isdn.md](isdn.md). |
| Modem commands (`init`) | empty | Extra AT commands, starting with `AT`, 200 characters at most. |
| Answer command (`answer`) | empty (`ATA`) | For example `AT+MS=132;A` answers at V.32bis at most. |
| Connect timeout | 60 s | 5 to 600 s. |
| Take fax calls (`fax`) | on | Receive a calling fax machine as a fax. |
| Call details first (`header`) | off | TCP and unix-socket servers only. |
| Description | empty | One line, 100 characters at most; shown with the destination. |

A profile name has 1 to 40 characters (lower-case letters, digits, `-`
and `_`, starting with a letter or digit). The names `general`,
`receivefax` and `sendfax` are reserved.

A profile `bbs` is the destination `app-dmodem,bbs,1`, listed under
"D-Modem" in every destination picker. A profile keeps its name once
saved, because the name is its destination. A profile that a route or
another object still points at cannot be deleted.

### Settings

- **Engine**: dmodem-ari's `Stasis()` (the default), or `DModem()` in an
  Asterisk that has app_dmodem built for itself.
- **Log level**: the modem's `debug`, 0 to 3.
- **dmodem-ari** (engine `ari` only): the ARI URL
  (`http://127.0.0.1:8088` by default), user (`dmodem`), password
  (left empty on the form, the stored one is kept) and Stasis app name
  (`dmodem`). Optionally, T.38 through dmodem-ari's own SIP side, with
  its PJSIP endpoint (`dmodem`) and SIP address (`127.0.0.1:5090`).
- **Dialling out**: the dial-out context (`from-internal`), the caller ID,
  and the ring time (60 s; 5 to 300 s).
- **Fax**, the fax settings of `[general]`:
  - the directory received faxes go to (`/var/spool/asterisk/fax`);
  - our fax number (up to 20 digits, `+` and spaces);
  - the fastest rate (14400, 12000, 9600, 7200, 4800 or 2400 bit/s);
  - error correction mode (on) and T.38 (on);
  - **Fax destination** (on): a destination "D-Modem: receive a fax"
    (`app-dmodem,receivefax,1`) that answers as a fax machine.

### Dial out

A call to a remote modem (a BBS, an ISP) with a profile:

1. The number (digits, `*`, `#` and `+`, 40 at most) is dialled in the
   dial-out context. That is `from-internal` by default, so your outbound
   routes pick the trunk, as for an extension's call.
2. The call uses the settings' caller ID (or the one given) and ring
   time.
3. Once the far end answers, D-Modem is the calling modem:
   `DModem(<profile>,o)`, or `Stasis(<app>,<profile>,o)` with dmodem-ari.
   The profile's server or program then talks to the remote end.

The module asks Asterisk's manager (AMI) to originate the call, with the
channel `Local/<number>@<context>` and the channel variable
`DMODEM_DIALOUT=<profile>`.

### Line check

The line check reads each PJSIP endpoint (up to 200) as Asterisk has it
now, through the manager, and reports what matters to a modem call:

- no G.711 at all (a warning), or codecs other than G.711 (a warning).
  CLEARMODE (`codec_clearmode`'s) is listed apart, for ISDN data calls;
- both laws allowed (a warning: a call can be converted between them,
  which costs V.90);
- DTMF detected in the audio (`inband`, or a mode starting with `auto`:
  a warning);
- fax detection on (a warning: the modem's 2100 Hz answer tone sounds
  like a fax machine's);
- direct media on, and T.38 on (notes).

It also shows whether app_dmodem is loaded (`module show like
app_dmodem`), or whether dmodem-ari is connected (its Stasis app in `ari
show apps`).

### Generated files

This part shows what Apply Config writes.

## What Apply Config writes

Apply Config writes `/etc/asterisk/dmodem.conf` (with FreePBX's header
first). Don't edit that file by hand: the next reload writes it again.
It also writes the `app-dmodem` context into
`extensions_additional.conf`:

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

With dmodem-ari as the engine, `DModem(bbs)` is `Stasis(dmodem,bbs)` and
the fax step is `Stasis(dmodem,receivefax,<file>)`. The fax file is named
as `DModem()` names the faxes it receives by itself, except that an empty
caller ID leaves that part empty (where `DModem()` writes `unknown`).

app_dmodem reads `dmodem.conf` on each call, so a change applies from the
next call. dmodem-ari reads it when it starts, so restart it after Apply
Config.

Uninstalling the module empties its stored settings and profiles;
`dmodem.conf` is left for the next reload to drop.

## fwconsole dmodem

```
fwconsole dmodem dial <profile> <number> [--callerid='"BBS" <5550100>']
fwconsole dmodem profiles
fwconsole dmodem export [file]          # the settings and profiles, as JSON
fwconsole dmodem import <file> [--keep] # back in; --keep keeps the profiles
                                        # that the file doesn't have
```

- `dial` is the Dial out page for scripts (cron, a BBS's mailer).
- `export` without a file prints the JSON. An export holds the ARI
  password, so a file it writes is readable by its owner alone (mode
  0600).
- `import` checks the settings and each profile as the forms do (see
  [Backup and restore](#backup-and-restore)). After an import, run
  `fwconsole reload` to write the files.

## Backup and restore

The module takes part in FreePBX's Backup & Restore (`Backup.php`,
`Restore.php`). A backup that includes D-Modem keeps its settings and
profiles: the same data as `fwconsole dmodem export`.

A restore (like `fwconsole dmodem import` without `--keep`) checks the
data as the forms do:

- The settings are checked together. If any of them fails, none is
  restored, and the failing ones are named in the restore's log.
- Each profile is checked on its own. One that fails (for example a
  profile named `general`) is left out and named in the log; the others
  are restored.
- Profiles that are not in the backup are deleted.

`dmodem.conf` and the dialplan are made again on the reload after the
restore. Received faxes are files in the fax directory, outside the
module's data, so a backup of the module does not include them.

## The calls' path

V.90 needs the call's G.711 untouched from the network to the modem
([asterisk.md](asterisk-details.md#the-channel)). In FreePBX:

- In Settings > Asterisk SIP Settings, and on the trunk and the modem
  extension, allow one law only: the one your provider uses (ulaw in
  North America and Japan, alaw elsewhere). Your provider must not
  transcode either.
- On those endpoints: DTMF RFC 4733, Fax Detect off, and T.38 off where
  no fax is meant to use it.
- On the modem's inbound route: Detect Faxes no, no call recording, and
  no music on hold or announcement before the answer.

The line check shows what is still wrong.

## The module's code

- `module.xml`: the module's name, version (16.0.2), menu item, console
  command and FreePBX versions.
- `Dmodem.class.php`: the BMO (FreePBX's module object) class. It has the
  forms (`doConfigPageInit()`), the pages, `genConfig()` and
  `writeConfig()` for `dmodem.conf`, the dialplan hook
  (`doDialplanHook()`), the destinations, the dial-out (`dialOut()`), the
  export and import (`exportData()`, `importData()`) and the line check
  (`check()`). The settings and profiles live in the module's kvstore.
- `lib/Config.php`: the settings' and profiles' defaults and checks, and
  `dmodem.conf` made from them. A `;` in a value is written `\;`, as
  Asterisk and dmodem-ari read it, and no line breaks get through.
- `lib/Dialplan.php`: the `app-dmodem` context.
- `lib/Apps.php`: the dialplan step class (`ext_dmodem_app`) for
  `DModem()`, `Stasis()` and `DModemReceiveFax()`, which FreePBX's
  extensions class has no classes of its own for.
- `lib/LineCheck.php`: reads `pjsip show endpoints`, `pjsip show endpoint
  <name>`, `module show` and `ari show apps`, and works out what an
  endpoint's settings mean for a modem.
- `lib/Dialout.php`: a call out's Originate action, and the checks of its
  number and caller ID (nothing that would break the manager's lines).
- `lib/Cli.php`: `fwconsole dmodem`'s subcommands
  (`Console/Dmodem.class.php` hands them over, so they work without
  Symfony).
- `Backup.php`, `Restore.php`: Backup & Restore (through
  `Dmodem::exportData()` and `importData()`).
- `functions.inc.php`: FreePBX's destination hooks.
- `page.dmodem.php`, `views/`, `assets/js/dmodem.js`: the pages.

## Tests

### unit.php

`freepbx/test/unit.php` (ctest `freepbx_unit`, label `quick`, built when
PHP is installed) tests the module without FreePBX. It runs on stand-ins
for what the module uses (`fpbx_env.php`):

- the kvstore, with DB_Helper's rules (storing false deletes a key, and
  values come back as strings);
- `extensions::add()` and its output, as FreePBX's
  `extensions.class.php` has them;
- the manager, with canned CLI output;
- the destination usage.

It checks:

- the forms, with good and bad values;
- `dmodem.conf`, read back as dmodem-ari reads it;
- the dialplan for both engines;
- the destinations;
- the line check, on Asterisk 20's own CLI output (`test/fixtures/`);
- the dial-out (its Originate, and refused numbers and caller IDs);
- `fwconsole dmodem`;
- an export imported elsewhere;
- the module's own Backup and Restore classes, on stand-ins of the backup
  module's base classes (`backup_env.php`);
- the pages.

`FREEPBX_FRAMEWORK=<a checkout of FreePBX's framework>` runs the dialplan
through FreePBX's own `extensions.class.php` instead of the stand-in.

### e2e.sh

`freepbx/test/e2e.sh` runs the module's output in a private Asterisk
(`ARI=1`: with dmodem-ari as the engine):

1. `gen.php` makes `dmodem.conf` and the `app-dmodem` context the way the
   page does (its forms posted).
2. An inbound route sends 5551000 to `app-dmodem,bbs,1`. dmodemd calls it
   over SIP and uses the BBS; the test checks that the call went through
   `app-dmodem,bbs,1`. (Two D-Modems on a clean G.711 path connect with
   V.91, at 62666/62666 bit/s.)
3. A call out: `dial.php` (as `fwconsole dmodem dial`) has the manager
   originate it through the dial-out context (`modem-out` here, the
   test's stand-in for `from-internal`) to dmodemd listening, with
   `DModem(term,o)` calling. The test checks that `Local/5551000@modem-out`
   was used.
4. The line check runs on the live endpoints through the manager
   (`linecheck.php`, `ami.php`). The test's endpoint allows both laws,
   which the check must flag as a warning.
