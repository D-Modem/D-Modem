# Building and testing the DSP

## Building

    cmake -B build                            # in the top directory
    cmake --build build

There is no i386 requirement. It builds natively on x86_64, ARM and other
architectures, and links with `-lm`. Only the interop tests need a 32 bit
compiler (`g++ -m32`), for the original library.

## Tests

    ctest --test-dir build -j16         # all of them (some four times the processors:
                                        # about 1.5 min on 4)
    ctest --test-dir build -L quick     # back-to-back pumps, libdmodem, fax
    ctest --test-dir build -L spandsp   # against spandsp (libspandsp-dev)
    ctest --test-dir build -L smartlink -j16  # calls between Smart Link's soft modem (its
                                              # original library) and this one, data and
                                              # class 1 fax (needs g++ -m32)
    ctest --test-dir build -L agere -j16    # calls between the Agere soft modem and this one
                                            # (sm56, pctel, hsf, broadcom, intel, spanpeer: the other modems;
                                            # open: two of this one)

The test programs are in `dsp/test/` (the link layer's and the core's in `link/test/` and `core/test/`). All the tests but one are GoogleTest programs (the system's libgtest-dev, or
fetched while configuring); ctest runs each of their cases as a test of
its own, named `<suite>/<Test>.<case>/<parameters>`, e.g.
`Line/PumpLoopback.DataBothWays/v34_echoes`, or for the calls
`<label>.<call>`, e.g. `agere.agere_v92_qc`. Run one by itself with
`ctest -R v34_echoes`, or the program with `--gtest_filter`:

