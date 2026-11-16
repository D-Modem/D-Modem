# ZyXEL fax mode (class 6)

Status: in D-Modem. `AT+FCLASS=6`, `#F`, `#B0` to `#B3`, `#V`, `#T`,
`#R`, `#L`, `#C`, `#P` and `&N32`.

## What it is

The ZyXEL U-1496 family and the Elite 2864 have their own fax command
set. ZyXEL calls it the "extended fax AT commands". Old ZyXEL fax
programs use it: ZFAX, and the `rcvfax` program that receives faxes for
a BBS. The modem does T.30 itself. The DTE gives and gets the pages as
T.4 data, with no `+F` commands.

The design comes from the U-1496S 5.06B firmware and from the Elite 2864
user's guide (chapter 15). The trace notes have the addresses.

## How to use

```
AT+FCLASS=6#P5551111
ATD5552222
```

| Command | Function |
|---|---|
| `AT+FCLASS=6` | ZyXEL fax mode. `AT+FCLASS?` gives `6`. |
| `AT#F`, `AT&N32` | A V.17 fax call on the ZyXEL modems. In class 6 all calls are fax calls, thus D-Modem accepts them with no effect. |
| `AT#B0` | Receiving: the pages go to the DTE after `CONNECT FAX/...` (the default). |
| `AT#B1` | Receiving for a BBS: `CONNECT FAX` and `ZyXEL`, then the parameters and each page after a DC2 from the DTE. |
| `AT#B2` | Polling: the next call receives the document of the remote. Refer to [Polling](#polling-b2). |
| `AT#B3` | Measures the ring cadence until a key. Refer to [Ring cadence](#ring-cadence-b3). |
| `AT#V0`, `AT#V1` | Normal or fine resolution (fine is the default). |
| `AT#Tn`, `#Rn`, `#Ln`, `#Cn` | Kept, with no effect. The data is always MH, 1728 pels, any length, with no fill. |
| `AT#P<id>` | The local identity (the CSI and the TSI). It is the remainder of the line or a string in quotes, up to 25 characters. The CSI and the TSI have the first 20. |

The settings are not stored in the modem. Put them in the init string.

## Sending

1. `ATD` dials. When the DIS comes, the modem sends the DCS at once: V.17
   at 14400 bps at most, no error correction mode, fine resolution if
   `#V1` and the remote has it.
2. When the CFR comes, the modem gives
   `CONNECT FAX/S1440V1T0R0L2C7P<id>`. `<id>` is the CSI of the remote,
   20 characters, with spaces at the end.
3. The DTE sends the pages: T.4 MH with EOLs, the first bit of each octet
   in the lowest bit, no DLE codes. An RTC (six EOLs) ends each page. If
   more data follows an RTC, the modem sends MPS and the next page.
4. The DTE stops. The ZyXEL modems end the document when the DTE sets
   RTS off. D-Modem has no RTS signal. Thus the document ends when the
   DTE sends no data for 2 seconds. The modem then sends EOP.
5. At the end the modem gives `DISCONNECT0P<id>` if the remote confirmed
   the last page, or `DISCONNECT1P<id>`. The modem is then in command
   mode.

If the call ends before `CONNECT FAX`, the modem gives `NO CARRIER`. If
the DTE sends no data for 30 seconds after `CONNECT FAX`, the modem ends
the call (`DISCONNECT1`).

## Receiving

`ATA` (or S0) answers. The modem answers as a data modem first, as the
ZyXEL modems do. A data caller gets a data call (`CONNECT <rate>`, with
no `DATA`). After the data call the modem is in class 6 again. A fax
caller is found as with `AT+FAA=1` (its CNG, the CM of V.8, T.38, or no
answer to the data signals). Then:

With `#B0`:

```
CONNECT FAX/S1440V1T0R0L2C7P5551111
<page 1><RTC><page 2><RTC>
DISCONNECT0P5551111
```

With `#B1`:

```
CONNECT FAX
ZyXEL
                       (the DTE sends DC2, hex 12)
<CR><LF>/S1440V1T0R0L2C7P555111           (35 characters)
<page 1><RTC>
                       (the DTE sends DC2)
<page 2><RTC>
DISCONNECT0P5551111
```

The modem sends each page when it is complete. With `#B1`, the
parameter line has 35 characters, thus only 18 characters of the
identity. The firmware does the same (it copies 35 bytes). If the call
is over and the DTE sends no DC2 for 60 seconds, the modem stops the
remaining pages and gives `DISCONNECT`.

`ATV0` and `ATQ1` have no effect on `CONNECT FAX` and `DISCONNECT`.

## Polling (#B2)

```
AT+FCLASS=6#B2#P5551111
ATD5552222
CONNECT FAX/S1440V1T0R0L2C7P5552222
<page 1><RTC>
DISCONNECT0P5552222
```

1. `#B2` sets polling for the next call only. ATD dials.
2. When the DIS comes, the modem sends its CIG (`#P`) and a DTC in
   place of the DCS.
3. The remote sends its document. The modem gives it to the DTE as when
   it receives a call: `CONNECT FAX/...`, or `CONNECT FAX` and `ZyXEL`
   with `#B1`.
4. At the end the modem gives `DISCONNECT0P<id>` or `DISCONNECT1P<id>`.

If the DIS of the remote does not show a document to be polled, the
modem ends the call and gives `NO CARRIER`. The ZyXEL firmware sends
the DTC also in this case. A fax machine with no document then ends the
call.

## Ring cadence (#B3)

`AT#B3` is a test of the ring detector. The modem must be idle.

```
AT#B3

Start to measure ring cadence...
Hit any key to stop!
OFF      1020 ms
ON       1980 ms
OFF      4020 ms
ON       1980 ms
                       (the DTE sends a character)
OK
```

- The modem gives one line at each start and end of a ring burst.
  `OFF` is the time with no ring before a burst. `ON` is the time of
  the burst. The first `OFF` starts at the command.
- The time has 8 characters of 10 ms steps, then `0 ms`.
- The modem gives no `RING` during the measurement, and it does not
  answer.
- One character from the DTE stops the measurement. The modem gives
  `OK`.
- A ring burst ends 100 ms after the last cycle of the ring signal.
  Only a line with its own ring signal (a sound card line, a pipe or a
  USB modem) has ring bursts. A SIP call gives no lines.

## The CONNECT FAX fields

| Field | Meaning |
|---|---|
| `S<rate>` | The first 4 digits of the rate: 1440, 1200, 9600, 7200, 4800, 2400. |
| `V<n>` | 0 normal, 1 fine resolution. |
| `T<n>` | Coding. The data of the DTE is MH, thus D-Modem gives `T0`. |
| `R<n>` | Width. 0 is 1728 pels. |
| `L<n>` | Length. 0 A4, 1 B4, 2 any length. D-Modem gives `L2`. |
| `C<n>` | Minimum row time. 7 is 0 ms. D-Modem gives `C7`: the data has no fill, and the modem adds the fill on the line. |
| `P<id>` | The identity of the remote, 20 characters. |

In the firmware, `T`, `R`, `L` and `C` come from bits 16 to 23 of the
DCS (`C` is bits 21 to 23, with bit 21 as the lowest). D-Modem gives the
values that agree with the data the DTE gets or sends.

## What is not the same as the firmware

| Item | ZyXEL | D-Modem |
|---|---|---|
| End of the document | RTS off | 2 seconds with no data from the DTE |
| The pages to the DTE | As they come from the line | Each page when it is complete, coded again as MH |
| Flow control while sending | CTS | The DTE buffer of D-Modem |
| `#T1` (MR data) | MR | MH only |
| `#B2` with no document at the remote | DTC | `NO CARRIER` (no DTC) |
| `#B3`, the label of a line | `ON` or `OFF` at a change of the ring status bit (the polarity is not traced) | `ON` gives the time of a burst, `OFF` the time between bursts |
| `#B3`, the end of a burst | The ring status bit of the DSP | 100 ms with no cycle of the ring signal |
| `+FCLASS=?` | `0,2,8` (6 is not in the list) | `0,1,1.0,2,2.0,6,8` |
| `&N32` in class 0 or 2 | A class 2 fax call | Accepted, no effect |
| The bit order of the data | Not traced | The first bit in the lowest bit (as `+FBO=0`) |

The Elite user's guide gives the DC2 as "hex18". The firmware tests for
hex 12 (DC2). Hex 18 (CAN) sets a different flag. D-Modem uses hex 12.

## Tests

- `core_test` (`CoreZfax.TwoPages/B0`, `/B1`): two modems. A sends two
  pages to B. A gets `CONNECT FAX/...` and `DISCONNECT0`. B gets the same
  pages. With `#B1`, B gets nothing until each DC2.
- `core_test` (`Core.ZfaxCommands`): the ranges of the `#` settings.
- `core_test` (`Core.ZfaxPoll`): A in class 6 with `#B2` polls B (class
  2.0, `+FLP=1`). A gets `CONNECT FAX/...`, the page of B and
  `DISCONNECT0`.
- `core_test` (`Core.ZfaxRingCadence`): two ring bursts give four
  lines. No `RING` comes. A key gives `OK`, and the next ring gives
  `RING`.
- `core/test/scripts/zfax.txt`: a data call to a modem in class 6, and
  class 6 after it.
