# The PBX modules and the login menu
## Asterisk module

`frontend/asterisk/` contains `app_dmodem` and the fax applications `DModemSendFax` and `DModemReceiveFax`:

- `app_dmodem` is a dialplan application. It answers a call on an Asterisk channel with the open modem. Then it sends the connection to a server (TCP or a unix socket) or to a program on a pty, for example pppd. Each profile in `dmodem.conf` sets this.
- `DModemSendFax` and `DModemReceiveFax` are a group 3 fax machine on the audio or through T.38 (PJSIP).

The module uses `libdmodem` (`frontend/dmodemd/dmodem.h`, and `frontend/dmodemd/dmodem.hpp` with C++ classes for it). `libdmodem` is the modem core and the open DSP as a library.  This puts AGPL code in a GPLv2 Asterisk module. Thus, the module is disabled by default. Build it only for your own Asterisk. Do not distribute it ([LICENSING.md](../LICENSING.md)).  To distribute D-Modem for Asterisk, use dmodem-ari (below).  Refer to [docs/asterisk.md](asterisk.md).

`frontend/ari/` contains `dmodem-ari`. It does the same functions as a separate program next to Asterisk. It is an application of the Asterisk REST Interface (ARI). The dialplan `Stasis(dmodem,<profile>)` sends a call to it. `Stasis(dmodem,sendfax,<file>)` or `Stasis(dmodem,receivefax,<file>)` sends a fax to it. dmodem-ari gets the audio through RTP from an external media channel, or from its own SIP leg. Its own SIP leg can change to T.38 (`t38 = yes`).  Asterisk does not load parts of D-Modem.  dmodem-ari is a prototype. Refer to [docs/ari.md](ari.md).

## FreePBX module

`frontend/freepbx/` contains a FreePBX module (16 and 17) for `app_dmodem` or `dmodem-ari`. It has these functions:

- You edit the profiles in the web interface of FreePBX. Each profile is a destination for inbound routes and IVRs.
- The module writes `dmodem.conf` and the dialplan when you do Apply Config.
- A fax destination.
- Calls to a remote modem with a profile (from the page or with `fwconsole dmodem dial`).
- The settings are in the Backup & Restore of FreePBX.
- A check of the PJSIP trunks and extensions for items that cause problems for modem calls. These are other codecs, the two G.711 laws, and DTMF or fax detection in the audio.

`cmake --build build --target freepbx_module` makes the package of the module.  Refer to [docs/freepbx.md](freepbx.md).

## FreeSWITCH module

`frontend/freeswitch/` contains `mod_dmodem`. It does the same functions for FreeSWITCH. It has the `dmodem` application and the fax applications `dmodem_send_fax` and `dmodem_receive_fax`. The `dmodem` application connects to a server through TCP or a unix socket, or to a program on a pty. Each profile in `dmodem.conf.xml` sets this. The fax applications operate on the audio or through T.38 (sofia).  Refer to [docs/freeswitch.md](freeswitch.md).

The two modules share `frontend/pbx/dmodem_bridge.c`, which uses the C API of libdmodem. This file contains these items:

- The options of the profile.
- The modem with its DTE on a socketpair.
- The data between the modem and the server of the profile (TCP or a unix socket, with the details of the call first as an option), or the program on a pty.

Each module keeps the side of its PBX: the frames of the channel, its variables and log, and the fax applications.

## Login menu (optional)

[modemmenu](https://github.com/D-Modem/menu) is a login and menu for callers, in Go. First, it compares a user name and password with a text file, LDAP, an SQL database or PAM. Then it gives a BBS, PPP or a shell. It starts them with the credentials of the caller, the dialed number, the connect rate and the time of the call.  Each module can start it as a `mode = exec` profile. Or, the module can connect calls to one `modemmenu -serve` through a `mode = unix` profile with `header = yes`.
