# V.wb: wideband on a bare pair

A phone line carries sound only up to about 3.4 kHz. Because of this, the
maximum speed of dial-up modems is about 56 kbit/s. A **bare pair** of copper wires
does not have this limit. Examples are a pair that you own between two buildings,
an old alarm circuit, or a dry pair that you rent from a phone company. **V.wb** is
the mode of D-Modem for such a pair. It uses frequencies up to 36 kHz and gets
about 149000 bit/s in each direction.

V.wb must have D-Modem at the two ends. It must also have hardware that samples the line at
48 kHz or more: the pico-serial WB stick (refer to
[Softmodem hardware](../hardware.md)), or any sound codec with that speed. V.wb
is not ADSL, and it does not connect to the equipment of a phone company.

For the signal, the start-up and the measurements, refer to
[the technical details](vwb-details.md).

## Using it

At 48 kHz and more, V.wb is the only mode. The line operates as a
[leased line](../leased.md): no dial tone, no ringing.

    AT&L1S0=1        at the answering end
    AT&L1            at the calling end, then ATD

If the connection drops, the modems connect again automatically. Error
correction and compression operate on top, as on all other calls.

## How it works

The signal is divided into many narrow tones, 187.5 Hz apart (this is the same
principle as in ADSL and Wi-Fi). At the start, the modems measure the quality of each
tone. Then they put more bits on clean tones and fewer bits on noisy tones. One
direction uses the lower part of the band, and the other direction uses the upper part.

## Speeds in simulation

| Length of pair | Speed each way |
|---|---|
| 0.5 km | 148656 bit/s |
| 1 km | about 125000 bit/s |
| 3 km | about 100000 bit/s |
| 5 km | about 70000 bit/s |

We measured these speeds on a simulated pair, with noise, interference and a
clock offset between the two ends. There were no bit errors. A short break of the
line causes one retrain, and error correction sends the lost data again. One
end uses about 1.5% of one processor core.

## Still to do

- Move bits between tones during a call, when the noise changes.
- Trellis coding, for a little more speed.
- A test on a real pair with real WB sticks.
