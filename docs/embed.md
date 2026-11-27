# dmodemd as a library (libdmodem_embed)

`libdmodem_embed` is `dmodemd` as a library. A program uses it to run the
modem in a thread of its own process. Thus, the program does not start a
separate `dmodemd` process. The D-Modem desktop app
([D-Modem/gui](https://github.com/D-Modem/gui)) uses this library.

For the full technical description, refer to
[embed-details.md](embed-details.md).

## What it does

1. The program calls `dmodem_embed_start()` with the settings of the line.
2. The library starts the modem in a new thread.
3. The library returns a descriptor (a socket). This descriptor is the DTE
   of the modem.
4. The program writes AT commands and data to the descriptor. It reads the
   responses of the modem from the descriptor. This is the same as the
   pty of `dmodemd`.

The library has the same lines as `dmodemd`:

| Line | `dmodemd` option | Condition |
| --- | --- | --- |
| SIP | `SIP_LOGIN`, `-L` | Always |
| USB modem | `--usb` | libusb (Linux: `libusb-1.0-0-dev`) |
| Softmodem codec | `-a` | ALSA (Linux: `libasound2-dev`) |
| ISDN card | `--misdn` | Linux |

## Differences from dmodemd

- There is no pty and no `/dev/ttyDM0` link.
- The library does not need root. The library does not change the user or
  the scheduling priority of the program.
- The library does not read environment variables. The program gives all
  of the settings in `dmodem_embed_config`.
- Each process can run one modem at a time.

## How to use it

Build D-Modem with CMake. The option `DMODEM_EMBED` is on by default. It
builds the static library `dmodem_embed`. To use the library in a CMake
project, add the D-Modem tree with `add_subdirectory()`, and link the
target `dmodem_embed`. The header is `frontend/embed/dmodem_embed.h`. The
API is in C.

An example:

```c
dmodem_embed_config cfg;
dmodem_embed *e = dmodem_embed_new();
int fd;

dmodem_embed_config_init(&cfg);
cfg.sip_user = "alice";
cfg.sip_password = "secret";
cfg.sip_domain = "sip.example.com";
fd = dmodem_embed_start(e, &cfg);
/* write "ATX3DT5551234\r" to fd, read the result codes from fd */
dmodem_embed_free(e);
```

## Licence

The library is under the AGPL-3.0-only, or the commercial licence, as all
of D-Modem. Refer to [LICENSING.md](../LICENSING.md).

## Status

- The SIP line operates. A test makes a call between two programs, each
  with its own embedded modem.
- The USB, ALSA and mISDN lines use the same code as `dmodemd`. The test
  does not use these lines.
- The library builds on Linux. Windows and macOS builds are not tested
  at this time.
