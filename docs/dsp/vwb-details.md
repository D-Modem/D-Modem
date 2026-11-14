# V.wb: wideband DMT on a bare pair: technical details

This page gives the technical details for [V.wb: wideband on a bare pair](vwb.md), the plain-English overview. Read the overview first. This page has the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

`AT+MS=VWB` (or `AT+MS=200`) starts the own wideband mode of D-Modem. On
a line sampled at 48000 Hz or more, it is the default. It is also the only
mode that the core operates on such a line.

It is for a **bare copper pair** between two D-Modems: a dry pair, an
alarm or leased circuit, or your own pair in a building or between
buildings. The line hardware is the WB stick of pico-serial (`SPEC-WB.md`
in pico-serial: a codec at 96 kHz, a wideband transformer, protection),
or a different codec that samples the pair at 48000 Hz or more.

The two ends are D-Modem. Thus, the modulation follows no Recommendation.
It is not ADSL. It does not communicate with the DSLAM of a carrier, and
it does not need megahertz of band.

The code is in two files:

- `dsp/vwb.cpp`: the modem, `VwbModem`.
- `dsp/dp_vwb.cpp`: the pump that the core operates, `DP_VWB`. It does
  not start below 48000 Hz.

V.42 (LAPM) and V.44 operate on it, as on all other calls.

## The signal

The signal is discrete multitone (DMT, the same principle as OFDM) in
frequency-division duplex. It has many narrow tones, each with its own
QAM. Each direction has its own part of the band.

| Line rate | FFT | Cyclic prefix | Symbol | Tones used |
|---|---|---|---|---|
| 48000 Hz | 256 | 20 | 276 samples | 21 to 102 (3.9 to 19.1 kHz) |
| 96000 Hz | 512 | 40 | 552 samples | 21 to 192 (3.9 to 36 kHz) |
| 192000 Hz | 1024 | 80 | 1104 samples | 21 to 192 |

- The tones are 187.5 Hz apart at all rates, and there are 173.9 symbols
  each second.
- The band is that of the WB stick. Its transformer and DC block remove
  the signal below about 2.7 kHz. The ADC of its codec passes up to
  37 kHz at 96 kHz, and only 21 kHz at 192 kHz. Thus, 96000 Hz is the
  rate of the stick. We keep 192000 Hz for tests and for other hardware.
- The caller sends on the lower part of the band, and the answerer sends
  on the upper part. There is a guard of 4 tones between them. The modems
  select the split at training. Thus, the two directions get about the
  same rate.
- Each tone sends 0 to 12 bits. Its QAM has ceil(b/2) bits on I and
  floor(b/2) on Q, Gray coded on each axis, at unit power. Each tone also
  has a fine gain in the range ±2.5 dB.
- In data, each band has 8 pilot tones, which send the point of the sync
  symbol. They keep the timing of the receiver.

### Timing and the two clocks

Each receiver resamples its input onto the clock of the far end, with a
32-tap Kaiser windowed sinc at 256 phases (`VwbInterp`).

A type 2 timing loop controls the resampler. Its error is the slope of
the phase errors of the pilots across the band (or of all tones, while
the sync symbols are on). The loop changes only the rate of the
resampler. It never changes its phase in steps. Thus, a correction never
puts a step into a symbol, and the echo of the receiver stays clean.

The answerer is **loop timed**:

1. It sends on the clock of the caller. Its transmitter goes through the
   same mapping as its receiver.
2. It aligns its symbols with those of the caller as they get to its own
   receiver. It measures the delay of its own echo while it sends its
   full-band sync.

Thus, its echo falls on the same symbol boundaries as the signal that it
receives. In its own band, the echo is orthogonal to the far signal. At
the caller, the two are apart by two times the delay of the pair. The
caller measures that offset (the delay of its own echo comes from its
full-band sync). Then the plan moves the symbols of the answerer by half
of it. Thus, at the two ends, the echo is the same few samples ahead.

The FFT window starts half of the cyclic prefix (20 samples at 96 kHz)
before the end of the prefix. This leaves 20 samples for the dispersion
of the pair and 20 for the offset of the echo.

Thus, the hybrid leak of the own signal of each side (10 to 20 dB down on
the stick, -15 dB in the tests) has no measurable effect. The receiver
tracks the clock offset, whatever its value. The tests use ±100 ppm (the
crystals of the stick are in the range ±50 ppm). The receivers give the
offset that they track.

## The start-up

