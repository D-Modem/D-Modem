# T.31: technical details

This page gives the commands, the modulation codes, the data streams
and the timers of T.31 (08/95). For a summary, refer to [T.31](t31.md).

## The commands (Table 5)

| Command | Function |
|---|---|
| `+FCLASS=1.0` | Select Class 1. `+FCLASS=?` gives the classes. |
| `+FTS=<t>` | Stop the transmission, then wait `t` × 10 ms. Mandatory values 0–255. |
| `+FRS=<t>` | Wait until there is silence for `t` × 10 ms. A character from the DTE stops the wait. |
| `+FTM=<mod>` | Send data on the modulation `mod` |
| `+FRM=<mod>` | Receive data on the modulation `mod` |
| `+FTH=<mod>` | Send HDLC frames on the modulation `mod` |
| `+FRH=<mod>` | Receive HDLC frames on the modulation `mod` |
| `+FAR=<n>` | Adaptive reception. 0 (off) is mandatory. |
| `+FCL=<t>` | The time from a carrier loss to the end of reception, `t` × 100 ms |
| `+FDD=<n>` | `<DLE><SUB>` for two `<DLE>` characters. 0 is mandatory. |
| `+FIT=<t>,<a>` | The DTE inactivity timer, `t` seconds (0 is off). With `a` = 0, the modem goes on hook and sets `+FCLASS=0`. With `a` = 1, it only goes on hook. |
| `+FMI?`, `+FMM?`, `+FMR?` | The same as `+GMI?`, `+GMM?` and `+GMR?` (Annex A) |
| `+FLO=<n>` | The same as `+IFC=<n>,<n>` (Annex A). `+FLO?` gives 255 for other `+IFC` values. |
| `+FPR=<n>` | The DTE rate as a code (Annex A) |

The action commands give ERROR when the modem is on hook.

## The modulation codes (Table 6)

| Code | Modulation | Rate (bit/s) |
|---|---|---|
| 3 | V.21 channel 2 | 300 |
| 24, 48 | V.27 ter | 2400, 4800 |
| 72, 96 | V.29 | 7200, 9600 |
| 73, 97, 121, 145 | V.17, long training | 7200, 9600, 12000, 14400 |
| 74, 98, 122, 146 | V.17, short training | 7200, 9600, 12000, 14400 |

Codes 3 (with `+FTH` and `+FRH`) and 24 and 48 (with `+FTM` and `+FRM`)
are mandatory.

## The start of a call (7.1)

- The answerer sends CED, then does `+FTH=3` and gives CONNECT.
- The caller sends CNG, then does `+FRH=3`.

## The data streams (6.11)

| Stream | Meaning |
|---|---|
| `<DLE><ETX>` | The end of the stream |
| `<DLE><DLE>` | One `<DLE>` in the data |
| `<DLE><SUB>` | Two `<DLE>` in the data, when `+FDD=1` |
| `<DLE><other>` | Removed |

## The sending commands (8.3.3, 8.3.5)

| Item | Rule |
|---|---|
| CONNECT | At the start of the training, not at its end |
| `+FTM` before the data | The training, then ones until the data comes |
| `+FTM`, buffer empty | If the last character was NUL: NULs until more data, or 5 s, then ERROR. Else: the carrier stops, then OK. |
| `+FTH`, each frame | The modem adds the FCS and a flag. The control field bit 0x10 (final) ends the transmission with OK. Else the modem gives CONNECT for the next frame. |
| `+FTH`, null frame | `<DLE><ETX>` alone: the carrier stops, then OK |
| `+FTH`, no data | 5 s after CONNECT with no data: the carrier stops, then ERROR |

## The receiving commands (8.3.4, 8.3.6, 8.4)

| Item | Rule |
|---|---|
| CONNECT | When the carrier (and for `+FRH`, a flag) comes |
| `+FRH`, each frame | The octets to the FCS, then `<DLE><ETX>`, then OK (FCS good) or ERROR |
| Carrier loss | NO CARRIER |
| A character from the DTE | The reception stops, then OK. DC1 and DC3 are flow control. |
| A different signal | +FCERROR. With `+FAR=1`, V.21 flags give `+FRH:3` and the modem does `+FRH=3`. |

## The DTE inactivity timer (8.5.4, Tables 8 and 9)

The timer runs only off hook. It starts when the modem waits for the
DTE: after a final result code, after CONNECT for transmit data, and
after flow control off. It stops when the DTE sends a command or data.

## T.31 in D-Modem

| Item | D-Modem (`dsp/fax_class1.cpp`, `core/at.cpp`) |
|---|---|
| Classes | `+FCLASS=1` and `1.0`. Class 1.0 has V.34 half-duplex (`+F34`). |
| Modulations | All the codes of Table 6. `+FTH` and `+FRH` take only 3. |
| `+FTM` fill | Ones before the first data. Zeros when the buffer is empty after data. |
| `+FTM`, buffer empty | 5 s with no data, then ERROR |
| `+FTH`, no data | 5 s after CONNECT with no data, then ERROR |
| `+FAR` | 0 only |
| `+FCL` | Kept, with no effect. The receivers find the carrier loss in their own time. |
| `+FDD` | 0 only. `<DLE><SUB>` from the DTE is discarded. |
| `+FIT` | 0–255 s, actions 0 and 1. The timer runs in command state only. |
| `+FLO`, `+FPR`, `+FMI`, `+FMM`, `+FMR` | As Annex A |
| T.38 | The same commands on IFP packets |
| Not implemented | +FCERROR. `+FRM` and `+FRH` wait for the selected signal only. |

When the buffer of `+FTM` is empty and the last character was not
NUL, D-Modem continues with zeros for up to 5 s. It does not stop at
once with OK. On a computer, the DTE data can arrive late. A stop at
once would end the page.

The tests `Class1T38*` (`dsp/test/class1_t38_test.cpp`) and
`Class1V34*` (`dsp/test/class1_v34_test.cpp`) send fax calls with a
Class 1 DTE of the test against the T.30 machine of D-Modem.

## Corrections (2026-11)

| Error | Correction |
|---|---|
| `+FAR`, `+FCL`, `+FDD`, `+FIT` and `+FLO` gave ERROR in Class 1 | The commands are accepted, with the values above |
| `+FTS=?` and `+FRS=?` gave ERROR | They give `(0-255)` |
| No DTE inactivity timer | `+FIT` hangs up after its time in command state |
| `+FTH` sent flags with no end when the DTE sent no frame | ERROR 5 s after CONNECT |
| `+FTM` sent zeros with no end when the buffer was empty | ERROR after 5 s |
| `+FTM` sent zeros before the first data | It sends ones (8.3.3) |

The tests `Core.FaxClass1Parameters` and `Core.FaxClass1DteTimeouts`
(`core/test/core_test.cpp`) and `Class1Audio.FtmOnesThenTimeout`
(`dsp/test/class1_t38_test.cpp`) check these corrections.
