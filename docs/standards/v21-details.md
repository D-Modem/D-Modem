# V.21: technical details

This page gives the technical details for [V.21](v21.md). The clause
numbers are the clause numbers of V.21 (11/1988). This page is our own
description, not a copy of the Recommendation.

## The signal (2, 3)

| Item | Channel 1 | Channel 2 |
|---|---|---|
| Sent by | The call modem | The answer modem |
| Mean frequency | 1080 Hz | 1750 Hz |
| ONE bit (mark, F<sub>Z</sub>) | 980 Hz | 1650 Hz |
| ZERO bit (space, F<sub>A</sub>) | 1180 Hz | 1850 Hz |

- The frequency shift is ± 100 Hz from the mean frequency. The higher
  frequency is the ZERO bit.
- The modulation rate in baud is the same as the bit rate. The maximum is
  300 bit/s. Some old modems operate at 200 bit/s only.
- The transmitter must be in ± 6 Hz of each frequency.
- The line can move the frequency by ± 6 Hz more. Thus, the receiver must
  operate with a frequency error of ± 12 Hz.

## Data (4)

The modem can send asynchronous characters or synchronous bits. In
synchronous operation, the modem does not add bits to keep the
synchronization when there is no data.

## Channels when only one direction sends data (7)

The call modem always sends on channel 1. This rule also applies when
only one modem sends data and the other modem sends only check signals.
Two persons can agree to use the channels in a different way for a
special service.

## Level (6)

- The maximum power to the line is 1 mW (0 dBm).
- At the input of an international circuit, the nominal level must not be
  more than -13 dBm0 (refer to V.2).

## The carrier detector: circuit 109 (8.3)

| Received level | Circuit 109 |
|---|---|
| More than -43 dBm | ON |
| Less than -48 dBm | OFF |
| Between -48 dBm and -43 dBm | Not specified |

The detector must have hysteresis. The level that turns the detector ON
must be at least 2 dB higher than the level that turns it OFF. An
operator can make the detector less sensitive (for example, -33 dBm and
-38 dBm) on a line with known conditions.

## Response times (8.2)

On the switched telephone network, these times apply:

| Circuit | OFF to ON | ON to OFF |
|---|---|---|
| 106 (ready for sending) | 400 ms to 1000 ms | 2 ms or less |
| 109 (carrier detected) | 300 ms to 700 ms | 20 ms to 80 ms |

On leased lines without alternate voice, the OFF to ON times are shorter:
20 ms to 50 ms for circuit 106 and 20 ms or less for circuit 109.

## Echo control (5)

If the echo suppressors of the network must be disabled, use the
procedure of [V.25](v25.md).

## Interchange circuits (8, 9)

V.21 lists the V.24 circuits that the modem must have. For the switched
network, they are 102 to 107, 108/1 or 108/2, 109 and 125. The electrical
characteristics are the characteristics of V.28. A softmodem has no
physical circuits. D-Modem gives the same information through AT
commands and result codes.

## V.21 in D-Modem

`dsp/dp_fsk.cpp` has the modulator (`FskMod`) and the demodulator
(`FskDemod`). The table of modes in the same file gives the frequencies
of V.21, V.23, Bell 103 and Bell 202.

The demodulator has these parts:

1. A band-pass filter around the channel.
2. A complex down converter to the centre of the channel.
3. A low-pass filter.
4. A discriminator that measures the phase change between two samples.
5. A bit clock that locks to the bit transitions.

The carrier detector examines the level and the frequency deviation. The
deviation check makes sure that the detector does not identify a 2100 Hz
answer tone as a V.21 carrier.

### Corrections (2026-11)

A check against clause 8 found two errors in the carrier detector, and
one error in the call set up:

| Error | Correction |
|---|---|
| The detector turned ON at about -40.5 dBm0. V.21 requires ON above -43 dBm (8.3). | The detector turns ON at about -42.7 dBm0 and OFF at about -46.5 dBm0. The hysteresis is about 4 dB. |
| After the far carrier stopped, the detector stayed ON for up to about 220 ms. V.21 requires ON to OFF in 20 ms to 80 ms (8.2.2). | A second power meter (one bit, 4 ms at most) finds the end of the carrier. The detector turns OFF when this meter stays below the OFF level for 5.4 bits (18 ms at 300 baud). The time is now about 25 ms to 60 ms. |
| In the call set up, the answer modem set circuit 109 ON (CONNECT) about 170 ms after the mark of the call modem started. V.21 requires 300 ms to 700 ms (8.2.2, Note 2). | The answer modem waits for 300 ms of mark (about 320 ms in total). The call modem stays in its mark for 350 ms. Thus, it sets circuit 109 ON about 500 ms after the mark of the answer modem starts, after the answer modem. |

This page takes 0 dBm0 at the codec as 0 dBm at the line. The tests
`Fsk.CarrierThresholds` and `Fsk.CarrierOffTime`
(`dsp/test/pump_test.cpp`) measure the levels and the time.
`Fsk.CallSetupCarrierOnTime` measures the call set up times of V.21 and
V.23 at the two modems. The same
detector is in V.23, Bell 103 and Bell 202. V.23 has the same thresholds
as V.21, and shorter OFF times. `Fsk.V23CarrierOffTime` measures the
OFF times of V.23 (5 ms to 15 ms at 1200 baud, 15 ms to 80 ms at 75
baud).

Because the detector now turns OFF earlier, fewer incorrect characters
go to the DTE after the far modem disconnects. The transcripts
`call_v22.expect` and `data_drop.expect` (`core/test/scripts/`) show
this change.
