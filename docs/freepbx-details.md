# FreePBX: the D-Modem module: technical details

This page gives the technical details for [FreePBX: the D-Modem module](freepbx.md), the plain-English overview. Read the overview first. This page keeps the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

`frontend/freepbx/dmodem/` is a FreePBX module (version 16.0.2, for FreePBX 16
and 17) for [app_dmodem](asterisk.md) or [dmodem-ari](ari.md). It
gives these items:

- the profiles of the modem, which you edit in the web interface of FreePBX. Each profile is a
  destination for inbound routes, IVRs, time conditions and similar items;
- calls out to a remote modem with a profile;
- the settings and profiles in the Backup & Restore of FreePBX;
- a check of the PJSIP trunks and extensions for modem calls.

It adds **Applications > D-Modem** to the menu and the `fwconsole dmodem`
command. The module is only PHP. Nothing in it is compiled.

## Installing

1. **The modem.** Select one engine:
   - **dmodem-ari** together with Asterisk (`build/ari/dmodem-ari`), with an ARI
     (Asterisk REST Interface) user in Settings > Asterisk REST
     Interface Users. This is the default engine.
   - **app_dmodem**, on an Asterisk for which you build it yourself. With
     the headers of Asterisk installed, run
     `cmake -B build -DDMODEM_APP_DMODEM=ON && cmake --build build`. Copy
     `app_dmodem.so` to the modules directory of Asterisk, then run
     `asterisk -rx "module load app_dmodem.so"`. Do not give a built app_dmodem
     to other persons ([LICENSING.md](../LICENSING.md)).
2. **The module.** `cmake --build build --target freepbx_module` makes
   `build/freepbx/dmodem-<version>.tgz`. Upload it in Admin > Module
   Admin (Upload modules). Or, unpack it into the modules directory of FreePBX
   and install it from the command line:

   ```
   tar xzf dmodem-16.0.2.tgz -C /var/www/html/admin/modules/
   fwconsole ma installlocal dmodem
   fwconsole reload
   ```

   The module is not signed. Thus, FreePBX shows it as unsigned.

## Using it

The page of the module has five parts.

### Profiles

A profile tells where the data of a call goes after the modems connect:

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
| Hand over the line (`handoff`) | off | Unix-socket servers with call details first only: the server gets the modem's line itself (a terminal) instead of a copy of its bytes. See [the handoff](asterisk-details.md). |
| Description | empty | One line, 100 characters at most; shown with the destination. |

A profile name has 1 to 40 characters (lower-case letters, digits, `-`
and `_`). The first character is a letter or digit. The names `general`,
`receivefax` and `sendfax` are reserved.

A profile `bbs` is the destination `app-dmodem,bbs,1`. Each destination picker shows it under
"D-Modem". After you save a profile, you cannot change its name, because the name is its
destination. You cannot delete a profile if a route or
a different object still refers to it.

### Settings

- **Engine**: the `Stasis()` of dmodem-ari (the default), or `DModem()` in an
  Asterisk that has app_dmodem built for it.
- **Log level**: the `debug` of the modem, 0 to 3.
- **dmodem-ari** (engine `ari` only): the ARI URL
  (`http://127.0.0.1:8088` by default), user (`dmodem`), password
  (empty on the form; the module keeps the stored password) and Stasis app name
  (`dmodem`). Optionally, T.38 through the SIP side of dmodem-ari, with
  its PJSIP endpoint (`dmodem`) and SIP address (`127.0.0.1:5090`).
- **Dialling out**: the dial-out context (`from-internal`), the caller ID,
  and the ring time (60 s; 5 to 300 s).
- **Fax**, the fax settings of `[general]`:
  - the directory for received faxes (`/var/spool/asterisk/fax`);
  - our fax number (up to 20 digits, `+` and spaces);
  - the fastest rate (14400, 12000, 9600, 7200, 4800 or 2400 bit/s);
  - error correction mode (on) and T.38 (on);
  - **Fax destination** (on): a destination "D-Modem: receive a fax"
    (`app-dmodem,receivefax,1`) that answers as a fax machine.

### Dial out

A call to a remote modem (a BBS, an ISP) with a profile:

1. The module dials the number (digits, `*`, `#` and `+`, 40 at most) in the
   dial-out context. That is `from-internal` by default. Thus, your outbound
   routes select the trunk, as for the call of an extension.
