# frontend/

This folder has the programs and modules that use the modem (libdmodem: `core/`, `dsp/`
and `link/`). Each one builds into `build/<its name>/`, for example
`build/dmodemd/dmodemd` and `build/asterisk/app_dmodem.so`.

| Directory | What it is |
|-----------|------------|
| `dmodemd/` | The modem as a program: SIP calls, softmodem hardware, USB modems and ISDN. Also the public header of libdmodem (`dmodem.h`) and `libdmodem_sip.a`. |
| `asterisk/` | `app_dmodem` and `codec_clearmode`, modules for Asterisk |
| `ari/` | `dmodem-ari`, the modem for Asterisk as an ARI application |
| `freepbx/` | The FreePBX module (PHP) for `app_dmodem` or `dmodem-ari` |
| `freeswitch/` | `mod_dmodem`, the module for FreeSWITCH |
| `capi/` | `dmodem-capid`, a CAPI 2.0 controller on a D-Modem |
| `pbx/` | `dmodem_bridge.c`, the modem call that the PBX modules share |

The support for only one operating system is in [platforms/](../platforms/README.md).