| Step | Caller | Answerer |
|---|---|---|
| 1 | probe: tone 24 (4.5 kHz) | listens for it (20 dB over the other tones for 6 symbols) |
| 2 | finds the sync, becomes quiet; measures the timing, the clock, H and the SNR of each tone | sync symbol on the full band, 160 symbols, then 2 segues (the symbol negated); measures the delay of its echo |
| 3 | sync on the full band, 160 symbols, then 2 segues; measures the delay of its echo | finds it (it looks only after its own echo has gone by), measures the same, then locks its transmitter to the clock of the caller |
| 4 | idle (the sync symbol on its message band); looks for the sync of the answerer after its own echo has gone by | sync on its message band, 80 symbols, 2 segues, then frames with its SNR table |
| 5 | finds the sync of the answerer; on the SNR frame: the plan, then 2 segues and frames with the plan | |
| 6 | | on the plan: GO frames (2, 1), then the sync of the plan, 24 symbols, then data |
| 7 | on the GO of the answerer: its own GOs (2, 1), the sync of the plan, then data | |

The timeline is from the logs of `vwb_test` at 96 kHz:

- The answerer hears the probe after 35 ms.
- The two full-band syncs are complete after 2.1 s.
- The plan is complete after 2.8 s.
- Data moves in the two directions after 5.0 s.

**The tail of the echo.** Each side hears its own full-band sync through
the hybrid, with the delay of the buffers of the host and the pair. A USB
or ALSA line holds tens of milliseconds in each direction (the USB line of
dmodemd sends its samples about 50 ms ahead). If the receiver identified
that echo as the sync of the far end, it would lock onto it.

Thus, each side looks for the next sync of the far end only 32 symbols
(184 ms) after its own sync has ended:

- The answerer looks for the full-band sync of the caller.
- The caller looks for the sync of the answerer on its message band. That
  sync is 80 symbols long (48, plus those 32). Thus, it is still there
  for the caller to find and train on.

**The message channel** is QPSK on the lowest 30% of the tones (for the
caller) and the highest 30% (for the answerer), with a minimum of 16
tones each, and 4 pilots each. The split of the plan is always between
these two bands. Thus, while one side uses the plan and the other side
does not, the echo of neither side falls on tones that it receives.

- Each bit goes on 2 tones (for the caller) or 4 (for the answerer). The
  receiver adds the copies. It weighs each copy by the measured SNR of
  its tone.
- A frame is a type octet, a length (2 octets), the payload and a CRC-32.
  A PRBS (x^15 + x^14 + 1) whitens it. If not, a frame of zeros would
  peak and clip. It fills a whole number of symbols, the same number for
  all frames of one direction.
- Frames repeat until the far end answers them.
- The SNR frame of the answerer has one octet for each tone (dB × 2).
- The plan frame of the caller holds:
  - The split (as an offset from the first tone of the band).
  - The guard.
  - The shift of the answerer (2 octets).
  - A code for each tone: its bits, 15 for a pilot, 0 unused.
  - The gain of each tone (128 + 0.1 dB steps).

**The plan:**

1. The bits of each tone are the maximum for which the SNR is sufficient
   with a 4 dB margin. A b-bit QAM needs q²P/2, with q = 4.75 and P the
   power of the constellation at unit spacing.
2. The split is the split that gives the most bits to the slower
   direction.
3. Each band gets 8 pilots, each on the best tone near its position.
4. Gains make the margins equal in the range ±2.5 dB. The power of the
   band does not change.

## Data

1. The octets of the core go through the self-synchronizing scrambler
   1 + x^-18 + x^-23 (the GPC of V.34, `bits.h`).
2. Then RS(255,239) (`rs.cpp`: 8 octets corrected in each codeword).
3. Then a block interleaver of D codewords. D is the number of codewords
   that 30 ms of the line hold, from 1 to 8: 2 at 150 kbit/s.
4. The bits go onto the tones in tone order, least significant first.

The rate that the modem gives is the rate of the octets: the bits of a
symbol, times 239/255, times 173.9.

The interleaver is shallow intentionally. The window of V.42 (15 frames
of 128 octets) waits a round trip for its acknowledgement. In the tests,
the delay of a deeper interleaver decreased LAPM to 53 kbit/s on a
149 kbit/s pump. At D = 2, it gets 138 kbit/s.

The receiver follows the channel on its decisions (LMS, 0.01) and on the
pilots. It **retrains** (it does the start-up again; `ATI11` counts it,
and the core keeps the call) in these conditions:

