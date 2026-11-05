# The PBX modules and the login menu
## Asterisk module

`frontend/asterisk/` has `app_dmodem`, a dialplan application that answers a call on an Asterisk channel with the open modem and hands the connection to a server (TCP or a unix socket) or a program on a pty such as pppd, per profile in `dmodem.conf`, and `DModemSendFax` and `DModemReceiveFax`, a group 3 fax machine on the audio or over T.38 (PJSIP).  It is built on `libdmodem` (`frontend/dmodemd/dmodem.h`, and `frontend/dmodemd/dmodem.hpp` with C++ classes for it), the modem core and open DSP as a library.  Because that puts AGPL code inside a GPLv2 Asterisk module, it is off by default and only for building on your own Asterisk, never to be distributed ([LICENSING.md](../LICENSING.md)); dmodem-ari, below, is the way to ship D-Modem for Asterisk.  See [docs/asterisk.md](asterisk.md).

`frontend/ari/` has `dmodem-ari`, the same as a program of its own beside Asterisk, an application of the Asterisk REST Interface (ARI): the dialplan's `Stasis(dmodem,<profile>)` hands it a call, and `Stasis(dmodem,sendfax,<file>)` or `Stasis(dmodem,receivefax,<file>)` a fax, whose audio it takes over RTP from an external media channel, or from a SIP leg of its own that can go over to T.38 (`t38 = yes`).  Nothing of D-Modem is loaded into Asterisk.  It is a prototype; see [docs/ari.md](ari.md).

## FreePBX module

`frontend/freepbx/` has a FreePBX module (16 and 17) for `app_dmodem` or `dmodem-ari`: the profiles edited in FreePBX's web interface, each a destination for inbound routes and IVRs, `dmodem.conf` and the dialplan written on Apply Config, a fax destination, calls out to a remote modem with a profile (the page or `fwconsole dmodem dial`), the settings in FreePBX's Backup & Restore, and a check of the PJSIP trunks and extensions for what costs modem calls (other codecs, both G.711 laws, DTMF or fax detection in the audio).  `cmake --build build --target freepbx_module` packs it.  See [docs/freepbx.md](freepbx.md).

## FreeSWITCH module

`frontend/freeswitch/` has `mod_dmodem`, the same for FreeSWITCH: the `dmodem` application (a server over TCP or a unix socket, or a program on a pty, per profile in `dmodem.conf.xml`) and the fax applications `dmodem_send_fax` and `dmodem_receive_fax`, on the audio or over T.38 (sofia).  See [docs/freeswitch.md](freeswitch.md).

The two modules share `frontend/pbx/dmodem_bridge.c` on libdmodem's C API: the profile's options, the modem with its DTE on a socketpair, and the data between it and the profile's server (TCP or a unix socket, optionally with the call's details first) or program on a pty.  Each module keeps its PBX's side (the channel's frames, its variables and log, the fax applications).

## Login menu (optional)

[modemmenu](https://github.com/D-Modem/menu) is a login and menu for callers, in Go: it checks a user name and password against a text file, LDAP, an SQL database or PAM, then offers a BBS, PPP or a shell, started with the caller's credentials, the dialed number, the connect rate and the time of the call.  Either module runs it as a `mode = exec` profile, or connects calls to one `modemmenu -serve` by a `mode = unix` profile with `header = yes`.
