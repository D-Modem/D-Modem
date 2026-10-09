# Open data pumps for slmodemd

This directory is a C reimplementation of the interface that the binary-only
`dsplibs.o` (Smart Link's DSP library) gives to the slmodemd modem core. It
exports the same entry points, so the rest of slmodemd and D-Modem build
against it unchanged:

| Entry point | What it does here |
|---|---|
| `prop_dp_init` / `prop_dp_exit` | Register / deregister the data pumps below |
| `dcr_create/process/reset/delete` | DC removal. Same algorithm and constants as the original (skip 0.6 s, average 1 s, then 0.9/0.1 smoothing every 2 s) |
| `dp_runtime_create/delete` | State shared by the pumps of one call (for example, which answer tone call progress heard) |
| `RD_*` | Ring detector for snooping codecs (low frequency, high amplitude bursts) |
| `CID_*` | Caller ID: Bell 202 FSK, SDMF and MDMF, formatted (`AT+VCID=1`) or raw hex |
| `VOICE_*` | IS-101 style voice: 8 bit linear at 8000 Hz with `<DLE>` shielding, `+VTS` tones and DTMF |
| `FAX_*`, `FAX_class1_command` | Stubs. Fax is **not** implemented |

## Data pumps

| DP id (`AT+MS=`) | Modulation | Notes |
|---|---|---|
| `DP_CALLPROG` | Call progress | Dial tone (X2/X4), DTMF dialing, V.25 calling tone, busy cadence, answer tone (2100 Hz / 2225 Hz / V.22 unscrambled ones), no-answer timeout |
| 21 | ITU V.21, 300 bit/s FSK | |
| 23 | ITU V.23, 1200/75 bit/s FSK | The answering side sends the 1200 bit/s channel |
| 103 | Bell 103, 300 bit/s FSK | |
| 22 | ITU V.22, 1200 bit/s DPSK | |
| 212 | Bell 212A, 1200 bit/s DPSK | 2225 Hz answer tone, scrambled ones handshake |
| 122 | ITU V.22bis, 2400 bit/s 16-QAM | Full S1 handshake, falls back to V.22 |
| 8, 32, 132 | (V.8, V.32, V.32bis) | Not implemented. These IDs map to V.22bis so the default `AT+MS=92` setting (the core starts the V.8 pump for it) still connects |

V.32bis, V.34, V.90, V.92, K56flex, V.8/V.8bis and fax modulations
(V.27ter, V.29, V.17) are **not** reimplemented. `AT+MS=34`, `90` and `92`
are refused with `ERROR` because no pump is registered for them. With the
default `AT+MS=92`, the modem connects at V.22bis or lower.

The V.22/V.22bis constellation, quadrant coding and phase tables come from
the original library's `SMCv22_*` and `DECv22_*` tables.

### How the QAM receiver works

1. Band-pass filter, then down-conversion to complex baseband.
2. Root raised cosine matched filter (alpha 0.75), then AGC.
3. Cubic interpolator driven by a Gardner timing loop. It tracks about
   ±300 ppm of clock offset between the two ends.
4. T/2 spaced normalized LMS equalizer.
5. Decision-directed carrier PLL.

The equalizer and the PLL's frequency term only train on scrambled
signals. The unscrambled handshake sequences (ANS, unscrambled ones, S1)
are close to pure tones and would de-tune them.

## Building

    make DSPLIBS=open          # in slmodemd/ or at the top level

The default is still `DSPLIBS=blob`, which links the original `dsplibs.o`.
The open build has no i386 requirement. It builds natively on x86_64,
ARM and other architectures, and links with `-lm`.

## Tests

    make -C dsp/test check     # back-to-back pumps through a simulated line
    make -C dsp/test interop   # calls between the original library and this one (needs gcc -m32)

`dsp_loopback <dp> [snr dB] [seconds] [srate] [line gain] [clock ppm]` runs a
caller and an answerer through a simulated line with delay, a low-pass
filter, white noise and an optional sample clock offset. It then checks the
bit error rate in both directions.

`interop` connects two `modem_half` processes with pipes. One process is
linked with `dsplibs.o` and the other with this code. Each call goes through
the full modem core: AT commands, V.42/LAPM detection and data in both
directions. Results at the time of writing:

| `AT+MS=` | original caller → open answerer | open caller → original answerer |
|---|---|---|
| 122 (V.22bis) | CONNECT 2400 | CONNECT 2400 |
| 22 (V.22) | CONNECT 1200 | CONNECT 1200 |
| 212 (Bell 212A) | CONNECT 1200 | CONNECT 1200 |
| 103 (Bell 103) | CONNECT 300 | CONNECT 300 |
| 23 (V.23) | CONNECT 1200 | CONNECT 1200 |
| 21 (V.21) | CONNECT 300 | fails: the original V.21 answerer only sends 2225 Hz and does not connect to itself either |

Known limitation: the original library in its default V.92 mode does
V.8 with automode off. Its CM message offers only V.34, V.90 and V.32bis,
not V.22bis, so it cannot connect to this implementation. Set `AT+MS=122`
on that side.
