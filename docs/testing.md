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
    ctest --test-dir build -L interop -j16  # calls between the original library and this one,
                                            # data and class 1 fax (needs g++ -m32)
    ctest --test-dir build -L agere -j16    # calls between the Agere soft modem and this one
                                            # (sm56, pctel, hsf, spanpeer: the other modems)

The test programs are in `dsp/test/` (the link layer's and the core's in `link/test/` and `core/test/`). All the tests are GoogleTest programs (the system's libgtest-dev, or
fetched while configuring); ctest runs each of their cases as a test of
its own, named `<suite>/<Test>.<case>/<parameters>`, e.g.
`Line/PumpLoopback.DataBothWays/v34_echoes`, or for the calls
`<label>.<call>`, e.g. `agere.agere_v92_qc`. Run one by itself with
`ctest -R v34_echoes`, or the program with `--gtest_filter`:

| Program | What |
|---|---|
| `pump_test` | the data pumps back to back (`loopback.cpp`): each modulation on clean and noisy lines, 8000 Hz, clock offsets, gains, echoes; the answering automode |
| `dmodem_test` | libdmodem: pairs of its modems, retrains and renegotiations in the data, V.91 across G.711 networks, `dmodem.hpp` |
| `dmodem_fax_test` | libdmodem's fax: a data modem answering hands the call to a fax receiver |
| `fax_test` | the class 1 modulations, T.4/TIFF (also against libtiff's `tiffcp`), two T.30 machines back to back |
| `spandsp_test` | spandsp's V.17 transmitter and T.4 encoder into ours, our T.30 against spandsp's |
| `../../link/link_test`, `../../core/test/core_test`, `core_equiv` | the data link layer, the core, the new core against the old one (or against a script's `.expect`, see `docs/core.md`) |
| `interop_test`, `agere_test`, `sm56_test`, `pctel_test`, `hsf_test`, `spanpeer_test` | calls between this modem and the others (`interop_test.cpp`: one table of the calls, each program a label's) |

Each call writes its modem logs to `build/dsp/test/logs/<test>/`. The
calls run as fast as the modems can: every modem keeps time by its
line's samples (the Agere, SM56, PCTel and HSF harnesses count jiffies
and timers in samples, our core does with `PIPE_SYNC`, and so does
slmodemd's old core under the original library, `test/legacy/`, whose
timers are run by the line too), so a call goes as it would in real
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
  fast as it goes);
- the driver waits 100 ms of the line after a CONNECT before its data (the
  SM56 says CONNECT a little ahead of taking data, and echoes what
  comes sooner as a command line);
- the data that comes around a call's mid-call commands (`MIDCALL`) is
  kept: before the escape's OK and right after `ATO`'s CONNECT.

The fax calls (`interop_call --fax`) keep time by the driver's own relay
of the line (A's samples at `LINE_RATE`: 8000, 9600 with the original
library), and go ten times as fast as real time at most
(`MODEM_SPEED=10`, `half_open`'s and `half_blob`'s): the fax modems take
so little that the line would otherwise go a few hundred times as fast,
and the milliseconds the driver takes to answer what a modem said (it
plays T.30 for both machines) would be most of a second of the line,
past T.30's turnarounds. The modems take the DTE's commands every 192
samples (20 ms at 9600 Hz, the original library's frame): its V.17
receiver takes a short train or not depending on how its frames fall on
it, so our training has to start at the same point of them every time.
So they take the turnarounds the paced calls did and give the same TCF
and pages, in a tenth of the time.

The calls run a little nicer (`nice` 10) than the rest of the machine.
Each takes about one processor while it runs, but for the moments on
hook in real time, so `-j` can be some four times the processors: on 4,
the whole suite takes some 75 s at `-j16` (105 at `-j8`; 13 minutes when
every call ran in real time). Much more than that (`-j32` on 4) leaves
the calls on hook short of real time, and the PCTel V.90 call, which
fails when its setup lands a period later, can fail. None of the calls
has to run alone. `MODEM_REALTIME=1 ctest ...` runs every call in real
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
filter, no bit errors): 33600 bit/s at 40 dB (also with ±80 ppm clock
offset, or with near and far echo 6 and 10 dB below our signal), 31200
at 38 dB, 28800 at 35 dB, 26400 at 33 dB, 24000 at 30 dB, 21600 at
28 dB, 16800 at 25 dB, 14400 at 22 dB. `[echo dB] [far echo dB]` add the echoes;
`FAR_DELAY=<samples>` delays the far one (200 ms: still 31200 bit/s).

`interop` connects two `modem_half` processes with pipes. One process is
linked with `test/smartlink/dsplibs.o` on slmodemd's old C core
(`test/legacy/`: the original library calls into it), the other with this code on the modem
core (`../core`). Each call goes through the full modem core: AT commands,
V.42/LAPM detection and data in both directions. Results at the time of writing:

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

`NET=<rate A>:<rate B>:<ulaw|alaw>` puts `netsim`, a simulated G.711
network, between the two: each direction goes through the law at 8000 Hz,
and to a modem at 9600 Hz through a converter that acts like a codec on
an analogue line (a raised cosine at 4000 Hz, so that PCM samples stay
free of intersymbol interference). `half_blob8`/`half_open8` are the
modems at 8000 Hz (the original library does not work at 8000 Hz). `NET_GAIN`
sets an analogue gain on the way to the 9600 Hz modem, and
`NET_ECHO=<dB>:<ms>` the echo of a hybrid at its line (in its own sample
clock: what it receives comes back with what it sends; from the line,
before its codec would clip it). The delay has to be more than the
modem at A sends ahead of what it has received (some 20 ms for the HSF
modem): netsim says how many samples went without their echo
otherwise, and an echo with gaps in it is not one a canceller can
learn.

`fax_interop` does the same with `AT+FCLASS=1`. `interop_call --fax` plays
the T.30 side of both fax machines: CSI/DIS, DCS, TCF, CFR, one page (for
V.17 with the short train), EOP, MCF and DCN, at ten times real time
(above). Results at the time of writing:

| Modulation | original sender → open receiver | open sender → original receiver |
|---|---|---|
| V.27ter 2400, 4800 | pass | pass |
| V.29 7200, 9600 | pass | pass |
| V.17 7200, 9600, 12000, 14400 (long train for TCF, short for the page) | pass | pass |

The original library makes a few bit errors with some of its own signals,
so `interop_call --fax` accepts a page with a bit error rate below 1e-3 (a fax
page without ECM survives that). Its V.27ter training also contains stray
180 degree phase flips, which the open receiver follows.

`fax_test`'s `FaxModulation` cases send random data through each fax
modulation over a simulated line. The V.17 short train codes run a long
train first, so the receiver has an equalizer to start from.
