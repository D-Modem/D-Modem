# D-Modem

An open V.92 softmodem: connect to dial-up modems over a SIP call, with
no modem hardware.

D-Modem's data pumps, error correction and AT command set are open
source. `dmodemd` puts the modem on a SIP line and gives the computer a
serial port (a pty, a COM port or a socket) to dial from. It can also
drive real phone-line hardware: an old laptop's softmodem, Smart Link and
Agere USB modems, and the [pico-serial](https://github.com/D-Modem/hardware)
sticks. On a PBX it answers calls through Asterisk, FreeSWITCH or FreePBX.

- V.92, V.90 and V.91 (up to 62667 bit/s between digital modems), V.34 and every older standard down to Bell 103
- Class 1, 2 and 2.0 fax with a built-in fax machine and T.38
- V.42 error correction with V.44, V.42bis and MNP 5 compression; V.80 for H.324
- Text telephones (V.18), alarm reports, fixed-line SMS, card-terminal fast connect, leased lines and ISDN data calls
- Voice and data together (V.70 DSVD, V.61) and V.150.1 modem relay
- Linux, Windows, macOS and Haiku; the [D-Modem app](https://github.com/D-Modem/app) for Android and iOS is built on it

## Quick start

On Linux, with CMake, a C++17 compiler and OpenSSL's headers:

    cmake -B build && cmake --build build
    export SIP_LOGIN=username:password@sip.example.com
    ./build/dmodemd/dmodemd

then open `/dev/ttyDM0` in a terminal program at 115200 bit/s, type
`ATX3` (there is no dial tone on SIP) and dial with `ATDT`.

## Documentation

- [Building](docs/building.md): Linux, Windows, macOS, Haiku, the tests
- [Using dmodemd](docs/usage.md): the SIP account, dialing, answering, fax, voice, SRTP, hardware
- [What the modem does](docs/features.md): every standard, in one place
- [The PBX modules](docs/modules.md): Asterisk, dmodem-ari, FreePBX, FreeSWITCH and modemmenu
- [docs/](docs/README.md): how each part works, with a technical details page beside each
- [The AT command reference](docs/at_commands.pdf) (PDF)
- [Source layout](docs/layout.md), [How it works](docs/how-it-works.md), [Known issues](docs/known-issues.md)

D-Modem began at Aon's Cyber Labs in 2021 ([acknowledgements](docs/acknowledgements.md)).

## The D-Modem suite

| Repository | What it is |
| --- | --- |
| [D-Modem](https://github.com/D-Modem/D-Modem) | The open V.92 softmodem: dmodemd, libdmodem and the Asterisk, FreeSWITCH and ARI modules |
| [app](https://github.com/D-Modem/app) | The D-Modem app for Android and iOS: a dial-up modem on a SIP call |
| [hardware](https://github.com/D-Modem/hardware) | pico-serial: RP2040 serial adapters and phone-line USB sticks |
| [menu](https://github.com/D-Modem/menu) | modemmenu: the login and menu dial-up callers reach |
| [bbs](https://github.com/D-Modem/bbs) | A bulletin board for telnet, SSH, TN3270 and modem callers |
| [go3270](https://github.com/D-Modem/go3270) | A Go library for 3270 terminal servers, used by bbs |
| [brand](https://github.com/D-Modem/brand) | Logos, colours, type and the naming guide |

## License

D-Modem is copyright the D-Modem Authors, under the GNU Affero General Public License version 3 only ([COPYING](COPYING)), or under a commercial licence from the D-Modem Authors.  See [LICENSING.md](LICENSING.md) for the parts under other licences: the Asterisk module (GPL-2.0) and the interop test binaries in `dsp/test/`, which belong to their owners and are **not** subject to this repository's terms ([dsp/test/NOTICE.md](dsp/test/NOTICE.md)).
