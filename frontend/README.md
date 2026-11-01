# frontend/

The programs and modules built on the modem (libdmodem: `core/`, `dsp/`
and `link/`).  Each one builds into `build/<its name>/`, for example
`build/dmodemd/dmodemd` and `build/asterisk/app_dmodem.so`.

| Directory | What it is |
|-----------|------------|
| `dmodemd/` | The modem as a program: SIP calls, softmodem hardware, USB modems and ISDN; also libdmodem's public header (`dmodem.h`) and `libdmodem_sip.a` |
| `asterisk/` | `app_dmodem` and `codec_clearmode`, modules for Asterisk |
| `ari/` | `dmodem-ari`, the modem for Asterisk as an ARI application |
| `freepbx/` | The FreePBX module (PHP) for `app_dmodem` or `dmodem-ari` |
| `freeswitch/` | `mod_dmodem`, the module for FreeSWITCH |
| `capi/` | `dmodem-capid`, a CAPI 2.0 controller on a D-Modem |
| `pbx/` | `dmodem_bridge.c`, the modem call the PBX modules share |

Support for one operating system alone is in [platforms/](../platforms/README.md).
