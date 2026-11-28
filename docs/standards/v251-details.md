# V.251: technical details

This page gives the technical details for [V.251](v251.md). The clause
numbers are the clause numbers of V.251 (02/98). This page is our own
description, not a copy of the Recommendation.

## +A8E (5.1)

`AT+A8E=<v8o>,<v8a>,<v8cf>[,<v8b>][,<cfrange>][,<protrange>]`

| Parameter | Value | Function |
|---|---|---|
| `<v8o>` (call) | 0 | No V.8 |
| | 1 | The modem does V.8 |
| | 2 to 5 | The computer controls V.8. The modem sends CI, CNG, CT or no tone |
| | 6 | The modem does V.8, and sends the `+A8` indications |
| `<v8a>` (answer) | 0 | No V.8 |
| | 1 | The modem does V.8 |
| | 2, 3 | The computer controls V.8. The modem sends ANSam, or no signal |
| | 4 | No V.8. The modem sends ANS |
| | 5 | The modem does V.8, and sends the `+A8` indications |
| `<v8cf>` | Hexadecimal | The call function of CI |
| `<v8b>` | 0, 1, 2 | No V.8 bis, V.8 bis by the modem, V.8 bis by the computer |
| `<cfrange>`, `<protrange>` | A string | Other call functions and protocols that the answerer accepts |

- The defaults are `1,1,,1,"",""`.
- When the modem is off hook, `AT+A8E` starts V.8 or V.8 bis again.
- A modem must have one or more of `<v8o>` 2 to 6, `<v8a>` 2 to 5, or
  `<v8b>` 2.

## The hexadecimal octets (4.1)

Each octet is two hexadecimal characters. Bit 0, the first bit on the
line, is the least significant bit. Thus, the octet 00011011 in time
order is `D8`.

## The indications (clause 6)

| Indication | From | Meaning |
|---|---|---|
| `+A8I:<cf>` | The answerer | CI received, with its call function. `+A8I:0`: no CI |
| `+A8C:<type>` | The answerer | A calling tone: 0 none, 1 CNG, 2 the 1300 Hz tone of V.25, 3 the AA of V.32 |
| `+A8A:<type>` | The caller | An answer signal: 0 none, 1 ANSam, 2 ANS, 3 ANS with phase reversals, 4 to 7 the start of V.21, V.22 bis, V.32 bis, V.34 |
| `+A8M:<octets>` | Either | The CM or the JM that the modem received. `+A8M:0`: none |
| `+A8J:<value>` | Either | 1: CJ sent or received. 0: no CJ |
| `+A8R:<signal>,<msg>...` | Either | A V.8 bis signal and its messages |

## The commands of DTE control (5.2, 5.3)

| Command | Function |
|---|---|
| `AT+A8M=<octets>` | Send this CM or JM |
| `AT+A8T=<signal>,<msg1>,<msg2>,<sig_en>,<msg_en>,<supp_delay>` | Send a V.8 bis signal (Table 3) and its messages. Then listen for signals and messages |

## The procedures (clauses 7 to 9)

1. **Call (7).** After the dial, the modem sends CI, CNG or CT. When it
   detects ANSam, it gives `+A8A:1`. Then the computer sends CM with
   `AT+A8M`. The modem gives the JM in `+A8M:`. `ATO` makes the modem
   send CJ, with `+A8J:1`.
2. **Answer (8).** The modem gives `+A8I` for CI and `+A8M:` for CM.
   The computer sends JM with `AT+A8M`. When CJ arrives, the modem gives
   `+A8J:1`.
3. **V.8 bis (9).** With `<v8b>` 2, the modem reports each V.8 bis
   signal and message with `+A8R`. The computer sends them with
   `AT+A8T`. `AT+A8E=,,,0` stops it.
4. With `<v8o>` 6 or `<v8a>` 5, the modem gives the same indications,
   but it sends CM, JM and CJ itself, with no `OK` between them.

## V.251 in D-Modem

| Item | D-Modem (`core/at.cpp`, `core/modem.cpp`) |
|---|---|
| `<v8o>` | 1, or 6 with the indications |
| `<v8a>` | 1, or 5 with the indications |
| `<v8cf>` | 0 only. The modem sets the call function from `+FCLASS` |
| `<v8b>` | 0 or 1. No DTE control of V.8 bis (`+A8T`, `+A8R`) |
| `<cfrange>`, `<protrange>` | Not implemented |
| `<v8b>` default | 0, not the 1 of V.251. An answer with V.8 bis makes a caller without it wait about 3.5 s |
| The time of the indications | All together after V.8, before CONNECT and the reports of V.250 |
| The indications that we send | The caller: `+A8A:1`, `+A8M:<JM>`, `+A8J:1`. The answerer: `+A8M:<CM>`, `+A8J:1`. None if V.8 did not complete |
| `+A8I`, `+A8C`, `+A8A` other than 1 | Not sent |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| The values that a modem must have (5.1) | Only `<v8o>` 1 and `<v8a>` 1 | Also `<v8o>` 6 and `<v8a>` 5, with the `+A8` indications |