2. The call uses the caller ID of the settings (or the given caller ID) and the ring
   time.
3. When the far end answers, D-Modem is the calling modem:
   `DModem(<profile>,o)`, or `Stasis(<app>,<profile>,o)` with dmodem-ari.
   Then the server or program of the profile communicates with the remote end.

The module asks the manager of Asterisk (AMI) to originate the call, with the
channel `Local/<number>@<context>` and the channel variable
`DMODEM_DIALOUT=<profile>`.

### Line check

The line check reads each PJSIP endpoint (up to 200), as Asterisk has it
now, through the manager. It reports the items that are important for a modem call:

- no G.711 (a warning), or codecs other than G.711 (a warning).
  The check shows CLEARMODE (of `codec_clearmode`) separately, for ISDN data calls;
- the two laws permitted (a warning: a call can be converted between them,
  and then V.90 is not possible);
- DTMF detected in the audio (`inband`, or a mode that starts with `auto`:
  a warning);
- fax detection enabled (a warning: the 2100 Hz answer tone of the modem sounds
  like the tone of a fax machine);
- direct media enabled, and T.38 enabled (notes).

It also shows if app_dmodem is loaded (`module show like
app_dmodem`), or if dmodem-ari is connected (its Stasis app in `ari
show apps`).

### Generated files

This part shows the files that Apply Config writes.

## What Apply Config writes

Apply Config writes `/etc/asterisk/dmodem.conf` (with the header of FreePBX
first). Do not edit that file manually, because the next reload writes it again.
Apply Config also writes the `app-dmodem` context into
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

With dmodem-ari as the engine, `DModem(bbs)` is `Stasis(dmodem,bbs)`, and
the fax step is `Stasis(dmodem,receivefax,<file>)`. The name of the fax file is
the same as the name that `DModem()` gives to the faxes that it receives automatically. One
difference: if the caller ID is empty, that part of the name is empty (`DModem()` writes `unknown`).

app_dmodem reads `dmodem.conf` on each call. Thus, a change applies from the
next call. dmodem-ari reads it when it starts. Thus, restart dmodem-ari after Apply
Config.

When you uninstall the module, it deletes its stored settings and profiles.
It does not delete `dmodem.conf`. The next reload removes it.

## fwconsole dmodem

```
fwconsole dmodem dial <profile> <number> [--callerid='"BBS" <5550100>']
fwconsole dmodem profiles
fwconsole dmodem export [file]          # the settings and profiles, as JSON
fwconsole dmodem import <file> [--keep] # back in; --keep keeps the profiles
                                        # that the file doesn't have
```

- `dial` is the Dial out page for scripts (cron, the mailer of a BBS).
- `export` without a file shows the JSON. An export holds the ARI
  password. Thus, only its owner can read a file that it writes (mode
  0600).
