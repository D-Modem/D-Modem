# V.wb: wideband on a bare pair

A phone line only carries sound up to about 3.4 kHz, which is why
dial-up modems top out around 56 kbit/s. A **bare pair** of copper wires
(one you own between two buildings, an old alarm circuit, a dry pair
rented from a phone company) has no such limit. **V.wb** is D-Modem's
own mode for such a pair. It uses frequencies up to 36 kHz and reaches
about 149000 bit/s each way.

It needs D-Modem at both ends, and hardware that samples the line at
48 kHz or more: the pico-serial WB stick (see
[Softmodem hardware](../hardware.md)), or any sound codec that fast. It
is not ADSL and does not connect to a phone company's equipment.

For the signal, the start-up and the measurements, see
[the technical details](vwb-details.md).

## Using it

At 48 kHz and above, V.wb is the only mode, and the line behaves like a
[leased line](../leased.md): no dial tone, no ringing.

    AT&L1S0=1        at the answering end
    AT&L1            at the calling end, then ATD

If the connection drops, the modems reconnect by themselves. Error
correction and compression run on top, as on any other call.

## How it works

The signal is split into many narrow tones, 187.5 Hz apart (the same
idea as ADSL and Wi-Fi). At the start the modems measure how clean each
tone is, then put more bits on clean tones and fewer on noisy ones. One
direction uses the lower part of the band and the other the upper part.

## Speeds in simulation

| Length of pair | Speed each way |
|---|---|
| 0.5 km | 148656 bit/s |
| 1 km | about 125000 bit/s |
| 3 km | about 100000 bit/s |
| 5 km | about 70000 bit/s |

These were measured on a simulated pair with noise, interference and a
clock offset between the two ends, with no bit errors. A short cut of the
line costs one retrain, and error correction resends what was lost. One
end uses about 1.5% of one processor core.

## Still to do

- Moving bits between tones during a call as the noise changes.
- Trellis coding, for a little more speed.
- A test on a real pair with real WB sticks.
