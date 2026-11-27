# V.26 bis: technical details

This page gives the technical details for [V.26 bis](v26bis.md). The
clause numbers are the clause numbers of V.26 bis (11/1988). This page
is our own description, not a copy of the Recommendation.

## The forward channel (2)

| Item | Value |
|---|---|
| Carrier | 1800 Hz ± 1 Hz. There is no pilot tone. |
| Modulation rate | 1200 baud ± 0.01 % |
| Phase of the transmitted spectrum | Linear. Figure 1 gives the limits of the phase distortion. |
| Level | As specified in V.2 |
| Division of power | Equal between the forward and backward channels (provisional) |
| Frequency offset | The receiver must operate with an error of at least ± 7 Hz. |
| Equalizer | A fixed compromise equalizer in the receiver (10) |

### 2400 bit/s (2.4)

Each dibit is a phase change from the previous symbol. The first bit of
the dibit is the first bit of the data. This is alternative B of V.26.

| Dibit | Phase change |
|---|---|
| 00 | +45° |
| 01 | +135° |
| 11 | +225° |
| 10 | +315° |

Some old modems use alternative A of V.26, with different phase changes.

The synchronizing signal is the continuous dibit 11. The modem sends it
from circuit 105 ON (or circuit 107 ON) to circuit 106 ON. The receiver
uses it to find the clock. Users must make sure that the data contains
many ONE bits, because the dibit 11 helps the clock to stay stable.

### 1200 bit/s (2.5)

Each bit is a phase change: +90° for ZERO, +270° for ONE.

## The backward channel (4)

| Item | Value |
|---|---|
| Maximum rate | 75 baud |
| ONE bit (mark, F<sub>Z</sub>) | 390 Hz |
| ZERO bit (space, F<sub>A</sub>) | 450 Hz |
| No data at the interface | Send the mark (ONE) |

The tolerances are the tolerances of R.35. The ± 6 Hz drift of the line
adds distortion. The design must include this distortion.

## Interchange circuits (5)

Table 2 lists the V.24 circuits for each configuration: one way or
either way, with or without the backward channel.

### Response times (5.2)

The test signal for circuit 109 is the synchronizing signal. The times
apply from 3 dB above the ON level of the detector to the maximum level.

| Circuit | OFF to ON | ON to OFF |
|---|---|---|
| 106 (ready for sending) | 750 ms to 1400 ms in the call set up. Else, 65 ms to 100 ms with limited echo protection, or 200 ms to 275 ms with echo protection. | 2 ms or less |
| 109 (forward channel detected) | 300 ms to 700 ms in the call set up. Else, 5 ms to 15 ms. | 5 ms to 15 ms |
| 121 (backward channel ready) | 80 ms to 160 ms | 2 ms or less |
| 122 (backward channel detected) | Less than 80 ms | 15 ms to 80 ms |

The longer times of circuits 106 and 109 apply only to automatic call
and answer, in the call set up. The times are provisional. The short ON
time of circuit 109 can let parts of the synchronizing signal go to
circuit 104.

### The detectors: circuits 109 and 122 (5.3)

| Received level | Circuits 109 and 122 |
|---|---|
| More than -43 dBm | ON |
| Less than -48 dBm | OFF |
| Between -48 dBm and -43 dBm | Not specified |

The ON level must be at least 2 dB higher than the OFF level. On a line
with known conditions, the operator can make the detectors less
sensitive (for example, -33 dBm and -38 dBm).

### Half duplex (5.4)

When circuit 105 is ON, the modem holds circuit 104 at ONE and circuit
109 OFF. It can continue for 150 ms ± 25 ms after circuit 105 goes OFF.
The same rule applies to circuits 119 and 122 when circuit 120 is ON.

### Other items (5.5 to 10)

- The DTE interprets a fault on circuit 107 as OFF. The modem interprets
  a fault on circuits 105 and 108 as OFF.
- The modem gives the clocks on circuits 114 and 115. The DTE can give
  the transmit clock on circuit 113.
- The electrical characteristics are the characteristics of V.28.
- The operator must not adjust the send level or the receive
  sensitivity.
- If the echo suppressors must be disabled, use the procedure of
  [V.25](v25.md).

## V.26 bis in D-Modem

| Item | D-Modem (`dsp/dp_v26bis.cpp`) |
|---|---|
| Direction | One way: the answer modem sends the forward channel, the call modem the backward channel, for the full call |
| Rate | 2400 bit/s, or 1200 bit/s when `+MS` gives a maximum below 2400 bit/s |
| Synchronizing signal | The dibit 11 at 2400 bit/s. At 1200 bit/s, V.26 bis gives no signal. D-Modem sends continuous ONE bits (+270°). |
| Circuit 106 of the answer modem | The synchronizing signal for 1 s, and until 150 ms of the mark of the call modem. Then CONNECT. |
| Rate at the call modem | From 32 symbols of the synchronizing signal in a row |
| Circuit 109 of the call modem, call set up | CONNECT after 340 ms of its own mark: about 390 ms after the synchronizing signal starts |
| Circuit 109 of the call modem, in data | `Circuit109` on the band of the forward channel. OFF in 6 ms to 11 ms. ON 8 ms to 11 ms after a dropout. No data goes to the DTE while it is OFF. |
| Circuit 122 of the answer modem | The FSK detector of [V.23](v23-details.md#v23-in-d-modem): ON in 48 ms or less, OFF in 15 ms to 80 ms |
| Equalizer | Adaptive, from the decisions. It starts at the data. |
| Not implemented | The half duplex forward channel, circuits 105, 106 and 121 as separate signals, alternative A |

### Corrections (2026-11)

| Error | Correction |
|---|---|
| The call modem gave CONNECT about 230 ms after the synchronizing signal started. V.26 bis requires 300 ms to 700 ms for circuit 109 in the call set up (Table 3). | The call modem sends its mark for 340 ms before CONNECT. The time is now about 390 ms. |
| In data, circuit 109 of the call modem used the 20 ms meter of the start-up. It took up to about 220 ms to turn OFF, and it came ON again at once after a dropout. V.26 bis requires 5 ms to 15 ms for the two changes. | In data, `Circuit109` with the meters of [V.26 ter](v26ter-details.md#corrections-2026-11), and ON 8 ms after a dropout. |
| While the carrier was lost in data, the receiver sent noise to the DTE. | No data goes to the DTE while circuit 109 is OFF. |

The test `V26bis.Circuit109` (`dsp/test/pump_test.cpp`) measures the
levels and the times on an 1800 Hz tone. It also measures the time to
CONNECT in a call between two D-Modem pumps.