| Program | What |
|---|---|
| `pump_test` | the data pumps back to back (`loopback.cpp`): each modulation on clean and noisy lines, 8000 Hz, clock offsets, gains, echoes; the answering automode. A side passes connected, with 90% of the bits its rate carries from the CONNECT on, at a BER of 1e-3 at most |
| `dmodem_test` | libdmodem: pairs of its modems, retrains and renegotiations in the data, V.91 across G.711 networks, the network's law for each call, `dmodem.hpp` |
| `dmodem_fax_test` | libdmodem's fax: a data modem answering hands the call to a fax receiver; a fax over T.38 through the API, in UDPTL with packets lost, and in IFP packets (`t38_ifp`, as Asterisk's channels have them) no longer than 72 octets |
| `hw_test` | the hardware the harnesses emulate (`hw/`), on a host of its own: the ICH's bus masters |
| `bits_test`, `g711_test`, `pcm_cp_test` | `bits.h`'s fields and CRCs, `g711.h`'s two laws (each octet back to itself, sign and magnitude), the PCM encoders on CPs that cannot carry their rate |
| `fax_test` | the class 1 modulations, T.4/TIFF (also against libtiff's `tiffcp`), two T.30 machines back to back |
| `t38_test` | T.38: the IFP packets' encodings (both versions), UDPTL's redundancy, two T.30 machines over T.38 with packets lost |
| `class1_t38_test` | class 1 over T.38: a DTE of the test's own through T.30 on the engine's commands against our T.30 machine over T.38, sending and receiving, ECM, both packet versions, T.38 after a while on the audio; the packets' fields and times |
| `spandsp_test` | spandsp's V.17 transmitter and T.4 encoder into ours, our T.30 against spandsp's, on the audio and over T.38 (its T.38 terminal) |
| `../../link/link_test`, `../../core/test/core_test`, `core_equiv` | the data link layer, the core, the core's transcripts against the scripts' `.expect` (slmodemd's old core's, recorded from it; see `docs/core.md`) |
| `smartlink_test`, `agere_test`, `sm56_test`, `pctel_test`, `hsf_test`, `broadcom_test`, `intel_test`, `spanpeer_test`, `open_test` | calls between this modem and the others ([interop.md](interop.md); `interop_test.cpp`: one table of the calls, each program a label's) |
| `call_tone_test`, `ring_detect_test` | the call progress tones a voice dial hears (ringing, busy, answer tones) and the ring signal a modem codec hears on hook (`dsp.h`'s `CallToneDetect`, `RingDetect`) |
| `dmodemd/test/alsa_test.py` (`dmodemd_alsa`, label `alsa`) | two dmodemd on fake ALSA modem cards (alsa-lib plugins), calling each other through a simulated exchange: dial tone, DTMF, rings, caller ID, V.34, busy, a stall, a voice call ([hardware.md](hardware.md)). Needs libasound2-dev |
| `dmodemd/test/root_test.py` (`dmodemd_root`) | dmodemd started as root: it goes on as nobody, saves its data file, and its pty's group and link (kept by a process of its own) follow the new pty after the DTE hangs up and go when it ends, even killed. Skipped unless ctest runs as root (CI runs it with sudo) |

## Test logs

A ctest run with a failure keeps the logs of the tests that failed, in
a directory of its own named for when it started, under
`build/test-logs/` (`latest` is the newest); a run without one (skipped
tests are not failures) leaves nothing there:

    build/test-logs/2026-10-16_21-34-03/
        run.txt                     when, the ctest command, the commit, the host
        results.txt                 each test as it ends: PASS, FAIL or SKIP, its exit
                                    status and seconds
        summary.txt                 the counts, then results.txt, failures first
        open.open_v90a_ulaw/        a test that failed (or was skipped, or killed)
            output.log              what it printed (ctest's output of it)
            info.txt                its command, start, debug levels, environment
            result.txt              its line of results.txt
            modemA.log, modemB.log  a call's modem logs (dmodemd_alsa: exchange.log,
                                    dmodemd-<line>.log)
        open.open_v90a_ulaw.2/      the same test again in the run (ctest --repeat)

This is plain ctest, with `-R`, `-L` or `--repeat` as well: a fixture
does it (`cmake/test_logs.cmake`). `test_logs_begin` makes the
directory before the first test and `test_logs_end` writes the summary
after the last, and every test runs under `cmake/test_run.sh`, which
keeps its output and passes it on to ctest. A test finds its directory
in `DMODEM_TEST_DIR`. When the test passes, the launcher deletes its
directory at once, and when the run ends without a failure
`test_logs_end` deletes the run's directory and points `latest` at the
newest run left (`DMODEM_TEST_KEEP_PASSED=1` keeps all of it: some 100
MB for a full run); a test without its `result.txt` was killed, at its
ctest timeout say, and counts as failed. The 20 latest runs stay
(`-DDMODEM_TEST_LOG_KEEP=<n>`; `-DDMODEM_TEST_LOG_ROOT=<dir>` puts them
elsewhere).

Every run logs verbosely: the launcher sets the debug level of every
modem and library the tests run to `DMODEM_TEST_DEBUG` (5 by default),
wherever it is not already set: `DSP_DEBUG` (the pumps), `CORE_DEBUG`,
`DMODEM_DEBUG` (libdmodem), `DBG` (the interop calls' modems),
`DMODEMD_DEBUG` (dmodemd's `-d`), and `LINK_DEBUG` and `SPANDSP_DEBUG`
on. Level 5 is what the modems log of each step of a call, with their
equalizers' and retrains' figures as they go; 6 and up log every bit
and symbol (`DSP_DEBUG=6 ctest -R ...` for one test).
`DMODEM_TEST_DEBUG=0` leaves the tests' own levels. With all of this a
full run takes about a tenth longer.

A call run by hand (`interop_call`, or a test program outside ctest)
writes its modem logs to `LOG_DIR`, or `build/dsp/test/logs/<test>/`
for the test programs. The
calls run as fast as the modems can: every modem keeps time by its
line's samples (the Agere, SM56, PCTel, HSF and Broadcom harnesses count
jiffies and timers in samples, and our core, under the open pumps or
the original library, does with `PIPE_SYNC`), so a call goes as it would in real
time, only sooner, and a busy machine only slows it down. The call's own
seconds (`STREAM`, `MIDCALL`, `WAIT`, `ANSWER_AFTER`, `HANGUP_WAIT`, and
how long it waits for what the modems say) are then the line's, from
netsim's clock (`NET_CLOCK_FD`), faster or slower than the wall clock
as the machine goes; the wall clock's only while the line stands still
(a modem gone). Three things keep such a call the same from run to run,
however late the driver answers:

- on hook our modem and the original library's run in real time anyway
  (`PIPE_SYNC`), so that the driver's commands meet the samples they
  would in real time (the call itself, from the dial or the answer, as
  fast as it goes); and their line runs from the start, as the other
  harnesses' does (the lead silence written when they start, not at
  the first ATD or ATA): a line that stood still until then took the
  commands as they came, and the call began wherever the scheduler had
  the two modems at that moment;
- the driver waits 100 ms of the line after a CONNECT before its data (the
  SM56 says CONNECT a little ahead of taking data, and echoes what
  comes sooner as a command line);
- the data that comes around a call's mid-call commands (`MIDCALL`) is
  kept: before the escape's OK and right after `ATO`'s CONNECT.

The fax calls (`interop_call --fax`) keep time by the driver's own relay
of the line (A's samples at `LINE_RATE`: 8000, 9600 with the original
library), and go ten times as fast as real time at most
(`MODEM_SPEED=10`, `half_open`'s and `half_smartlink`'s): the fax modems
take so little that the line would otherwise go a few hundred times as fast,
and the milliseconds the driver takes to answer what a modem said (it
plays T.30 for both machines) would be most of a second of the line,
past T.30's turnarounds. The modems take the DTE's commands every 192
samples (20 ms at 9600 Hz, the original library's frame): its V.17
receiver takes a short train or not depending on how its frames fall on
it, so our training has to start at the same point of them every time.
So they take the turnarounds the paced calls did and give the same TCF
and pages, in a tenth of the time. The relay holds the line while the
driver has not answered what a modem said (from the modem's output
until the driver's next poll or write): however late the scheduler lets
it run, its answer meets the line where it would at once (the Agere
modem's `+FTH` ended in ERROR now and then, its frame some seconds of
the line after its CONNECT).

The calls run a little nicer (`nice` 10) than the rest of the machine.
Each takes about one processor while it runs, but for the moments on
hook in real time, so `-j` can be some four times the processors: on 4,
the whole suite takes some 75 s at `-j16` (105 at `-j8`; 13 minutes when
every call ran in real time). Much more than that (`-j32` on 4) leaves
the calls on hook short of real time. None of the calls has to run
alone. `MODEM_REALTIME=1 ctest ...` runs every call in real
time, as they all did before (for comparing a failure).

The test programs are in `build/dsp/test/`. The calls go through
`interop_call.cpp`; `interop_call` makes one by hand:

    interop_call <caller> <answerer> ["ATX3;AT+MS=122"]
    interop_call --fax <sender> <receiver> [mod] [AT setup]

with the call's settings in the environment (`interop_call.h`: `NET`,
`STREAM`, `CALLS`, `EXPECT_B`, ...; `NETSIM` for netsim and `LOG_DIR`
for the logs, by default the current directory).

`dsp_loopback <dp> [snr dB] [seconds] [srate] [line gain] [clock ppm]
[echo dB]` runs a caller and an answerer through a simulated line with
delay, a low-pass filter (`FLAT=1` leaves it out), white noise and an
optional sample clock offset. It then checks the bit error rate in both
directions. `DUMP=<prefix>` records what each side hears. It is the
simulation of `pump_test`, for trying a line from the command line.

V.34 between two open modems (`dsp_loopback 8 ...`, with the low-pass
filter, no bit errors): 33600 bit/s at 40 dB (also with near and far
echo 6 and 10 dB below our signal; with ±80 ppm clock offset 33600 one
way and 31200 the other), 31200 at 38 dB, 28800 at 35 dB, 26400 at
33 dB, 24000 at 30 dB, 21600 at 28 dB, 19200 at 25 dB, 14400 at 22 dB.
`[echo dB] [far echo dB]` add the echoes; `FAR_DELAY=<samples>` delays
the far one (200 ms: still 33600 bit/s).

A call connects two modem processes with pipes: `half_open` (this code
on the modem core, `../core`) and another modem's harness, or a second
`half_open` (the `open` label). Each goes through the modem's whole
controller: AT commands, V.42/LAPM detection and data in both
directions. The other modems, their harnesses and what the calls with
them give are in [interop.md](interop.md): Smart Link's original library
(`half_smartlink`), the Agere, SM56, PCTel, HSF and Broadcom soft
modems and spandsp's modems.

`NET=<rate A>:<rate B>:<ulaw|alaw>` puts `netsim`, a simulated G.711
network, between the two: each direction goes through the law at 8000 Hz,
and to a modem at 9600 Hz through a converter that acts like a codec on
an analogue line (a raised cosine at 4000 Hz, so that PCM samples stay
free of intersymbol interference; `NET_PLAIN=1` takes dsp_util's short
resampler instead, which is not, so V.90 falls back to V.34 through it). `half_open8` is
the open modem at 8000 Hz (the original library does not work at 8000 Hz).
`NET_ANALOG=a` (or `b`) with `NET=8000:8000` makes A (or B) an analogue
modem with its codec at 8000 Hz (the raised cosine sampled at
`NET_PHASE`, `NET_PPM`), and `MODEM_ANALOG=<name>` (or `1`) gives that
half_open an analogue line (`Host::analog_line()`: it dials there, and
its pumps are the analogue modem's).
`NET_CODEC=amr:12.2` (AMR at 4.75 to 12.2 kbit/s) or `NET_CODEC=amrwb:23.85`
(AMR-WB at 6.6 to 23.85 kbit/s; `:dtx` after the rate turns their
discontinuous transmission on) puts a mobile network's speech codec on the
8000 Hz line in place of G.711, each direction encoded and decoded frame by
frame (`dsp/test/codec_sim.cpp`, on the OpenCORE and VisualOn libraries:
Debian's libopencore-amrnb-dev, libopencore-amrwb-dev, libvo-amrwbenc-dev;
without them netsim says so), and `NET_CODEC_LOSS=<percent>` erases that
share of its frames at random (`NET_CODEC_SEED`), as lost packets would.
`codec_probe` runs a file of samples through the same codec, for measuring
what it does to a signal.  What the modulations and the codecs make of each
other is in [cellular/](cellular/v_cvm.tex), a proposed Recommendation for
data over mobile telephony's speech paths ([v_cvm.pdf](v_cvm.pdf)).
`NET_GAIN` sets an analogue gain on the way to the 9600 Hz modem;
on that way the codes may also cross a digital loss pad (`NET_PAD=<dB>`:
each one re-encoded that much lower) and robbed bit signalling
(`NET_RBS=<mask>`: in the samples whose count mod 6 has its bit in the
mask the octet's least significant bit is forced to 1, as on T1 trunks),
what V.90's DIL finds ([v90a.md](dsp/v90a.md)). The 9600 Hz modem's
codec is on the network's clock, its samples on the network's symbols,
unless `NET_PHASE=<fraction>` (its samples that fraction of a network
sample late) or `NET_PPM=<ppm>` (its clock that many parts per million
fast, both ways) say otherwise: a converter whose output times move
by the clock (netsim's raised cosine toward it, a 3900 Hz low pass
from it), what V.90's PCM receiver has to follow. A modem's own echo
from its line's hybrid is the modem's to add (netsim has the samples in
blocks): `DSP_TEST_ECHO=<dB>:<ms>[a|c]` puts in what an open modem at
9600 Hz receives what it sent <ms> before, <dB> down, through a high
pass at 300 Hz and a low pass at 3400 Hz (`a` or `c`: the answerer's or
the caller's only); and
`NET_ECHO=<dB>:<ms>` the echo of a hybrid at its line (in its own sample
clock: what it receives comes back with what it sends; from the line,
before its codec would clip it). The delay has to be more than the
modem at A sends ahead of what it has received (some 20 ms for the HSF
modem): netsim says how many samples went without their echo
otherwise, and an echo with gaps in it is not one a canceller can
learn.

The fax calls (`<label>_fax_*`) do the same with `AT+FCLASS=1`:
`interop_call --fax` plays the T.30 side of both fax machines: CSI/DIS,
DCS, TCF, CFR, one page (for V.17 with the short train), EOP, MCF and
DCN, at ten times real time (above). It accepts a page with a bit error
rate below 1e-3 (a fax page without ECM survives that): the original
library makes a few bit errors with some of its own signals.

`fax_test`'s `FaxModulation` cases send random data through each fax
modulation over a simulated line. The V.17 short train codes run a long
train first, so the receiver has an equalizer to start from.
