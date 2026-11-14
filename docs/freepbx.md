# FreePBX: the D-Modem module

[FreePBX](https://www.freepbx.org/) is a web interface to operate an
Asterisk telephone system. The FreePBX module of D-Modem (for FreePBX 16
and 17) lets you configure D-Modem from that web interface. You do not
have to edit configuration files. It adds **Applications > D-Modem** to
the menu, and it adds a `fwconsole dmodem` command.

It operates with [dmodem-ari](ari.md) (the default, and the one that you
can share) or with [app_dmodem](asterisk.md) on an Asterisk that you
built.

For the generated files, the code and the tests, refer to
[the technical details](freepbx-details.md).

## Installing

1. Install [dmodem-ari](ari.md) next to Asterisk. Add an ARI user in
   **Settings > Asterisk REST Interface Users**.
2. Build the module package:

       cmake --build build --target freepbx_module

   This makes `build/freepbx/dmodem-<version>.tgz`.
3. Upload it in **Admin > Module Admin**. FreePBX gives a warning that it
   is unsigned. This is correct.

## Using it

- **Profiles.** A profile tells where the data of a modem call goes: a BBS
  server, a program (for example `pppd`), or a login. Each profile is a
  destination ("D-Modem") that you can select in inbound routes, IVRs,
  time conditions and other items.
- **Persons and modems on one number.** Give a profile a **Voice
  extension** (an IVR, a voicemail box or a telephone). If a person calls
  the profile, the call goes to that extension. If a modem calls, the
  modem answers as usual. The modem answers when the extension hangs up,
  or when the modem of the caller asks for data. **Modem voice** sends the
  voice channel of a DSVD or ASVD modem to the same extension while the
  data continues.
- **Fax.** With the fax destination, D-Modem answers as a fax machine and
  saves faxes to a folder. The fax destination is enabled. You can
  disable it in the settings.
- **Dial out.** Make D-Modem call a remote modem (a BBS or a provider)
  with a profile, from the page or from a script:

      fwconsole dmodem dial <profile> <number>

- **Line check.** Shows the settings on your trunks and extensions that
  can cause problems for a modem call (refer to the subsequent section).
- **Backup.** The Backup & Restore of FreePBX includes the settings and
  profiles of D-Modem. `fwconsole dmodem export` and `import` do the same
  from the command line.

After you change an item, click **Apply Config**. If you use dmodem-ari,
restart it after this. It then reads the new settings.

## Getting good modem calls

For fast modem calls (specially V.90), the audio of the call must get to
the modem with no changes. In FreePBX, do these steps:

- On the trunk and the extension of the modem, allow **one** codec only:
  `ulaw` in North America and Japan, `alaw` in other countries. Your
  provider must not convert it.
- Set DTMF to RFC 4733, and disable Fax Detect.
- On the inbound route of the modem, do not use fax detection, call
  recording, or music or an announcement before the modem answers the
  call.

The line check page shows the settings that are still incorrect.