- `import` checks the settings and each profile as the forms do (refer to
  [Backup and restore](#backup-and-restore)). After an import, run
  `fwconsole reload` to write the files.

## Backup and restore

The module is a part of the Backup & Restore of FreePBX (`Backup.php`,
`Restore.php`). A backup that includes D-Modem keeps its settings and
profiles: the same data as `fwconsole dmodem export`.

A restore (as `fwconsole dmodem import` without `--keep`) checks the
data as the forms do:

- The settings are checked together. If one of them fails, the restore does not restore any
  setting. The log of the restore gives the names of the settings that failed.
- Each profile is checked separately. If a profile fails (for example a
  profile with the name `general`), the restore does not restore it, and gives its name in the log.
  The restore restores the other profiles.
- The restore deletes the profiles that are not in the backup.

The reload after the restore makes `dmodem.conf` and the dialplan again.
Received faxes are files in the fax directory, outside the
data of the module. Thus, a backup of the module does not include them.

## The calls' path

V.90 must have the G.711 of the call without changes from the network to the modem
([asterisk.md](asterisk-details.md#the-channel)). In FreePBX:

- In Settings > Asterisk SIP Settings, and on the trunk and the modem
  extension, permit only one law: the law that your provider uses (ulaw in
  North America and Japan, alaw in other countries). Also, your provider must not
  transcode.
- On those endpoints: DTMF RFC 4733, Fax Detect disabled, and T.38 disabled where
  no fax must use it.
- On the inbound route of the modem: Detect Faxes no, no call recording, and
  no music on hold or announcement before the answer.

The line check shows the items that are still incorrect.

## The module's code

- `module.xml`: the name, version (16.0.2), menu item, console
  command and FreePBX versions of the module.
- `Dmodem.class.php`: the BMO (the module object of FreePBX) class. It has the
  forms (`doConfigPageInit()`), the pages, `genConfig()` and
  `writeConfig()` for `dmodem.conf`, the dialplan hook
  (`doDialplanHook()`), the destinations, the dial-out (`dialOut()`), the
  export and import (`exportData()`, `importData()`) and the line check
  (`check()`). The settings and profiles are in the kvstore of the module.
- `lib/Config.php`: the defaults and checks of the settings and profiles, and
  `dmodem.conf` made from them. A `;` in a value is written `\;`, as
  Asterisk and dmodem-ari read it. No line breaks go through.
- `lib/Dialplan.php`: the `app-dmodem` context.
- `lib/Apps.php`: the dialplan step class (`ext_dmodem_app`) for
  `DModem()`, `Stasis()` and `DModemReceiveFax()`. The
  extensions class of FreePBX has no classes for these.
- `lib/LineCheck.php`: reads `pjsip show endpoints`, `pjsip show endpoint
  <name>`, `module show` and `ari show apps`, and finds the meaning of the
  settings of an endpoint for a modem.
- `lib/Dialout.php`: the Originate action of a call out, and the checks of its
  number and caller ID (nothing that would break the lines of the manager).
- `lib/Cli.php`: the subcommands of `fwconsole dmodem`
  (`Console/Dmodem.class.php` gives them to it, thus they operate without
  Symfony).
- `Backup.php`, `Restore.php`: Backup & Restore (through
  `Dmodem::exportData()` and `importData()`).
- `functions.inc.php`: the destination hooks of FreePBX.
- `page.dmodem.php`, `views/`, `assets/js/dmodem.js`: the pages.

## Tests

### unit.php

`frontend/freepbx/test/unit.php` (ctest `freepbx_unit`, label `quick`, built when
PHP is installed) tests the module without FreePBX. It runs on stand-ins
for the items that the module uses (`fpbx_env.php`):

- the kvstore, with the rules of DB_Helper (when you store false, the key is deleted, and
  values come back as strings);
- `extensions::add()` and its output, as the
  `extensions.class.php` of FreePBX has them;
- the manager, with canned CLI output;
- the destination usage.

It checks:

- the forms, with good and bad values;
- `dmodem.conf`, read back as dmodem-ari reads it;
- the dialplan for both engines;
- the destinations;
- the line check, on the CLI output of Asterisk 20 (`test/fixtures/`);
- the dial-out (its Originate, and refused numbers and caller IDs);
- `fwconsole dmodem`;
- an export imported elsewhere;
- the Backup and Restore classes of the module, on stand-ins of the base classes of the backup
  module (`backup_env.php`);
- the pages.

`FREEPBX_FRAMEWORK=<a checkout of FreePBX's framework>` runs the dialplan
through the `extensions.class.php` of FreePBX instead of the stand-in.

### e2e.sh

`frontend/freepbx/test/e2e.sh` runs the output of the module in a private Asterisk
(`ARI=1`: with dmodem-ari as the engine):

1. `gen.php` makes `dmodem.conf` and the `app-dmodem` context in the same method as the
   page (with its forms posted).
2. An inbound route sends 5551000 to `app-dmodem,bbs,1`. dmodemd calls it
   over SIP and uses the BBS. The test makes sure that the call went through
   `app-dmodem,bbs,1`. (Two D-Modems on a clean G.711 path connect with
   V.91, at 62666/62666 bit/s.)
3. A call out: `dial.php` (as `fwconsole dmodem dial`) tells the manager to
   originate it through the dial-out context (here `modem-out`, the
   stand-in of the test for `from-internal`) to a dmodemd that listens. `DModem(term,o)`
   calls. The test makes sure that the call used `Local/5551000@modem-out`.
4. The line check runs on the live endpoints through the manager
   (`linecheck.php`, `ami.php`). The endpoint of the test permits the two laws.
   The check must show this as a warning.