- The pilots are lost for 8 symbols.
- Half of the last 32 codewords fail.
- A start-up phase times out.

If there is no data again in S7 seconds, the pump stops (NO CARRIER).

## Leased lines: AT&L1

A bare pair has no exchange: no dial tone, no ring signal, no hook. At
48000 Hz and higher, the core operates the line as a line without an
exchange, with or without `AT&L1`:

- It never does call progress.
- `ATD` (with a number or with no number) starts the data pump as the
  caller immediately.
- `ATA` starts it as the answerer, which waits for the probe of the
  caller for S7 seconds.

`AT&L1` (S47=1, the leased line setting; the leased lines of the voice
band are in [leased.md](../leased.md)) adds the behaviour of a permanent
connection:

- `ATS0=1` (all values of S0 except 0) answers with no ring signal
  immediately when the modem is idle. Each time a call ends, it waits for
  the probe of the caller again.
- When the line ends a call (NO CARRIER: no data again in S7), the modem
  starts it again automatically 2 s later. The caller dials again, and
  the answerer waits for the probe again.

`ATH`, a drop of DTR, `ATZ` or `AT&L0` stop this until the next command
line. Thus, you can type a command between the attempts.

## Rates measured in simulation

The two ends operate on the simulated pair of `dsp/test/vwb_line.cpp` at
96 kHz:

- The cable is 24 AWG with a loss of k·sqrt(f)·length (k = 0.025
  dB/km/sqrt(Hz), about 8 dB/km at 100 kHz), its skin-effect phase, and
  5 µs/km of delay.
- The transformer of each end is a high pass at 2.7 kHz.
- White noise at the receivers.
- -15 dB of the own signal of each side at its receiver (the hybrid).
- The clock of the answerer is 100 ppm fast (resampled to and from the
  line).
- About 5.3 ms of latency in each direction (the converters and buffers
  of the two sides), in blocks of a maximum of 5 ms.

All rates are octets after RS, in each direction, with the two directions
at the same time. All cases had no bit errors.

| Pair | Noise (below a tone as sent) | Caller → answerer | Answerer → caller |
|---|---|---|---|
| 0.5 km | 60 dB | 148656 bit/s (12 bits on every tone) | 148656 |
| 0.5 km | 50 dB | 134964 | 135942 |
| 1 km | 50 dB | 125347 | 128118 |
| 3 km | 50 dB | 99430 | 100082 |
| 5 km | 50 dB | 70090 | 72698 |
| 1 km, interferers at 9.3 kHz (-40 dB) and 27.2 kHz (-35 dB) | 60 dB | 119479 | 121435 |
| 1 km, ±100 ppm, 2 minutes | 60 dB | 148656 | 148656 |
| 1 km at 48000 Hz | 60 dB | 60636 | 60636 |
| 1 km at 192000 Hz | 60 dB | 148656 | 148656 |

148656 bit/s is the maximum at 96 kHz: 76 data tones in each direction at
12 bits. For more, tones above 36 kHz (the ADC of the stick passes
37 kHz) or more bits on each tone are necessary. This is about 300 kbit/s
for the two directions together, as SPEC-WB expected.

Other results:

- **Through V.42.** Under libdmodem (`vwb_call_test`), 400 kB in each
  direction at the same time, byte for byte, went at 138558 bit/s in each
  direction through V.42 on the 148656 bit/s pump.
- **Through the buffers of a host.** The samples of each end were 4672
  ahead and in 20 ms frames, as on the USB line of dmodemd (the
  HostLatency test of `vwb_call_test`). The call connects as before. V.42
  then sends 83 kbit/s in each direction (the window of LAPM waits a
  longer round trip). dmodemd itself operates the call between two fake
  WB sticks on this pair in real time
  (`frontend/dmodemd/test/pico_wb_test.py`, [hardware.md](../hardware-details.md#pico-serial-wb-wideband-bare-pair)).
- **Dropout.** A 0.5 s cut of the pair causes one retrain. The data
  continues after it, and LAPM sends again the data that was lost.
- **CPU.** One end uses about 1.5% of one x86 core at 96 kHz.

## Left to do

- Bit swapping in a call (the bits of a tone move when its noise
  changes).
- Trellis coding.
- The extended windows of V.42, which a long interleaver and the buffers
  of a host would need.
- A real pair, and a real WB stick (the `--usb` of dmodemd operates it:
  [hardware.md](../hardware-details.md#pico-serial-wb-wideband-bare-pair)).
