# V.58: technical details

This page gives the object classes of V.58 (09/94), and the test
function object in more detail. For a summary, refer to [V.58](v58.md).

## Compliance (1.2)

An implementation of V.58 must:

- support the vSeriesDce object, at least,
- support the mandatory package of each object that it supports, and
- support all the attributes, actions and notifications of each package
  that it supports. An attribute or action for a function that the DCE
  does not have makes no change to the DCE.

## The object classes (3)

| Clause | Object class | It holds |
|---|---|---|
| 3.1 | vSeriesCallControl | Dialling, answering, the call progress and the numbers |
| 3.2 | vSeriesDataCompression | V.42 bis and other compression, with its parameters |
| 3.3 | vSeriesDce | The DCE itself: its identity, its state, the self-test |
| 3.4 | vSeriesDteInterface | The V.24 circuits, the DTE rate and the character format |
| 3.5 | vSeriesErrorControl | V.42 and other error control, with its parameters |
| 3.6 | vSeriesLineInterface | The line: leased or switched, 2-wire or 4-wire, the levels |
| 3.7 | vSeriesSignalConvertor | The modulation, the rates and the line signal quality |
| 3.8 | vSeriesTestFunction | The loops of V.54 and the error rate test |

## The test function object (3.8)

### Attributes

| Attribute | Values |
|---|---|
| cct140Enable, cct141Enable | If FALSE, the DCE ignores circuit 140 or 141 |
| erroredBitsReceived, erroredBlocksReceived | 0 to 65535. The error rate test sets them to zero when it starts. They stay at the maximum count. |
| loop2Local | 0 disabled, 1 enabled and inactive, 2 front panel, 3 management system, 4 remote DCE |
| loop2LocalEnable | If TRUE, V.54 remote control or the front panel can close loop 2 here |
| loop2Remote | 0 disabled, 1 enabled and inactive, 2 circuit 140, 3 front panel, 4 management system |
| loop2RemoteEnable | If FALSE, the front panel cannot close loop 2 at the far end |
| loop3 | 0 inactive, 1 circuit 141, 2 front panel, 3 management system |
| loop3Enable | If TRUE, the front panel can close loop 3 |
| v54Address, v54Mode | The address of V.54 Table 4, and point-to-point or multipoint |

### Actions

| Action | What it does |
|---|---|
| invokeErrorRateTest | Starts a bit error test, a block error test or both. It has a block length (1 to 65535 bits), a number of blocks (1 to 65535) and a pattern (63, 511 or 2047 bits, all ONEs, or alternate ONEs and ZEROs). A loop must be closed first. |
| invokeLoop2Local | Closes or opens loop 2 here |
| invokeLoop2Remote | Closes or opens loop 2 at the far end with the signals of V.54. It confirms if the far end answers. |
| invokeLoop3 | Closes or opens loop 3 |
| stopErrorRateTest | Stops the error rate test. It gives the blocks sent, the errored blocks and the errored bits. |

### Notification

loop2InvokedByRemoteDce tells the management system that the far DCE
closed loop 2 here. There is no notification if loop2LocalEnable is
FALSE.

## The AT commands of V.250 (6.7)

| V.58 | V.250 |
|---|---|
| cct140Enable, cct141Enable | `+TE140`, `+TE141` |
| loop2LocalEnable | `+TERDL` |
| loop2RemoteEnable, loop3Enable | `+TEPDL`, `+TEPAL` |
| loop3, loop2Local, loop2Remote | `+TALS`, `+TDLS`, `+TRDLS` |
| v54Address, v54Mode | `+TADR`, `+TMODE` |
| invokeErrorRateTest, stopErrorRateTest | `+TTER` |
| erroredBitsReceived, erroredBlocksReceived | `+TNUM` |
| invokeLoop2Local, invokeLoop2Remote, invokeLoop3 | `+TLDL`, `+TRDL`, `+TAL` |
| The self-test of vSeriesDce | `+TSELF`, `+TRES` |

## V.58 in D-Modem

| Item | D-Modem (`core/v58.cpp`) |
|---|---|
| Objects | Only the test function object (3.8), and the self-test of vSeriesDce, through the `+T` commands. There is no TMN interface. |
| cct140Enable, cct141Enable, loop2RemoteEnable, loop3Enable | `+TE140`, `+TE141`, `+TEPDL`, `+TEPAL`: 0 only. There are no circuits 140 and 141, and no front panel. |
| loop2LocalEnable | `+TERDL` (the same as `&T4` and `&T5`) |
| loop2Local | `+TDLS`: 1, 3 (`+TLDL` or `&T3`), 4 (the far DCE) |
| loop2Remote | `+TRDLS`: 1, 4 (`+TRDL`, `&T6` or `&T7`) |
| loop3 | `+TALS`: 0, 3 (`+TAL`, `&T1` or `&T8`) |
| v54Address, v54Mode | `+TADR`, `+TMODE`: 0 only (point-to-point, no address) |
| invokeErrorRateTest | `+TTER`, types 1 to 3, with the 511-bit pattern only. Other patterns give ERROR. |
| erroredBitsReceived, erroredBlocksReceived | `+TNUM`, with a maximum of 65535 each. They stay until the next test starts. |
| loop2InvokedByRemoteDce | The log line "loop 2: the far end asks for it, looping". The DTE gets no result code. |
| The self-test | `+TSELF`: the pattern generator, the checker and the recognisers check themselves. `+TRES` gives 1 (pass) or 2 (fail). |

D-Modem has no other objects of V.58. The `+TMO` objects of V.59 give
diagnostic information.
