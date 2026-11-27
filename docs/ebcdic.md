# EBCDIC terminals and hosts

## What it does

Most terminals and computers send ASCII characters to a modem. IBM
mainframes, IBM midrange computers and some IBM terminals use EBCDIC.
EBCDIC has different codes for the same characters. For example, the
letter `A` is 0x41 in ASCII and 0xC1 in EBCDIC.

D-Modem accepts AT commands in ASCII and in EBCDIC. It identifies the
character set automatically from the `AT` at the start of each command
line. Then it sends the echo, the information text and the result codes
in the same character set. You do not set a switch on the modem or send
a special command.

## How the modem identifies EBCDIC

The modem examines the first two characters of each command line:

| Characters from the DTE | Character set |
| ----------------------- | ------------- |
| 0xC1 0xE3 (`AT`)        | EBCDIC        |
| 0x81 0xA3 (`at`)        | EBCDIC        |
| 0xC1 0x61 (`A/`)        | EBCDIC        |
| 0x41 0x54 (`AT`)        | ASCII         |
| 0x61 0x74 (`at`)        | ASCII         |
| 0x41 0x2F (`A/`)        | ASCII         |

These EBCDIC codes are the same in each code page that D-Modem knows.

An ASCII DTE that sends a parity bit can set bit 7 of each character.
Thus ASCII `A` can be 0x41 or 0xC1, and ASCII `T` can be 0x54 or 0xD4.
The modem ignores bit 7 when it examines ASCII. No ASCII pair, with or
without a parity bit, is the same as an EBCDIC pair. Thus the modem
does not identify ASCII as EBCDIC.

A modem with autobaud uses the same characters to find the bit rate and
the parity of the DTE. D-Modem uses them to find the character set.

The modem keeps the character set of the last command line. It uses
that character set for these items:

- The echo and the result of each command line.
- The unsolicited result codes, for example `RING`, `CONNECT` and
  `NO CARRIER`.
- The escape character (S2). In EBCDIC, the escape sequence `+++` is
  0x4E 0x4E 0x4E.

The modem keeps the values of the S-registers in ASCII. For example,
S3 is 13 (carriage return) in the two character sets, and S2 is 43
(`+`). The modem changes S2 to EBCDIC when it examines the data from an
EBCDIC DTE.

## The +DEBC command

`AT+DEBC=<mode>,<data>,<cp>` sets the EBCDIC function.

| Value       | Meaning                                                   |
| ----------- | --------------------------------------------------------- |
| `<mode>` 0  | ASCII only. The modem ignores a command line in EBCDIC.   |
| `<mode>` 1  | Each command line identifies the character set (default). |
| `<mode>` 2  | EBCDIC only. The modem ignores a command line in ASCII.   |
| `<data>` 0  | The modem does not change the data of a call (default).   |
| `<data>` 1  | The modem changes the data of a call (refer to the next section). |
| `<cp>`      | The IBM code page. The default is 37 (USA and Canada).    |

The modem sends the result of the command line that sets `+DEBC` in the
character set of that command line. The subsequent lines use the new
setting. `ATZ` sets `+DEBC=1,0,37`. `AT&V` shows the values.

`AT+DEBC?` shows the values, for example `+DEBC: 1,0,37`.
`AT+DEBC=?` shows the permitted values.

### The data of a call

With `<data>` 0, the modem sends the data of a call without a change.
Use this setting when the two computers use the same character set, or
when the computers send files and not text.

With `<data>` 1 and an EBCDIC DTE, the modem changes the data:

- The modem changes the EBCDIC from the DTE to ISO 8859-1 before it
  sends the data to the remote modem.
- The modem changes the ISO 8859-1 from the remote modem to EBCDIC
  before it sends the data to the DTE.

Use this setting when an EBCDIC terminal calls an ASCII host, for
example a bulletin board system. Do not use it for file transfers,
because it changes binary data.

### Code pages

D-Modem knows these IBM code pages. Each code page changes each of the
256 EBCDIC codes to one ISO 8859-1 code, and back.

| `<cp>` | Country or region                     |
| ------ | ------------------------------------- |
| 37     | USA, Canada                           |
| 273    | Germany, Austria                      |
| 275    | Brazil                                |
| 277    | Denmark, Norway                       |
| 278    | Finland, Sweden                       |
| 280    | Italy                                 |
| 284    | Spain, Latin America                  |
| 285    | United Kingdom                        |
| 297    | France                                |
| 500    | International                         |
| 871    | Iceland                               |
| 1047   | Latin 1 (z/OS UNIX System Services)   |

Code pages 37 and 1047 are almost the same. They are different for six
characters: `[`, `]`, `^`, `¬`, `Ý` and `¨`. If the brackets of the
terminal are not correct, try the other code page.

## Examples

An EBCDIC terminal sends `ATI7` and a carriage return:

```
DTE:   C1 E3 C9 F7 0D
Modem: C1 E3 C9 F7 0D 0D 25 C3 96 A4 95 A3 99 A8 7A 40 E4 E2 C1 0D 25
       D6 D2 0D 25
```

The modem sends `Country: USA` and `OK` in EBCDIC. The line feed (S4,
10) is 0x25 in code page 37.

An ASCII terminal on the same modem sends `AT` and a carriage return.
The modem identifies ASCII and sends `OK` in ASCII.

## Limits

- The modem does not change the data of fax and voice calls. These
  calls send binary data, and the `<DLE>` codes of voice and fax are
  binary codes.
- The modem uses one octet for each character. It does not use the
  shift codes (SO and SI) of the double-byte code pages, for example
  for Japanese.
- The code pages with the euro sign (1140 to 1149) are not available.
  Their euro sign is not in ISO 8859-1.

## Where the code is

- `core/ebcdic.h`: the code pages and the identification of the
  character set.
- `core/ebcdic_tables.h`: the tables. `tools/gen-ebcdic-tables.py`
  writes this file from the code pages of go3270. These code pages come
  from the ICU data of the Unicode Consortium.
- `core/at.cpp`: `Modem::at_process()` identifies the character set of
  each command line. `+DEBC` is in the `kept_params` table.
- `core/modem.cpp`: `Modem::put_text()` sends the text of the modem to
  the DTE. `Modem::check_escape()` examines the escape sequence.
  `LinkOwner::write_chars()` and `Modem::link_chars()` change the data
  of a call.
- `core/test/core_test.cpp`: the tests `Core.EbcdicCodePages`,
  `Core.EbcdicCommands` and `Core.EbcdicCall`.
