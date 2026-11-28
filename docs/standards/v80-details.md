# V.80: technical details

This page gives the technical details for [V.80](v80.md). The clause
numbers are the clause numbers of V.80 (08/96). This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on [the data link layer: technical
details](../link-details.md#v80).

## How the mode starts (8.1 to 8.3)

| Command | Function |
|---|---|
| `AT+ES=6` | The modem that calls asks for synchronous access mode |
| `AT+ES=,,8` | The modem that answers accepts synchronous access mode |
| `AT+ESA=...` | The parameters of the mode (the table below) |
| `AT+ITF=<off>,<on>,<report_period>` | The flow control thresholds and the buffer reports (8.4) |

- After CONNECT, the mode starts in the transparent sub-mode, in the two
  directions.
- The character format is 8 data bits, no parity. `+ICF` has no effect.
- With `+ER=1`, the modem reports `+ER: NONE` before CONNECT (8.3).

## The +ESA parameters (8.2)

| Parameter | Values | Default |
|---|---|---|
| `<trans_idle>` | 0: the SYN octet `<syn1>` as idle, no hunt. 1: hunt for 8-bit SYN. 2: hunt for 16-bit SYN | 0 |
| `<framed_idle>` | 0: flags between frames. 1: marks | 0 |
| `<framed_un_ov>` | 0: an underrun or an overrun gives an abort. 1: a flag, then discard until `<EM><resume>` | 0 |
| `<hd_auto>` | Half-duplex V.34 control | 0 |
| `<crc_type>` | 0: no FCS. 1: FCS-16. 2: FCS-32 | 0 |
| `<nrzi_en>` | The NRZI code | 0 |
| `<syn1>`, `<syn2>` | The SYN octets | 255, 255 |

## The in-band codes (Table 9)

| Code | From the computer | To the computer |
|---|---|---|
| 5Ch, 76h, A0h, A1h (T1 to T4) | Send one 19h, 99h, DC1 or DC3 | One of them received |
| 5Dh, 77h, A2h, A3h (T5 to T8) | Send two of the same | Two of the same received |
| A4h to AFh (T9 to T20) | Send a pair of two different ones | A pair received |
| B0h mark | Go to the transparent sub-mode | An abort (seven or more 1s) received |
| B1h flag | Send a flag (and the FCS). Go to the framed sub-mode | A good frame ended |
| B2h err | Send an abort | A bad frame ended, or the hunt found a flag |
| B3h hunt | Start the hunt of the receiver | (none) |
| B4h under | (none) | Transmit underrun |
| B5h tover | (none) | Transmit overrun |
| B6h rover | (none) | Receive overrun: data was lost here |
| B7h resume | Continue after an underrun or an overrun | (none) |
| B8h bnum | (none) | Two octets follow: the octets in the transmit buffer |
| B9h unum | (none) | Two octets follow: the octets discarded |
| BAh eot | End the carrier | The carrier is gone |
| BBh ecs | Go to online command state | The confirmation |
| BCh rrn | Ask for a rate renegotiation | A renegotiation |
| BDh rtn | Ask for a retrain | A retrain |
| BEh rate | Two octets follow: the highest transmit and receive rates | Two octets follow: the new rates |

## The rate codes (Table 10)

The rate code is 20h for 1200 bit/s, 21h for 2400 bit/s, and 22h for
4800 bit/s. Each code after 22h adds 2400 bit/s, to 2Eh for 33600 bit/s.

## The two-octet counts (8.4)

`<octnum0>` and `<octnum1>` give a count from 0 to 16383. Bit 0 of each
octet is 0, so the octets are never EM, DC1 or DC3. Bits 1 to 7 of
`<octnum0>` are the low 7 bits of the count. Bits 1 to 7 of `<octnum1>`
are the high 7 bits.

## The procedures (8.8)

1. **A sub-mode change.** The modem flushes its receive buffer each time
   that the sub-mode changes.
2. **Transparent, transmit (8.8.1.1).** When the computer has no data,
   the modem sends `<EM><under>` and sends SYN octets on the line.
3. **Transparent, receive (8.8.1.2).** With `<trans_idle>` 1 or 2, the
   receiver discards the data until it finds the SYN sequence. Then it
   sends the octets, with the SYN, aligned on the SYN.
4. **Framed, transmit (8.8.2.1).** `<EM><flag>` ends a frame. The modem
   sends the FCS and a flag. An underrun in a frame gives an abort or a
   flag (`<framed_un_ov>`), and `<EM><under>`. An underrun between frames
   gives flags or marks, and no `<EM><under>`.
5. **Framed, receive (8.8.2.2).** The receiver hunts for a flag and sends
   `<EM><err>`. Then it sends each frame, followed by `<EM><flag>` or
   `<EM><err>`.
6. **Escape (8.8.3).** `<EM><ecs>` gives `<EM><ecs>` and OK. The modem
   goes to online command state. `+++` is data in this mode.
7. **Retrain and renegotiation (8.8.4.1, 8.8.4.2).** At the end, the
   modem sends `<EM><rate>` with the new rates.
8. **The end of the carrier (8.8.4.3).** The carrier ends after
   `<EM><eot>` from the computer, or when the remote modem ends it. Then
   the modem sends `<EM><eot>` and NO CARRIER. It goes to command state,
   but it stays connected to the network (off hook).

## V.80 in D-Modem

| Item | D-Modem (`link/v80.cpp`, `core/modem.cpp`) |
|---|---|
| Modes | Synchronous access mode only. No synchronous mode (8.6) and no frame tunnelling mode (8.7) |
| The 7-bit in-band commands (clause 7, `+IBC`, `+IBM`) | Not implemented |
| `<hd_auto>`, `<nrzi_en>` | 0 only |
| `<EM><tover>` | Never sent. Flow control stops the computer before the buffer is full |
| `<EM><bnum>` | Each `<report_period>` × 10 ms |
| Shielding to the computer | 19h and 99h always. DC1 and DC3 when the modem uses DC1/DC3 flow control (`+IFC=,1`) |
| Receive buffer | 2048 octets, then `<EM><rover>` |
| `<EM><rtn>`, `<EM><rrn>` | V.34, V.90, V.92 and the V.32 family |
| After the end of the carrier | Off hook in command state until `ATH`. If the line ends the call, or DTR goes off, the modem goes on hook |

## Corrections (2026-11)

| Item | Before | Now |
|---|---|---|
| The end of the carrier (8.8.4.3) | The modem went on hook | The modem stays off hook until `ATH` |
