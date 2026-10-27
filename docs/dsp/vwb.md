# V.wb: wideband DMT on a bare pair

`AT+MS=VWB` (or `AT+MS=200`) runs D-Modem's own wideband mode; on a
line at 48000 Hz and more it is the default, the only mode there. It is
for a **bare copper pair** between two of our modems: a dry pair, an
alarm or leased circuit, or a pair of your own in a building or between
buildings. The line is pico-serial's WB stick (`SPEC-WB.md` in
pico-serial: a codec at 96 kHz, a wideband transformer, protection), or
any codec that samples the pair at 48000 Hz or more. Both ends are ours,
so the modulation follows no Recommendation. It is not ADSL: it does
not reach a carrier's DSLAM, and it does not need megahertz of band.

The code is `dsp/vwb.cpp` (the modem: `VwbModem`) and `dsp/dp_vwb.cpp`
(the pump the core runs, `DP_VWB`). V.42 (LAPM) and V.44 run on top, as
on any other call.

## The signal

Discrete multitone (DMT, OFDM) in frequency-division duplex.

| Line rate | FFT | Cyclic prefix | Symbol | Tones used |
|---|---|---|---|---|
| 48000 Hz | 256 | 20 | 276 samples | 21 to 102 (3.9 to 19.1 kHz) |
| 96000 Hz | 512 | 40 | 552 samples | 21 to 192 (3.9 to 36 kHz) |
| 192000 Hz | 1024 | 80 | 1104 samples | 21 to 192 |

- The tones are 187.5 Hz apart at every rate, and there are 173.9
  symbols a second.
- The band is the WB stick's. Its transformer and DC block cut below
  about 2.7 kHz. Its codec's ADC passes up to 37 kHz at 96 kHz, and
  only 21 kHz at 192 kHz. So 96000 Hz is the stick's rate, and 192000
  Hz is kept for tests and for other hardware.
- The caller sends on the lower part of the band and the answerer on
  the upper part, with a guard of 4 tones between them. The split is
  chosen at training, so that both ways get about the same rate.
- Each tone carries 0 to 12 bits. Its QAM has ceil(b/2) bits on I and
  floor(b/2) on Q, Gray coded on each axis, at unit power. Each tone
  also has a fine gain within ±2.5 dB.
- Each band has 8 pilot tones, which carry the sync symbol's point.
  They keep the receiver's timing.

### Timing and the two clocks

Each receiver resamples its input onto the far end's clock. It uses a
32-tap Kaiser windowed sinc at 256 phases. A type 2 timing loop drives
it, from the slope of the pilots' phase errors across the band (from
every tone while the sync symbols are on). The loop changes the
resampler's rate only, never its phase in steps. So a correction never
puts a step into a symbol, and the receiver's own echo stays clean.

The answerer is **loop timed**:

1. It sends on the caller's clock: its transmitter goes through the
   same mapping as its receiver.
2. It lines its symbols up with the caller's as they reach its own
   receiver. It measures the delay of its own echo while it sends its
   full-band sync.

Its echo therefore lands on the same symbol boundaries as what it
receives, and in its own band it is orthogonal to the far signal. At
the caller, the two are apart by twice the pair's delay. The caller
measures that offset (its own echo's delay comes from its full-band
sync). The plan then moves the answerer's symbols by half of it, so
that at both ends the echo is the same few samples ahead.

The FFT window starts 20 samples (at 96 kHz) before the end of the
cyclic prefix. That leaves 20 samples for the pair's dispersion and 20
for the echo's offset.

