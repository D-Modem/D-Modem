# V.23: technical details

This page gives the technical details for [V.23](v23.md). The clause
numbers are the clause numbers of V.23 (11/1988). This page is our own
description, not a copy of the Recommendation.

## The forward channel (2, 3)

| Mode | Maximum rate | Mean frequency F<sub>0</sub> | ONE bit (mark, F<sub>Z</sub>) | ZERO bit (space, F<sub>A</sub>) |
|---|---|---|---|---|
| Mode 1 | 600 baud | 1500 Hz | 1300 Hz | 1700 Hz |
| Mode 2 | 1200 baud | 1700 Hz | 1300 Hz | 2100 Hz |

- Use mode 1 on lines with long loaded cables, or with signal
  receivers near 2000 Hz. Use mode 2 on other lines.
- The transmitter must be in ± 10 Hz of F<sub>Z</sub> and F<sub>A</sub>.
  This is a limit. Thus, F<sub>0</sub> is in ± 10 Hz, and the shift
  F<sub>A</sub> - F<sub>Z</sub> is in ± 20 Hz of its nominal value.
- The line can move the frequency by ± 6 Hz more. Thus, the receiver must
  operate with a frequency error of ± 16 Hz.

## The backward channel (4, 5)

| Item | Value |
|---|---|
| Maximum rate | 75 baud |
| ONE bit (mark, F<sub>Z</sub>) | 390 Hz |
| ZERO bit (space, F<sub>A</sub>) | 450 Hz |
| No data at the interface | Send the mark (ONE) |

The frequency tolerances are the tolerances of R.35 for voice frequency
telegraphy. The ± 6 Hz drift of the line adds distortion. The design must
include this distortion.

## Division of power (6)

When one modem sends on the two channels, the total power is 1 mW. Until
more study is done, divide the power equally between the two channels
(-3 dBm each).

## Information for the design (7)

- The loss between two subscribers is 5 dB to 30 dB at 800 Hz or
  1000 Hz. It can be up to 35 dB at F<sub>0</sub>.
- A good receiver range is -40 dBm to 0 dBm for the forward channel.
- The operator must not adjust the send level or the receive
  sensitivity.

## Interchange circuits (8.1, 8.2, 9)

V.23 lists the V.24 circuits for each configuration. The data can go one
way or either way, with or without the backward channel. The line can be
the switched network or a leased line. The backward channel adds circuits 118 to 122. The
electrical characteristics are the characteristics of V.28. A softmodem
has no physical circuits. D-Modem gives the same information through AT
commands and result codes.

## Response times (8.3)

The test tone is the mark (ONE) frequency. The times apply from 3 dB
above the ON level of the detector to the maximum level.

| Circuit | OFF to ON | ON to OFF |
|---|---|---|
| 106 (ready for sending) | 750 ms to 1400 ms in the call set up. Else, 20 ms to 40 ms without echo protection, or 200 ms to 275 ms with echo protection. | 2 ms or less |
| 109 (forward channel detected) | 300 ms to 700 ms in the call set up. Else, 10 ms to 20 ms. | 5 ms to 15 ms |
| 121 (backward channel ready) | 80 ms to 160 ms | 2 ms or less |
| 122 (backward channel detected) | Less than 80 ms | 15 ms to 80 ms |

The longer times of circuits 106 and 109 apply only to automatic call
and answer, in the call set up. V.23 gives these times as
provisional.

## The detectors: circuits 109 and 122 (8.4)

| Received level | Circuits 109 and 122 |
|---|---|
| More than -43 dBm | ON |
| Less than -48 dBm | OFF |
| Between -48 dBm and -43 dBm | Not specified |

The detector must have hysteresis. The ON level must be at least 2 dB
higher than the OFF level. On a line with known conditions, the operator
can make the detector less sensitive (for example, -33 dBm and -38 dBm).

## Half duplex on a 2-wire line (8.5)

When the modem sends on the forward channel (circuit 105 ON), it holds
circuit 104 at ONE and circuit 109 OFF. It can continue to do this for
150 ms ± 25 ms after circuit 105 goes OFF. This protects the receiver
from echoes of its own signal. The same rule applies to circuits 119 and
122 when the modem sends on the backward channel.

## Fault conditions (8.6)

The DTE interprets a fault on circuit 107 as OFF. The modem interprets a
fault on circuits 105 and 108 as OFF.

## Echo control (10)

If the echo suppressors of the network must be disabled, use the
procedure of [V.25](v25.md).

## Clock (11)

A clock is optional. If the modem has a clock for synchronous data, it
sends alternate ZERO and ONE bits between circuit 105 ON and circuit 106
ON. Part of this pattern can go to the far DTE after circuit 109 goes
ON. The DTE must identify these bits as not data.

## V.23 in D-Modem

| Item | D-Modem (`dsp/dp_fsk.cpp`) |
|---|---|
| Modes | Mode 2 (1200 baud) with the backward channel (75 baud). The answer modem sends the forward channel. |
| Mode 1 (600 baud) | Not implemented |
| Frequencies | 1300/2100 Hz and 390/450 Hz (`v23_params`) |
| Detectors | `FskDemod`: ON at about -42.7 dBm0, OFF at about -46.5 dBm0 |
| Circuit 109 (the call modem) | ON in 12 ms to 14 ms after the call set up. ON to OFF in 5 ms to 15 ms. |
| Circuit 122 (the answer modem) | ON in 48 ms or less. ON to OFF in 15 ms to 80 ms. |
| Call set up | The call modem waits for 150 ms of the mark, then sends its mark for 350 ms. It gives CONNECT about 500 ms after the mark of the answer modem starts. |
| Half duplex | `dsp/dp_sms.cpp` (fixed-line SMS) does not listen while it sends. |
| Not implemented | Circuits 105, 106 and 121 as separate signals. The 1200 bit/s channel in the two directions (half duplex) is only in the SMS pump. |

The detector is the detector of [V.21](v21-details.md#v21-in-d-modem).
It examines the level and the frequency deviation.

### Corrections (2026-11)

A check against clause 8 found these errors:

| Error | Correction |
|---|---|
| The detector of the forward channel turned ON in about 4 ms. V.23 requires 10 ms to 20 ms after the call set up (8.3.2). | At 600 baud and more, the detector turns ON after 12 ms of a level above the ON level. |
| Near the threshold, the detector of the backward channel took about 90 ms to turn ON. V.23 requires less than 80 ms for circuit 122 (8.3.2). | The level meter has a time constant of 50 ms or less (it was 107 ms at 75 baud). The time is now 48 ms or less. |
| The detectors stayed ON for too long after the far carrier stopped (8.3.2). | Refer to the [V.21 corrections](v21-details.md#corrections-2026-11). `Fsk.V23CarrierOffTime` measures the times. |

The tests `Fsk.V23CarrierOnTime` and `Fsk.CallSetupCarrierOnTime`
(`dsp/test/pump_test.cpp`) measure the ON times. The tests take 0 dBm0
at the codec as 0 dBm at the line.
