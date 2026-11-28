# V.91: technical details

This page gives the technical details for [V.91](v91.md). The clause
numbers are the clause numbers of V.91 (05/99). This page is our own
description, not a copy of the Recommendation. The details of the
D-Modem implementation are on [V.91 digital to digital: technical
details](../dsp/v91-details.md).

## The line signals (6)

| Item | Rule |
|---|---|
| Data rates (6.1) | 28000 bit/s to 64000 bit/s, in steps of 8000/6 bit/s |
| Symbol rate (6.2) | 8000 symbols each second, from the clock of the digital network |
| Encoder (6.3) | The encoder of 5.4/V.90, with Sr = 0, S = 6 and K from 15 to 42 |
| Scrambler (6.3.1) | GPC of V.34 (1 + x<sup>-18</sup> + x<sup>-23</sup>), in the two modems |
| Control channel (6.4) | Only when the far modem asks for it (bit 27 of INFO). 96-bit sequences: a slip detection pattern (0111011111111010), a cleardown bit, a retrain bit, secondary channel data and a CRC. It uses 125 bit/s of the data rate. |
| Transparent mode (6.5) | The data bits go to the network as they are. Only on an unrestricted 64000 bit/s channel. |

## The start-up sequences (7)

| Sequence | Content |
|---|---|
| Ez (7.6) | 24T of zeros on the sign of Ucode 66, after the silence of V.8 |
| INFO, INFO' (7.8) | The capabilities and the DIL request of the modem, on the sign of Ucode 66. INFO' has bit 28 set. |
| Eu (7.4) | 12T of zeros. The default DIL follows, from data frame interval 0. |
| J (7.9) | The DIL descriptor of a modem that does not use the default DIL |
| PHIL (7.10) | Scrambled ones, until the modem can send the DIL that the far modem asks for |
| Em (7.7) | 12T of zeros. The DIL that the far modem asks for follows. |
| DIL (7.3) | As 8.4.1/V.90 |
| SCR (7.11) | Scrambled ones, an integer multiple of 6 symbols |
| CP, CP' (7.2) | The constellations and the rate that the modem can receive |
| Es (7.5) | 12T of zeros. B1 follows. |
| B1 (7.1) | Data frames of scrambled ones |

### The default DIL (Table 5)

| Parameter | Value |
|---|---|
| N | 125 segments |
| LSP, LTP | 12 |
| SP, TP | 0FC0 hex, 0FFF hex |
| H1 to H8 | 1 (segments of 12 symbols) |
| REF1 to REF8 | Ucode 0 |
| Training codes | 124, 0, 123, 1, 122, 2, and so on, to 62 |

## The operating procedures (8)

1. After V.8, each modem sends silence (Ucode 0) for 75 ms ± 5 ms, then
   Ez.
2. Each modem sends INFO until the far INFO arrives, then INFO'.
3. If the two modems ask for the default DIL, each modem sends Eu and the
   default DIL. If not, a modem sends J, PHIL, then Em and the DIL that
   the far modem asks for.
4. Each modem sends CP in less than 1.5 s after it receives Eu or Em.
   When the far CP arrives, it sends CP'.
5. After it sends CP' and receives CP' or Es, the modem sends Es and B1.
   The data follows.

| Timer | Rule |
|---|---|
| Escape to V.34 (8.3) | No INFO' in 1.5 s, or no B1 in 10 s, from the start of INFO. The modems continue with phase 2 of V.34 (INFO0). |
| Retrain (8.4) | A 10 s timer from the clamp of circuit 104. At its end, the modem stops the call. |
| Loss of frame synchronization (8.5) | A 10 s timer. The modem retrains, or finds the frames again. |
| Cleardown (8.6) | drn = 0 in CP, or bit 16 of the control channel |

- To retrain (8.4.1), a modem sends INFO again. The far modem retrains
  when it detects INFO.
- On a 4-wire leased circuit (8.8), the start-up begins at INFO, with no
  V.8.

## V.91 in D-Modem

| Item | D-Modem (`dsp/v91.cpp`) |
|---|---|
| Data rates | Up to K = 41 (62667 bit/s). A K of 42 (64000 bit/s) is not possible through the 16-bit linear audio of dmodemd. |
| DIL | We always ask for the default DIL. We send a DIL that the far modem gives in J. |
| Transparent mode | We never ask for it and never grant it. Through 16-bit linear audio, the µ-law codes +0 and −0 are the same value. |
| Control channel | We send it when the far modem asks for it |
| Ez | 24 symbols of −Ucode 66 |
| Timers | INFO' 1.5 s and B1 10 s (escape to V.34), retrain 10 s |
| Leased circuits | INFO at once, no V.8, no escape to V.34 |

We found no error in the V.91 procedures.
