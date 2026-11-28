# V.253: technical details

This page gives the technical details for [V.253](v253.md). The clause
numbers are the clause numbers of V.253 (02/98). This page is our own
description, not a copy of the Recommendation.

## The syntax (6.3)

| Form | Function | Response |
|---|---|---|
| `+V<name>=<values>` | Set a parameter | OK, or ERROR and no change |
| `+V<name>?` | Read a parameter | The values only, for example `128` |
| `+V<name>=?` | Test a parameter | The ranges in parentheses, for example `(0-128)` |
| `+V<action>` | Do an action | As the action tells |
| `+V<action>=?` | Test an action | OK if the modem has it |

`+FCLASS=?` is different: its list has no parentheses, as in T.31.

## The voice states (4)

| State | Start | End |
|---|---|---|
| Command | `AT+FCLASS=8` | |
| Transmit | `AT+VTX`, then CONNECT | `<DLE><ETX>` from the computer |
| Receive | `AT+VRX`, then CONNECT | Any character from the computer, or `<DLE>!` |
| Duplex | `AT+VTR`, then CONNECT | `<DLE>^` from the computer |

## The main commands (9 and 10)

| Command | Function | Default |
|---|---|---|
| `+FCLASS=8` | Voice mode | |
| `+VLS=<n>` | The device: 0 none (on hook), 1 the line, others as `+VLS=?` lists | 0 |
| `+VSM=<cml>,<vsr>,<scs>,<sel>` | Sample format, rate, silence compression, silence expansion | |
| `+VTS=<string>` | DTMF digits, `[f1,f2,d]` tones, `{d,dur}` digits with a duration | |
| `+VTD=<dur>` | The default tone duration, in 10 ms | 100 |
| `+VGT=<level>`, `+VGR=<gain>` | The transmit volume and the receive gain. 128 is nominal, 0 is automatic | 0 or 128 |
| `+VIT=<timer>` | The DTE/DCE inactivity timer, in s. 0 is off | 0 |
| `+VSD=<sds>,<sdi>` | The silence detection: sensitivity, and time in 0.1 s | 128, 50 |
| `+VRA=<n>`, `+VRN=<n>` | The ringback timers, in 0.1 s | 50, 10 |
| `+VNH=<hook>` | No automatic hang-up after data or fax | 0 |
| `+VIP` | All voice parameters to their defaults | |
| `+VCID=<pmode>` | Caller ID: 0 off, 1 formatted, 2 the message in hexadecimal | 0 |
| `+VRID=<rmode>` | The caller ID of the last call again: 0 formatted, 1 in hexadecimal | 0 |
| `+VDR=<enable>,<report>` | The cadence of the ring (`DRON=`, `DROF=`) | 0,0 |
| `+VDT=<enable>,<report>` | The cadence of the control tones (`CPON=`, `CPOF=`) | 0 |

## The caller ID reports (9.2.3)

The formatted report has one tag on each line: `DATE = MMDD`,
`TIME = HHMM`, `NMBR = <number>` and `NAME = <name>`. `P` is a private
number or name. `O` is a number or name that is not available. The
unformatted report gives all of the message in hexadecimal. In voice
mode, the report goes between `<DLE>X` and `<DLE>.`.

## The `<DLE>` codes (6.3.7, 7.3, 8.3)

| Code | From the modem | From the computer |
|---|---|---|
| `<DLE><DLE>` | A 10h sample | A 10h sample |
| `<DLE><ETX>` | The end of the stream | The end of the playback |
| `0` to `9`, `*`, `#`, `A` to `D` | A DTMF digit | |
| `b` | Busy tone | |
| `c` | Fax calling tone (CNG) | |
| `d` | Dial tone | |
| `e`, `f` | The calling tone and the answer tone of a data modem | |
| `R` | Ring | |
| `s` | Silence | |
| `q` | Quiet after speech | |
| `!` | | Stop the recording |
| `^` | | Stop the duplex state |
| `u`, `d` | | The volume up, down |

## V.253 in D-Modem

| Item | D-Modem (`core/at.cpp`, `core/modem.cpp`, `dsp/`) |
|---|---|
| Formats (`+VSM=?`) | 0 and 1 (8-bit PCM), 4 and 5 (G.711 µ-law and A-law), and our own numbers from 128 (linear, A-law, µ-law, IMA ADPCM, G.726, ZyXEL ADPCM) |
| `+VSM` with less than four values | Accepted. A rate that is not given is 8000. V.253 asks for ERROR, but vgetty sends two values |
| Silence compression | `<scs>` and `<sel>` are kept for `+VSM?`, but the modem does not compress silence |
| `+VGT`, `+VGR` | 0 to 128 |
| Devices (`+VLS`) | 0 and 1. Also 14 and 15 when the line has a handset |
| `+VRID` | The last caller ID message, from the line or from the SIP call. Nothing if no caller ID came |
| `+VDR`, `+VDT` | Not implemented. The modem has no ring or tone cadence reports |
| `+VBT`, `+VPR` | Not implemented. `+ITF` and `+IPR` do these functions |
| Speakerphone (`+VSP`, `+VDX`, `+VGM`, `+VGS`) | Not implemented |
| `+VDID`, `+VRL` | Not implemented |
| The `#V` commands of Rockwell and USRobotics | The same functions, with their old response forms (`+VGT=128`) |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| Read (6.3.3.2.2) | `+VGT=128` | `128` |
| Test (6.3.3.4.2, 6.3.3.4.3) | `+VGT=0-128`, `0,1,2` for `+VCID=?` | `(0-128)`, `(0-2)` |
| Test of an action (6.3.3.1.2) | ERROR for `+VIP=?`, `+VTX=?`, `+VRX=?`, `+VTR=?` | OK |
| `+VRID` (9.1.3) | ERROR | The caller ID of the last call |
