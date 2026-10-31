# FreePBX: the D-Modem module

[FreePBX](https://www.freepbx.org/) is a web interface for running an
Asterisk phone system. D-Modem's FreePBX module (for FreePBX 16 and 17)
lets you set up D-Modem from that web interface instead of editing
configuration files. It adds **Applications > D-Modem** to the menu and a
`fwconsole dmodem` command.

It works with either [dmodem-ari](ari.md) (the default, and the one you
can share) or [app_dmodem](asterisk.md) on an Asterisk you built
yourself.

For the generated files, the code and the tests, see
[the technical details](freepbx-details.md).

## Installing

1. Install [dmodem-ari](ari.md) beside Asterisk, and add an ARI user in
   **Settings > Asterisk REST Interface Users**.
2. Build the module package:

       cmake --build build --target freepbx_module

   This makes `build/freepbx/dmodem-<version>.tgz`.
3. Upload it in **Admin > Module Admin**. FreePBX will warn that it is
   unsigned; that is expected.

## Using it

- **Profiles.** A profile says where a modem call's data goes: a BBS
  server, a program such as `pppd`, or a login. Each profile appears as
  a destination ("D-Modem") you can pick in inbound routes, IVRs, time
  conditions and so on.
- **Fax.** The fax destination (on unless you turn it off in the
  settings) has D-Modem answer as a fax machine and save faxes to a
  folder.
- **Dial out.** Make D-Modem call a remote modem (a BBS or a provider)
  with a profile, from the page or from a script:

      fwconsole dmodem dial <profile> <number>

- **Line check.** Shows the settings on your trunks and extensions that
  would spoil a modem call (see below).
- **Backup.** D-Modem's settings and profiles are included in FreePBX's
  Backup & Restore. `fwconsole dmodem export` and `import` do the same
  from the command line.

After changing anything, click **Apply Config**. If you use dmodem-ari,
restart it afterwards so it reads the new settings.

## Getting good modem calls

Fast modem calls (V.90 especially) need the call's audio to reach the
modem untouched. In FreePBX:

- On the trunk and the modem's extension, allow **one** codec only:
  `ulaw` in North America and Japan, `alaw` elsewhere. Your provider must
  not convert it either.
- Set DTMF to RFC 4733, and turn Fax Detect off.
- On the modem's inbound route: no fax detection, no call recording, no
  music or announcement before the call is answered.

The line check page shows what is still set wrongly.