The hybrid's leak of each side's own signal (10 to 20 dB down on the
stick, -15 dB in the tests) therefore costs nothing measurable. The
clock offset is tracked whatever it is. The tests run ±100 ppm (the
stick's crystals are within ±50 ppm), and the receivers report the
offset they track.

## The start-up

| Step | Caller | Answerer |
|---|---|---|
| 1 | probe: tone 24 (4.5 kHz) | listens for it (20 dB over the other tones for 6 symbols) |
| 2 | finds the sync, goes quiet; measures the timing, the clock, H and the SNR of every tone | sync symbol on the whole band, 160 symbols, then 2 segues (the symbol negated); measures its echo's delay |
| 3 | sync on the whole band, 160 symbols, then 2 segues; measures its echo's delay | finds it (looking only once its own echo has gone by), measures the same, then locks its transmitter to the caller's clock |
| 4 | idle (the sync symbol on its message band); looks for the answerer's sync once its own echo has gone by | sync on its message band, 80 symbols, 2 segues, then frames carrying its SNR table |
| 5 | finds the answerer's sync; on the SNR frame: the plan, then 2 segues and frames carrying the plan | |
| 6 | | on the plan: GO frames (2, 1), then the plan's sync, 24 symbols, then data |
| 7 | on the answerer's GO: its own GOs (2, 1), the plan's sync, then data | |

The timeline is from `vwb_test`'s logs at 96 kHz:

- the probe is heard after 35 ms;
- both full-band syncs are done after 2.1 s;
- the plan is made after 2.8 s;
- data runs both ways after 5.0 s.

**The echo's tail.** Each side hears its own full-band sync through the
hybrid, as long after sending it as the host's buffers and the pair
take: a USB or ALSA line holds tens of milliseconds each way (dmodemd's
USB line sends its samples about 50 ms ahead). Heard as the far end's,
it would be locked onto. So each side looks for the far end's next sync
only 32 symbols (184 ms) after its own has ended: the answerer for the
caller's full-band sync, the caller for the answerer's sync on its
message band, which is 80 symbols long (48 and those 32) so that it is
still there to be found and trained on.

**The message channel** is QPSK on the lowest 30% of the tones (the
caller's) and the highest 30% (the answerer's), with 4 pilots each. The
plan's split always falls between these two bands. So while one side
has switched to the plan and the other has not, neither side's echo
lands on tones it receives.

- Each bit goes on 2 tones (the caller's) or 4 (the answerer's). The
  receiver adds the copies, each weighed by its tone's measured SNR.
- A frame is a type octet, a length (2 octets), the payload and a
  CRC-32. It is whitened by a PRBS (x^15 + x^14 + 1); a frame of zeros
  would otherwise peak and clip. It fills a whole number of symbols,
  the same number for every frame of one direction.
- Frames repeat until the far end answers them.
- The answerer's SNR frame is one octet a tone (dB × 2).
- The caller's plan frame holds:
  - the split;
  - the guard;
  - the answerer's shift (2 octets);
  - a code for each tone: its bits, 15 for a pilot, 0 unused;
  - each tone's gain (128 + 0.1 dB steps).

**The plan:**

1. Each tone's bits are the most whose SNR need is met with a 4 dB
   margin. A b-bit QAM needs q²P/2, with q = 4.75 and P the
   constellation's power at unit spacing.
2. The split is the one that maximizes the slower direction's bits.
3. Each band gets 8 pilots, each on the best tone near its place.
4. Gains equalize the margins within ±2.5 dB, and the band's power is
   kept.

## Data

1. The core's octets go through the self-synchronizing scrambler
   1 + x^-18 + x^-23 (V.34's GPC, `bits.h`).
2. Then RS(255,239) (`rs.cpp`: 8 octets corrected in each codeword).
3. Then a block interleaver of D codewords. D is as many codewords as
   30 ms of the line hold, from 1 to 8: 2 at 150 kbit/s.
4. The bits go onto the tones in tone order, least significant first.

The reported rate is the octets' rate: the bits of a symbol, times
239/255, times 173.9.

The interleaver is shallow on purpose. V.42's window (15 frames of 128
octets) waits a round trip for its acknowledgement, and a deeper
interleaver's delay cut LAPM to 53 kbit/s on a 149 kbit/s pump in the
tests. At D = 2 it gets 138 kbit/s.

The receiver follows the channel on its decisions (LMS, 0.01) and on
the pilots. It **retrains** (the start-up again; `ATI11` counts it, and
the core keeps the call) when:

- the pilots are lost for 8 symbols;
- half of the last 32 codewords fail;
- a start-up phase times out.

If there is no data again within S7 seconds, the pump gives up (NO
CARRIER).

## Leased lines: AT&L1

A bare pair has no exchange: no dial tone, ringing or hook. At 48000 Hz
and above the core never runs call progress, and `AT&L1` (S47) does the
same on any line:

- `ATD` (any number, or none) starts the data pump as the caller at
  once.
- `ATA` starts it as the answerer. The answerer waits for the caller's
  probe for S7 seconds.
- `ATS0=1` (any S0 but 0) answers with no ring as soon as the modem is
  idle. Each time a call ends, it waits for the caller's probe again.
- When the line ends a call (NO CARRIER: no data again within S7), the
  modem starts it again by itself 2 s later. The caller dials again,
  and the answerer waits for the probe again.

`ATH`, DTR, `ATZ` or `AT&L0` stop this until the next command line, so
that a command can be typed between the attempts.

## Rates measured in simulation

Both ends run on the simulated pair of `dsp/test/vwb_line.cpp` at 96 kHz:

- the cable is 24 AWG with a loss of k·sqrt(f)·length (k = 0.025
  dB/km/sqrt(Hz), about 8 dB/km at 100 kHz), its skin-effect phase,
  and 5 µs/km of delay;
- each end's transformer is a high pass at 2.7 kHz;
- white noise at the receivers;
- -15 dB of each side's own signal at its receiver (the hybrid);
- the answerer's clock 100 ppm fast (resampled to and from the line);
- 5 ms of buffering each way.

All rates are octets after RS, each way, both ways at once. Every case
ran with no bit error.

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

148656 bit/s is the most there is at 96 kHz: 76 data tones a way at 12
bits. More would need tones above 36 kHz (the stick's ADC passes 37
kHz) or more bits a tone. That is about 300 kbit/s both ways together,
as SPEC-WB expected.

The other results:

- **Through V.42.** Under libdmodem (`vwb_call_test`), 400 kB each way
  at once, byte for byte, went at 138558 bit/s each way through V.42 on
  the 148656 bit/s pump.
- **Through a host's buffers.** With each end's samples 4672 ahead and
  in 20 ms frames, as dmodemd's USB line has them (`vwb_call_test`'s
  HostLatency), the call connects as before; V.42 then carries 83 kbit/s
  each way (LAPM's window waits a longer round trip). dmodemd itself
  runs the call between two fake WB sticks on this pair in real time
  (`dmodemd/test/pico_wb_test.py`, [hardware.md](../hardware.md#pico-serial-wb-wideband-bare-pair)).
- **Dropout.** A 0.5 s cut of the pair costs one retrain. The data goes
  on after it, and LAPM sends again what was lost.
- **CPU.** One end takes about 1.5% of one x86 core at 96 kHz.

## Left to do

- Bit swapping in a call (a tone's bits moved as its noise changes).
- Trellis coding.
- V.42's extended windows, which a long interleaver, and a host's
  buffers, would need.
- A real pair, and a real WB stick (dmodemd's `--usb` drives it:
  [hardware.md](../hardware.md#pico-serial-wb-wideband-bare-pair)).
