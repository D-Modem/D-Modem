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
| `FAX_*`, `FAX_class1_command` | T.31 class 1 fax (`AT+FCLASS=1`): V.21 HDLC, V.27ter, V.29 and V.17 (see [Fax](#fax-class-1)) |

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
| 32 | ITU V.32, 4800/9600 bit/s | Uncoded 4800, trellis coded 9600, echo canceller |
| 132 | ITU V.32bis, 4800 to 14400 bit/s | Rate chosen from the measured training quality |
| 8, 34, 90, 92 | V.8 and ITU V.34, 2400 to 33600 bit/s | V.8 negotiation, then V.34; hands over to the V.32bis pump (with its V.22bis/V.22 fallback) when the other end does not offer V.34. V.90 and V.92 are not implemented and run as V.34 |

V.90, V.92, K56flex and V.8bis are **not** reimplemented. With the default
`AT+MS=92` the modem connects at V.34 or lower.

The V.22/V.22bis constellation, quadrant coding and phase tables come from
the original library's `SMCv22_*` and `DECv22_*` tables, and the V.32bis
constellations from its `VTBv32_*` tables.

### V.32 / V.32bis

`dp_v32.c` runs the start-up sequence the way the original library does it
(worked out from recordings of it): answer tone, the AA/AC/CA/CC round trip
delay measurement, S, S-bar and TRN (256 two point symbols, then four point
symbols, from the original's fixed scrambler seeds), the R1/R2/R3 rate
words and E. The receivers train an NLMS echo canceller while their own
TRN goes out. Each side offers the rates its training quality supports
(14400 from about 25 dB SNR, 12000 from 22 dB, 9600 from 19 dB, 7200 from
16 dB, else 4800).

### V.8 and V.34

`dp_v34.c` runs the start-up, `v34_modem.c` the primary channel training
signals and receiver, `v34_data.c` the data mode, `v34_tables.c` the
constants (the superconstellation, Table 10 rates, the PP sequence).

- **V.8:** ANSam (2100 Hz, 15 Hz AM, reversals every 450 ms), CM/JM/CJ on
  V.21. A CM or JM without V.34, or a V.32 calling modem (1800 Hz AA),
  hands the call to the V.32bis pump.
- **Phase 2:** INFO0/INFO1 on the 600 baud DPSK control channel, tones A/B
  and their reversals for the round trip delay, and line probing with L1/L2
  (21 tones, 150 Hz apart). The probe gives the SNR after equalization for
  each symbol rate, from the received level of each tone and the noise
  between successive periods (insensitive to clock offsets). The answerer
  picks the symbol rate that allows the highest data rate at both ends.
- **Phases 3 and 4:** S, S-bar, PP, TRN, J, J', MP, E as the original library
  sends them. TRN starts with a cleared scrambler (the original's receiver
  trains on a copy of it). The receiver solves the equalizer by least
  squares over 1500 TRN symbols, since LMS is far too slow for the accuracy
  33600 bit/s needs on a band limited line. Two solutions in phase 3 and one
  in phase 4 measure the sample clock offset from the drift of the
  equalizer's delay. From then on the timing is decision directed: the error
  against the slope of the equalizer output, for any constellation.
- **Data mode:** shell mapping, the superconstellation with expanded
  shaping, differential encoding, the 16 state 4D trellis code, high and
  low mapping frames and the superframe sync inversions, at every rate and
  symbol rate of Table 10 except the auxiliary channel ones. As the original
  library does: the trellis output of a 4D symbol is the low bit of the
  encoder state before that symbol, and the inversion pattern starts at its
  bit 2J - 2. The receiver runs a Viterbi decoder over the 16 states with
  a decision delay of 32 4D symbols (for the equalizer and timing it still
  takes the nearest point). It gains 3 to 4 dB over deciding on the nearest
  points: with it the bit error rate stays below 1e-5 from an SNR of 3 dB per
  bit per symbol plus 4 dB, at every symbol rate, and the rate selection
  asks for 2 dB more than that. Neither side asks for precoding, non-linear encoding or
  pre-emphasis; when the remote asks for them they are not applied.

- **Echo canceller:** real pass band NLMS on the line samples, structured
  like the one in Agere's V.34 (`agrmodemlib.o`, read for reference): a
  near end part over the last 27 ms of our signal and a far end part over
  27 ms around the echo from the other end's hybrid, with one shared error.
  Both train while our own phase 3 sequences go out and the remote is
  silent (the caller waits for the answerer's J to stop), fast, then
  slower. Where the far echo is gets measured: the near part's residual
  is correlated with our scrambled signal at every delay up to 1.7 s (over
  VoIP the echo comes back after the network delay). The tone detectors of
  phase 2 listen exactly on a bin of the 64 sample probe period, where the
  echo of our own L1/L2 has nothing, and the reversal detector does not
  depend on the symbol timing.

Not implemented: precoding, pre-emphasis, the 32 and 64 state trellis
codes, the auxiliary channel, rate renegotiation and retrains.

### Fax (class 1)

`fax_class1.c` implements the T.31 class 1 commands on top of the
modulations in `fax_modems.c`:

| Command | |
|---|---|
| `+FTH=3` / `+FRH=3` | V.21 channel 2 HDLC frames: flags, bit stuffing, CRC-16, `<DLE>` shielding, `OK`/`ERROR` per received frame |
| `+FTM=` / `+FRM=` | 24, 48 (V.27ter), 72, 96 (V.29), 73, 74, 97, 98, 121, 122, 145, 146 (V.17, long and short train) |
| `+FTS=` / `+FRS=` | Silence for / wait for silence of n * 10 ms |

The answering side sends CED (2100 Hz for 3 s) and then starts `+FTH=3` by
itself; the calling side sends CNG while it waits for an answer and starts
`+FRH=3` when the answer tone is heard. Any character from the DTE stops
`+FRH`/`+FRM` once they reported `CONNECT`.

The receivers find the training by matching the received phase steps
against the known sequence. The V.29 and V.27ter training sequences repeat
every 127 symbols, so the end of the alternations before them tells which
repetition is being received. A V.17 short train continues from the
equalizer of the last long train at the same rate.

### How the QAM receiver works

1. Down-conversion to complex baseband.
2. Root raised cosine matched filter, evaluated at the T/2 sampling
   instants themselves (from a finely tabulated pulse, no interpolator),
   then AGC.
3. Gardner timing loop. It tracks about ±300 ppm of clock offset between
   the two ends.
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
    make -C dsp/test fax_interop   # class 1 fax calls, the same way

`dsp_loopback <dp> [snr dB] [seconds] [srate] [line gain] [clock ppm]
[echo dB]` runs a caller and an answerer through a simulated line with
delay, a low-pass filter (`FLAT=1` leaves it out), white noise and an
optional sample clock offset. It then checks the bit error rate in both
directions. `DUMP=<prefix>` records what each side hears.

V.34 between two open modems (`dsp_loopback 8 ...`, with the low-pass
filter, no bit errors): 33600 bit/s at 40 dB (also with ±80 ppm clock
offset, or with near and far echo 6 and 10 dB below our signal), 31200
at 38 dB, 28800 at 35 dB, 26400 at 33 dB, 24000 at 30 dB, 21600 at
28 dB, 16800 at 25 dB, 14400 at 22 dB. `[echo dB] [far echo dB]` add the echoes;
`FAR_DELAY=<samples>` delays the far one (200 ms: still 31200 bit/s).

`interop` connects two `modem_half` processes with pipes. One process is
linked with `dsplibs.o` and the other with this code. Each call goes through
the full modem core: AT commands, V.42/LAPM detection and data in both
directions. Results at the time of writing:

| `AT+MS=` | original caller → open answerer | open caller → original answerer |
|---|---|---|
| 34 (V.34) | CONNECT 33600 | CONNECT 33600 |
| 132 (V.32bis) | CONNECT 14400 | CONNECT 14400 |
| 122 (V.22bis) | CONNECT 2400 | CONNECT 2400 |
| 22 (V.22) | CONNECT 1200 | CONNECT 1200 |
| 212 (Bell 212A) | CONNECT 1200 | CONNECT 1200 |
| 103 (Bell 103) | CONNECT 300 | CONNECT 300 |
| 23 (V.23) | CONNECT 1200 | CONNECT 1200 |
| 21 (V.21) | CONNECT 300 | fails: the original V.21 answerer only sends 2225 Hz and does not connect to itself either |

`fax_interop` does the same with `AT+FCLASS=1`. `fax_interop.py` plays
the T.30 side of both fax machines: CSI/DIS, DCS, TCF, CFR, one page (for
V.17 with the short train), EOP, MCF and DCN. The modems run in real time
so that the script keeps up with the T.30 timers. Results at the time of
writing:

| Modulation | original sender → open receiver | open sender → original receiver |
|---|---|---|
| V.27ter 2400, 4800 | pass | pass |
| V.29 7200, 9600 | pass | pass |
| V.17 7200, 9600 (long and short train) | pass | pass |
| V.17 12000, 14400 | fails: the original transmitter's signal has no usable constellation at these rates (it cannot receive itself either) | pass |

The original library makes a few bit errors with some of its own signals,
so `fax_interop.py` accepts a page with a bit error rate below 1e-3 (a fax
page without ECM survives that). Its V.27ter training also contains stray
180 degree phase flips, which the open receiver follows.

`fax_loopback <mod> [snr dB] [bits] [srate]` sends random data through
one fax modulation over the simulated line. The V.17 short train codes run
a long train first, so the receiver has an equalizer to start from.

The original library in its default V.92 mode does V.8 with automode off,
and connects to this implementation at V.34.

`MODEM_REC=<prefix>` makes `modem_half` record its line input and output
(`<prefix>.in`, `<prefix>.out`), and `DBG=<level>` sets the debug level the
interop scripts run the modems with.
