# Building and testing the DSP: technical details

This is the technical companion to [Building and testing](testing.md), the plain-English overview. Start there; this page keeps the full detail: the code's names, the Recommendations' clauses, the numbers and the test lists.

## Building

    cmake -B build                            # in the top directory
    cmake --build build

There is no i386 requirement. D-Modem builds natively on x86_64, ARM and
other architectures, and links with `-lm`. Clang and GCC both build it.

Only the interop tests need a 32-bit build (`-m32`, with 32-bit
libraries: `g++-multilib` on Debian or Ubuntu), because the original
library and the other vendors' modems are 32-bit i386 code. Without it
those tests are skipped.

### Other C libraries

With a C library other than glibc (musl on Alpine, Windows', Apple's),
the test programs take glibc's `rand()` sequence from
`dsp/test/libc_compat.h`. Their noise and random fax pages are then the
same draws the tests' thresholds were set with; with a different
sequence a couple of the spandsp tests fail on musl. `libc_compat.h` is
included ahead of every test source in `dsp/test/`; with glibc on Linux
it is empty.

CI's `musl` job builds and runs the offline tests on Alpine. The tests
there need `coreutils` (for `stdbuf`) and `tiff-dev` (for spandsp's
headers).

### Windows

A Windows build (MinGW-w64, `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake`)
runs its tests under Wine: `ctest --test-dir build-win -L quick`. The
interop tests are left out there, since netsim is not built for Windows
([windows.md](windows.md)).

## Tests

### Running them

    ctest --test-dir build -j16         # all of them (-j about four times the
                                        # processors: about 1.5 min on 4)
    ctest --test-dir build -L quick     # back-to-back pumps, libdmodem, fax
    ctest --test-dir build -R doc_check # every function has a comment above it
                                        # (cmake/doc_check.py; run it on its own as
                                        # python3 cmake/doc_check.py . [FILE...])
    ctest --test-dir build -L spandsp   # against spandsp (libspandsp-dev)
    ctest --test-dir build -L smartlink -j16  # calls between Smart Link's soft modem
                                              # (its original library) and this one,
                                              # data and class 1 fax (needs -m32)
    ctest --test-dir build -L agere -j16      # calls between the Agere soft modem
                                              # and this one
    ctest --test-dir build -L flaky     # only the known flaky tests
                                        # (-LE flaky: all but them)

The labels for the interop calls are one per peer:

| Label | The calls |
|---|---|
| `smartlink` | Smart Link's original library |
| `agere`, `sm56`, `pctel`, `hsf`, `broadcom`, `intel`, `ess` | the other vendors' soft modems |
| `spanpeer` | spandsp's modems |
| `hda` | the open modem on an emulated HD-audio modem codec |
| `open` | two open modems |
| `ring` | netsim putting the call through as each country's exchange does |
| `adaptive` | fax senders and data callers into the open modem answering with `AT+FAA=1` |

Other labels: `quick` (the offline tests), `spandsp` (spandsp's
transmitters and T.30 against ours), `alsa` (dmodemd on emulated
hardware), `osmocom` (V.110 against libosmocore, when it is configured),
`flaky` and `logs` (the log fixture's own tests).

### The flaky tests

The known flaky tests are listed in `cmake/flaky_tests.cmake`. Each is an
open intermittent problem, and most fail only on a loaded machine. They
run last and one at a time:

- Every other test requires the fixture `dmodem_steady`, whose cleanup
  test `dmodem_flaky_gate` runs once they have all ended.
- The flaky tests depend on `dmodem_flaky_gate` and are `RUN_SERIAL`.
- Each keeps its own labels and has `flaky` as well.
- Run on its own (`-R`), a flaky test waits for nothing.
- A gtest program's flaky cases are found by a discovery of their own
  (`TEST_FILTER`, CMake 3.22 and later; with older CMake they run with
  the others).

When one of them is fixed, take it off the list. A new one goes on the
list with its task number.

### Test names

The test programs are in `dsp/test/`; the link layer's and the core's are
in `link/test/` and `core/test/`, and dmodemd's and the Windows driver's
in `dmodemd/test/` and `winmodem/test/`.

Most of them are GoogleTest programs (the system's libgtest-dev, or
fetched while configuring). ctest runs each of their cases as a test of
its own, named `<suite>/<Test>.<case>/<parameters>`, for example
`Line/PumpLoopback.DataBothWays/v34_echoes`. The interop calls are named
`<label>.<call>`, for example `agere.agere_v92_qc`. The others are
scripts and commands added straight to ctest (`doc_check`,
`v44_preset_check`, `compress_bench_check`, the `dmodemd_*` Python
tests, and so on).

Run one test by itself with `ctest -R v34_echoes`, or run the program
directly with `--gtest_filter`.

### The test programs

**The data pumps and their parts** (`dsp/test/`, label `quick` unless
noted):

| Program | What it tests |
|---|---|
| `pump_test` | The data pumps back to back (`loopback.cpp`): each modulation on clean and noisy lines, at 8000 Hz, with clock offsets, gains and echoes; the answering automode. A side passes when it is connected and has carried at least 90% of the bits its rate allows from the CONNECT on, at a bit error rate of 1e-3 at most |
| `bits_test`, `g711_test`, `pcm_cp_test` | `bits.h`'s fields and CRCs; `g711.h`'s two laws (each octet back to itself, sign and magnitude); the PCM encoders on CPs that cannot carry their rate |
| `voice_codec_test` | `+VSM`'s sample formats (`voice_codec.h`) |
| `dtmf_test` | `dsp.h`'s DTMF detector on the generator's digits, through G.711, with twist, and by length |
| `call_tone_test`, `ring_detect_test` | The call progress tones a voice dial hears (ringing, busy, answer tones) and the ring signal a modem codec hears on hook (`dsp.h`'s `CallToneDetect`, `RingDetect`) |
| `ring_tones_test` | Each country's tones (`tones.cpp`, from the ITU's list of national tones: dial, ringing, busy and congestion tones, the ring signal): the table, and the ring signal through `RingDetect` to the core, with a RING for each ring and the auto answer at S0=2 |
| `hw_test` | The hardware the harnesses emulate (`hw/`), on a host of its own: the ICH's bus masters, and the HD Audio controller's commands and streams with the Si3054 on it |
| `rs_test` | The Reed-Solomon code (`rs.cpp`) |
| `k56flex_test` | K56flex's data mode encoder into its decoder at every rate, pad, law and a set of robbed-bit patterns (see [K56flex](dsp/k56flex-details.md#5-data-mode)) |
| `k56flex_sm56` | The same encoder into the decoder of Motorola's SM56 client, and our training against the digital-side transmitter it carries (label `sm56`, 32 bit builds) |
| `sms_pdu_test` | Fixed-line SMS's messages (`sms_pdu.cpp`): SMS-SUBMIT and SMS-DELIVER against known encodings, the 7-bit alphabet with its escapes, UCS2, user data headers, the length limits |
| `vcvm_test`, `vcvm_startup_test`, `vcvm_data_test` | V.cvm's signal set, start-up and data mode, through the codecs where the AMR libraries are installed (see [V.cvm](dsp/vcvm-details.md#tests)) |
| `vwb_test` | V.wb, the wideband mode (`vwb.h`): its FFT, QAM and interpolator; then a caller and an answerer on a simulated bare pair at 96000 Hz (`vwb_line.cpp`: 24 AWG cable loss k·sqrt(f)·length, the transformers, white noise, interferers, -15 dB of hybrid echo, the clocks 100 ppm apart, a dropout). Error free on 0.5 to 5 km, the rate falling with length; through interferers; for two minutes with the clock offset either way; a retrain after the dropout; 48000 and 192000 Hz. Each pair's rates are printed (`VWB_DEBUG=1` adds the modems' logs) |

**The fax** (`dsp/test/`):

| Program | What it tests |
|---|---|
| `fax_test` | The class 1 modulations, T.4 and TIFF (also against libtiff's `tiffcp`), and two T.30 machines back to back |
| `t38_test` | T.38: the IFP packets' encodings (both versions), UDPTL's redundancy, and two T.30 machines over T.38 with packets lost |
| `class1_t38_test` | Class 1 over T.38: a DTE of the test's own goes through T.30 with the engine's commands against our T.30 machine over T.38, sending and receiving, with ECM, both packet versions, and T.38 after a while on the audio; the packets' fields and times |
| `spandsp_test` (label `spandsp`) | spandsp's V.17 transmitter and T.4 encoder into our receivers; our T.30 against spandsp's, on the audio and over T.38 (its T.38 terminal) |

The details of the fax tests are in [Fax](dsp/fax-details.md).

**libdmodem** (`dsp/test/`, label `quick`):

| Program | What it tests |
|---|---|
| `dmodem_test` | Pairs of libdmodem's modems: retrains and renegotiations in data, V.91 across G.711 networks, the network's law for each call, `dmodem.hpp` |
| `dmodem_fax_test` | libdmodem's fax: a data modem answering hands the call to a fax receiver; a fax over T.38 through the API, in UDPTL with packets lost, and in IFP packets (`t38_ifp`, as Asterisk's channels have them) of at most 72 octets |
| `leased_call_test` | Leased lines under libdmodem at 9600 Hz on a simulated circuit with no exchange ([leased.md](leased.md)). Each voice band modulation on `AT&L1`; V.29, V.33 and V.27ter at each rate on `AT&L2`; the faster end coming down; a dropout under LAPM; noise that brings V.33 down; a late answerer; a long cut and the call again; a reroute that V.34 and V.32bis retrain through (and V.32bis does not with `DSP_V32_LOST=0`); a burst of noise. V.54's loops 2 and 3 with V.52's self-test (`AT&T`), refused loops, S18. V.13's simulated carrier (S48). V.91 on a 4-wire digital circuit (`AT&L2+MS=V91`, G.711 octets at 8000 Hz): both laws, robbed bits, a late answerer, a short and a long cut. `LEASED_DEBUG=<level>` gives libdmodem's log |
| `clear_call_test` | The clear 64 kbit/s channel (`AT+MS=CLEAR`): two modems on a simulated digital channel. CONNECT 64000 both ways; data through X.75 and through the modem's own link; a late answerer, no answerer, a nailed-up channel (`AT&L2`) that calls again; X.75 as T.90 and isdn4linux have it, through a dropout; PPP's frames. `CLEAR_DEBUG=<level>` gives libdmodem's log |
| `v110_osmo_test` (labels `quick`, `osmocom`) | V.110 against libosmocore's implementation. Built only when `-DOSMOCORE_SRC=<libosmocore source tree>` (1.14 or later) is given |
| `vwb_call_test` | V.wb under libdmodem at 96000 Hz with `AT&L1` (S0=1 answering with no ring): bulk data both ways through V.42, a retrain under LAPM, NO CARRIER and the call again by itself, each end behind a host's buffers (dmodemd's USB line's: 20 ms frames, 4672 samples ahead), and refusal at 9600 Hz |

**The link layer and the core:**

| Test | What it tests |
|---|---|
| `g729a_test` (label `quick`) | D-Modem's G.729 Annex A: the frame's packing, a round trip, and every Annex A test vector bit for bit, encoder and decoder (skipped without `docs/specs/fetch_g729.sh`'s vectors) ([v70-details.md](v70-details.md#g729-annex-a)) |
| `g729a_bcg729_test` (label `quick`, built only where libbcg729 is installed) | The coder against bcg729 both ways: each decodes the other's frames as well as its own. bcg729 is GPL; it is linked into this test alone |
| `dsvd_audio_test` (label `quick`, not on Windows) | dmodemd's `--dsvd-audio` command: `cat` gives back what it is sent, a command that ends at once |
| `link_test`, `core_test`, `core_equiv` (label `quick`) | The data link layer; the core; and the core's transcripts against the scripts' `.expect` files (slmodemd's old core's, recorded from it; see [core.md](core.md)) |
| `compress_bench_check` (`compress_bench --check`) | The compressors' round trips on text, binary, ANSI and noise at three flush intervals. `compress_bench <file>...` measures them ([link-details.md](link-details.md#compression-benchmark)) |
| `zstd_ref_test` (label `quick`) | zstd against the reference libzstd, both ways; built where libzstd is found |
| `v44_preset_check` (label `quick`) | V.cvm's preset dictionary in `link/v44_preset.cpp` is the Recommendation's Annex E |
| `doc_check` (label `quick`) | Every function in the code has a comment above it |

**The interop calls** (`dsp/test/`, one program per label):

| Program | What it tests |
|---|---|
| `smartlink_test`, `agere_test`, `sm56_test`, `pctel_test`, `hsf_test`, `broadcom_test`, `intel_test`, `ess_test`, `spanpeer_test`, `open_test`, `hda_test`, `ring_test`, `adaptive_test` | Calls between this modem and the others ([interop.md](interop.md)). `hda` is the open modem on an emulated HD-audio modem codec; `ring` has netsim put the call through as each country's exchange does (dial tone, digits, ringing, busy and congestion tones); `adaptive` sends fax senders and data callers into the open modem answering with `AT+FAA=1`. All the calls are in one table in `interop_test.cpp`; each program runs one label's |

**dmodemd and the hardware** (`dmodemd/test/`):

| Test | What it tests |
|---|---|
| `dmodemd_alsa` (`alsa_test.py`, label `alsa`) | Two dmodemd on fake ALSA modem cards (alsa-lib plugins), calling each other through a simulated exchange: dial tone, DTMF, rings, caller ID, V.34, busy, a stall, a voice call ([hardware.md](hardware.md)). A fake HD-audio hwdep device (`fake_hda_hwdep.cpp`, by `LD_PRELOAD`) checks the Si3054's line settings (`-c UK`, `AT+GCI`, RZ cleared off hook, another chip left alone). Needs libasound2-dev |
| `dmodemd_usb` (`usb_test.py`, label `alsa`) | Two dmodemd on fake SmartUSB56s (`fake_st7554.cpp`, a libusb put in front of the real one by `LD_PRELOAD`) on the same exchange: the device's checks, rings, V.34, busy, a voice call, unplugging ([hardware.md](hardware.md)). Needs libusb-1.0-0-dev |
| `dmodemd_agere` (`agere_test.py`, label `alsa`) | dmodemd on two fake Agere USB 2.0 soft modems (`fake_agere.cpp`) on the same exchange: the ROM's stream, rings and caller ID, a call each way with text, pulse dialling, unplugging |
| `dmodemd_pico_line` (`pico_line_test.py`, label `alsa`) | The same on fake pico-serial Line sticks (`fake_pico_line.cpp`), with caller ID on hook, and the country's line settings written to the stick's DAA (`-c UK`, then `AT+GCI`) ([hardware.md](hardware-details.md#pico-serial-line)). Needs libusb-1.0-0-dev |
| `dmodemd_pico_wb` (`pico_wb_test.py`, labels `alsa` and `quick`) | Two dmodemd on fake pico-serial WB sticks (`fake_pico_wb.cpp`) at the two ends of a simulated bare pair, in real time at 96 kHz (`wb_pair.cpp`, built on `dsp/test/vwb_line.cpp`): the stick's rates and gains, V.wb by default, `AT&L1`, a CONNECT above 50000 bit/s (148656 on this pair), the LINK LEDs, 32 kB each way through V.42, the hang-up, unplugging. It takes about 20 s and runs alone (`RUN_SERIAL`) ([hardware.md](hardware-details.md#pico-serial-wb-wideband-bare-pair)). Needs libusb-1.0-0-dev |
| `dmodemd_*_builtin` (label `alsa`) | On macOS (which has no `LD_PRELOAD`), and on Linux with `-DDMODEM_TEST_BUILTIN_FAKES=ON`: the USB, pico Line, pico WB and Agere tests with each fake linked into a dmodemd of its own (`dmodemd_fake_st7554`, ...) in libusb's place. On Linux they are named `dmodemd_usb_builtin` and so on; on macOS they keep the plain names |
| `dmodemd_usb_windows`, `dmodemd_pico_line_windows` (label `alsa`, a Windows build under Wine) | The two tests above with dmodemd.exe and the fake linked in libusb's place (`dmodemd_fake_*.exe`), its DTE on TCP, and the fakes' connections relayed over TCP to a Linux build's exchange (`usb_dte.py`, `fake_net.h`) ([windows.md](windows-details.md#usb-modems---usb)) |
| `dmodemd_clearmode` (`clearmode_test.py`, label `quick`) | dmodemd's clear 64 kbit/s channel over SIP (RFC 4040's CLEARMODE) against the script's own SIP peer and clear channel, calling out and in |
| `dmodemd_srtp` (`clearmode_test.py --srtp`, label `quick`) | The same calls in SRTP; skipped in a build without SRTP |
| `dmodemd_v150` (`v150_test.py`, label `quick`) | dmodemd with `SIP_V150=yes` against the script's own SIP peer and V.150.1 gateway (SSE, SPRT), calling out and in: the SDP lines, ANSam as events, `SSE:M`, `INIT`, `JM_INFO`, `CONNECT 33600`, a line each way, `CLEARDOWN` ([v150-details.md](v150-details.md#tests)) |
| `V8bis.*` (`v8bis_test.cpp`, label `quick`) | V.8 bis's messages against their octets, the parameter trees parsed past what is not read, the signals through their detector, the messages through V.21 and HDLC ([v8bis-details.md](dsp/v8bis-details.md#tests)) |
| `Core.Dsvd*` (`core_test.cpp`, label `quick`) | Two modems with `AT+A8E=1,1,,1+DSVD=1`: V.8 bis chooses DSVD, V.76's data channel carries the DTEs' characters both ways and the hang-up closes it; DSVD at the caller alone: data and V.42; `Core.DsvdVoice`: with a voice path at both ends, each hears the other's tone through G.729 Annex A with the data alongside, the voice channel closed at hang-up ([v70-details.md](v70-details.md#tests)) |
| `Core.V8bis*` (`core_test.cpp`, label `quick`) | Two modems with `AT+A8E=1,1,,1` at both, one or the other: the transaction (MRe, CRd, CL, MS, ACK(1)) then V.8, or V.8 without it ([v8bis-details.md](dsp/v8bis-details.md#tests)) |
| `V150.*` (`v150_test.cpp`, label `quick`) | V.150.1's codings, SPRT over a lossy network (order, acknowledgements, windows, retransmission) and two modem relay sessions: the start, the modulation chosen, data both ways, the hang-up, compression refused ([v150-details.md](v150-details.md#tests)) |
| `dmodemd_misdn` (`misdn_test.py`, label `quick`) | Two dmodemd on fake mISDN cards calling each other through the script's ISDN network: a clear channel call, an audio call at V.22bis, busy, no answer |
| `dmodemd_root` (`root_test.py`, label `quick`) | dmodemd started as root: it carries on as nobody, saves its data file, and its pty's group and link (kept by a process of its own) follow the new pty after the DTE hangs up and go when it ends, even when killed. Skipped unless ctest runs as root (CI runs it with sudo) |
| `hda_modem_setup` (`hda_modem_setup_test.py`, label `quick`) | `hda-modem-setup` on a fake sysfs and kernel configuration: the modem codecs listed with their drivers; the early patch (the Si3054's vendor ID) and its module option written for a codec with no driver outside snd-hda-codec-si3054's list, or for one named; refused without the patch loader; another `patch=` option warned of; both files removed |
| `Si3056.*` (`si3056_test.cpp`, label `quick`) | The Si3056 datasheet's country settings (`si3056.h`): the names, and the registers they give on a Si3018 and a Si3019 line side, with the other bits kept |
| `Si3054.*` (`si3054_test.cpp`, label `quick`) | The Si3054 datasheet's country settings (`si3054.h`): the names, the chip ID check, the registers on and off hook, with the other bits kept |

**The Windows driver** (`winmodem/test/`):

| Test | What it tests |
|---|---|
| `PortTest.*`, `Wire.*` (`port_test.cpp`, label `quick`) | The virtual modem driver's COM port (`winmodem/port.h`): reads under each kind of serial timeout, writes held and timed, `WaitCommEvent`'s events, the modem's lines and the application's DTR and RTS, `LSRMST_INSERT`, purges, the settings; the frames to dmodemd ([windows.md](windows-details.md#the-virtual-modem-driver---driver)) |
| `VirtualModem.*` (`vmodem_test.cpp`, label `quick`, Windows under Wine) | dmodemd.exe on the driver's pipe (`--driver`), with the driver's end of it (port and pipe) in the test: AT commands and the INF's settings, DSR following dmodemd, the driver coming back, `--driver auto`, a name already taken; its `bridge` for `asterisk/test/sip.sh`'s `WIN_DRIVER=1` |
| `umdf1_test` (label `quick`, Windows) | The driver for Windows 7 and 8 (`dmodemvm7.dll`, UMDF 1) in a host of the test's own, with dmodemd.exe on its pipe |
| `winmodem_inf`, `dmodem_driver_list` (label `quick`) | `dmodem.inf` as `winmodem/make_inf.py` makes it from the core's result codes; `dmodem-driver.exe list` (Windows) |

## Test logs

A ctest run with a failure keeps the logs of the tests that failed, in a
directory of its own under `build/test-logs/`, named for when the run
started (`latest` points to the newest). A run without a failure leaves
nothing there; skipped tests are not failures.

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

This works with plain ctest, including `-R`, `-L` or `--repeat`. A
fixture does it (`cmake/test_logs.cmake`):

1. `test_logs_begin` makes the run's directory before the first test.
2. Every test runs under `cmake/test_run.sh`, which keeps its output and
   still passes it on to ctest. A test finds its directory in
   `DMODEM_TEST_DIR`.
3. When a test passes, the launcher deletes its directory at once. A test
   without its `result.txt` was killed (for example at its ctest timeout)
   and counts as failed.
4. `test_logs_end` writes the summary after the last test. When the run
   ended without a failure, it deletes the run's directory and points
   `latest` at the newest run left.

Settings:

- `DMODEM_TEST_KEEP_PASSED=1` keeps everything (some 100 MB for a full
  run).
- The 20 latest runs are kept (`-DDMODEM_TEST_LOG_KEEP=<n>`).
- `-DDMODEM_TEST_LOG_ROOT=<dir>` puts them elsewhere.

### Debug levels

Every run logs verbosely. The launcher sets the debug level of every
modem and library the tests run to `DMODEM_TEST_DEBUG` (5 by default),
wherever it is not already set:

- `DSP_DEBUG` (the pumps), `CORE_DEBUG`, `DMODEM_DEBUG` (libdmodem),
  `DBG` (the interop calls' modems), `DMODEMD_DEBUG` (dmodemd's `-d`);
- `LINK_DEBUG` and `SPANDSP_DEBUG` are switched on.

Level 5 is what the modems log of each step of a call, with their
equalizers' and retrains' figures as they go. Levels 6 and up log every
bit and symbol (`DSP_DEBUG=6 ctest -R ...` for one test).
`DMODEM_TEST_DEBUG=0` leaves the tests' own levels. With all of this, a
full run takes about a tenth longer.

A call run by hand (`interop_call`, or a test program outside ctest)
writes its modem logs to `LOG_DIR`, or to `build/dsp/test/logs/<test>/`
for the test programs.

## How the calls keep time

The interop calls run as fast as the modems can. Every modem keeps time
by its line's samples: the Agere, SM56, PCTel, HSF and Broadcom harnesses
count jiffies and timers in samples, and our core, under the open pumps
or the original library, does so with `PIPE_SYNC`. So a call goes as it
would in real time, only sooner, and a busy machine only slows it down.

The call's own seconds (`STREAM`, `MIDCALL`, `WAIT`, `ANSWER_AFTER`,
`HANGUP_WAIT`, and how long the driver waits for what the modems say)
are then the line's seconds, from netsim's clock (`NET_CLOCK_FD`). They
run faster or slower than the wall clock as the machine allows. The
wall clock is used only while the line stands still (a modem has gone).

Three things keep such a call the same from run to run, however late the
driver answers:

- **On hook, our modem and the original library run in real time anyway**
  (`PIPE_SYNC`), so the driver's commands meet the samples they would in
  real time. The call itself, from the dial or the answer, runs as fast
  as it can. Their line also runs from the start, as the other harnesses'
  lines do: the lead silence is written when they start, not at the first
  `ATD` or `ATA`. A line that stood still until then took the commands as
  they came, and the call began wherever the scheduler had the two
  modems at that moment.
- **The driver waits 100 ms of the line after a CONNECT before sending
  data** (`DATA_WAIT`). The SM56 says CONNECT a little before it takes
  data, and echoes anything that comes sooner as a command line.
- **The data around a call's mid-call commands (`MIDCALL`) is kept**:
  before the escape's OK, and right after `ATO`'s CONNECT.

### Fax calls

The fax calls (`interop_call --fax`) keep time by the driver's own relay
of the line (A's samples at `LINE_RATE`: 8000, or 9600 with the original
library). They go at most ten times as fast as real time (`MODEM_SPEED=10`,
for `half_open` and `half_smartlink`):

- The fax modems use so little processor time that the line would
  otherwise run a few hundred times as fast. The milliseconds the driver
  takes to answer a modem (it plays T.30 for both machines) would then be
  most of a second of the line, past T.30's turnarounds.
- The modems take the DTE's commands every 192 samples (`PIPE_SYNC=192`:
  20 ms at 9600 Hz, the original library's frame). The original library's
  V.17 receiver takes a short train or not depending on how its frames
  fall on it, so our training has to start at the same point of those
  frames every time.

So the calls take the same turnarounds the real-time calls did and give
the same TCF and pages, in a tenth of the time.

The relay also holds the line while the driver has not yet answered what
a modem said (from the modem's output until the driver's next poll or
write). However late the scheduler lets the driver run, its answer meets
the line where it would have at once. (Before this, the Agere modem's
`+FTH` ended in ERROR now and then, its frame arriving some seconds of
line time after its CONNECT.)

### Parallel runs

The calls run a little nicer (`nice` 10) than the rest of the machine.
Each takes about one processor while it runs, except for the moments on
hook in real time, so `-j` can be about four times the number of
processors. On 4 processors the whole suite takes some 75 s at `-j16`
(105 s at `-j8`; 13 minutes when every call ran in real time). Much more
than that (`-j32` on 4) leaves the calls on hook short of real time.
None of the calls has to run alone.

`MODEM_REALTIME=1 ctest ...` runs every call in real time, as they all
did before (useful for comparing a failure).

## Tools for trying a line by hand

The test programs are in `build/dsp/test/`.

### interop_call

The calls go through `interop_call.cpp`; the `interop_call` program
makes one by hand:

    interop_call <caller> <answerer> ["ATX3;AT+MS=122"]
    interop_call --fax <sender> <receiver> [mod] [AT setup]

The call's settings come from the environment (`interop_call.h`: `NET`,
`STREAM`, `BULK`, `CALLS`, `EXPECT_B`, ...; `NETSIM` for netsim's path
and `LOG_DIR` for the logs, by default the current directory).

A call connects two modem processes with pipes: `half_open` (this code on
the modem core, `../core`) and another modem's harness, or a second
`half_open` (the `open` label). Each goes through the modem's whole
controller: AT commands, V.42/LAPM detection and data in both
directions. `half_open8` is the open modem at 8000 Hz (the original
library does not work at 8000 Hz). The other modems, their harnesses and
the results of calls with them are in [interop.md](interop.md): Smart
Link's original library (`half_smartlink`), the Agere, SM56, PCTel, HSF,
Broadcom, Intel and ESS soft modems, and spandsp's modems.

### dsp_loopback

    dsp_loopback <dp> [snr dB] [seconds] [srate] [line gain] [clock ppm]
                 [echo dB] [far echo dB]

`dsp_loopback` runs a caller and an answerer through a simulated line
with delay, a low-pass filter, white noise and an optional sample clock
offset. It then checks the bit error rate in both directions. It is the
simulation `pump_test` uses, for trying a line from the command line.

| Setting | Effect |
|---|---|
| `[echo dB]` | An echo of each modem's own signal (the hybrid's, about 2 ms) at that level |
| `[far echo dB]` | The far end's hybrid echo, after a round trip over the line |
| `FAR_DELAY=<samples>` | Delays the far echo more, as over VoIP |
| `LINE_DELAY=<samples>[:<samples>]` | More delay toward the answerer (and toward the caller, if a second value is given) |
| `CALLER_DP=<id>` | Gives the caller a different pump from the answerer (automode) |
| `FLAT=1` | Leaves out the low-pass filter |
| `DUMP=<prefix>` | Records what each side hears |
| `CODEC=amr:12.2` | Puts a speech codec on the line each way (at 8000 Hz, where the AMR libraries are installed) |

V.34 between two open modems (`dsp_loopback 8 ...`, with the low-pass
filter, no bit errors):

| SNR | Rate |
|---|---|
| 40 dB | 33600 bit/s (also with near and far echo 6 and 10 dB below our signal; with ±80 ppm clock offset, 33600 one way and 31200 the other) |
| 38 dB | 31200 |
| 35 dB | 28800 |
| 33 dB | 26400 |
| 30 dB | 24000 |
| 28 dB | 21600 |
| 25 dB | 19200 |
| 22 dB | 14400 |

With `FAR_DELAY` set to 200 ms, it still runs at 33600 bit/s.

## netsim: the simulated network

`NET=<rate A>:<rate B>:<ulaw|alaw>` puts `netsim`, a simulated G.711
network, between the two modems. Each direction goes through the G.711
law at 8000 Hz. A modem at 9600 Hz is reached through a converter that
acts like a codec on an analogue line: a raised cosine at 4000 Hz, so
that PCM samples stay free of intersymbol interference. `NET_PLAIN=1`
uses dsp_util's short resampler instead, which is not free of it, so
V.90 falls back to V.34 through it.

### Analogue modems at 8000 Hz

`NET_ANALOG=a` (or `b`) with `NET=8000:8000` makes A (or B) an analogue
modem with its codec at 8000 Hz (the raised cosine sampled at
`NET_PHASE`, `NET_PPM`). `MODEM_ANALOG=<name>` (or `1`) gives that
`half_open` an analogue line (`Host::analog_line()`: it dials there, and
its pumps are the analogue modem's).

### Mobile speech codecs

`NET_CODEC=amr:12.2` (AMR at 4.75 to 12.2 kbit/s) or
`NET_CODEC=amrwb:23.85` (AMR-WB at 6.6 to 23.85 kbit/s) puts a mobile
network's speech codec on the 8000 Hz line in place of G.711. Each
direction is encoded and decoded frame by frame
(`dsp/test/codec_sim.cpp`, on the OpenCORE and VisualOn libraries:
Debian's libopencore-amrnb-dev, libopencore-amrwb-dev and
libvo-amrwbenc-dev; without them netsim says so).

- `:dtx` after the rate turns on the codecs' discontinuous transmission.
- `NET_CODEC_LOSS=<percent>` erases that share of the frames at random
  (`NET_CODEC_SEED`), as lost packets would.
- `codec_probe` runs a file of samples through the same codec, for
  measuring what it does to a signal.

What the modulations and the codecs make of each other is in
[cellular/](cellular/v_cvm.tex), a proposed Recommendation for data over
mobile telephony's speech paths ([v_cvm.pdf](v_cvm.pdf)).

### Impairments toward the 9600 Hz modem

- `NET_GAIN` sets an analogue gain on the way to the 9600 Hz modem.
- `NET_PAD=<dB>` puts a digital loss pad on that way: each code is
  re-encoded that much lower.
- `NET_RBS=<mask>` adds robbed bit signalling: in the samples whose count
  mod 6 has its bit set in the mask, the octet's least significant bit is
  forced to 1, as on T1 trunks.

Pads and robbed bits are what V.90's DIL finds ([v90a.md](dsp/v90a.md)).

The 9600 Hz modem's codec is on the network's clock, with its samples on
the network's symbols, unless:

- `NET_PHASE=<fraction>`: its samples are that fraction of a network
  sample late; or
- `NET_PPM=<ppm>`: its clock is that many parts per million fast, both
  ways.

This models a converter whose output times move with the clock (netsim's
raised cosine toward the modem, a 3900 Hz low pass from it), which V.90's
PCM receiver has to follow.

### Echoes

A modem's own echo from its line's hybrid is the modem's to add, because
netsim has the samples in blocks:

- `DSP_TEST_ECHO=<dB>:<ms>[a|c]` makes an open modem at 9600 Hz receive
  what it sent `<ms>` before, `<dB>` down, through a high pass at 300 Hz
  and a low pass at 3400 Hz (`a` or `c`: only the answerer or the
  caller).
- `NET_ECHO=<dB>:<ms>` adds the echo of a hybrid at A's line, in A's own
  sample clock: what A receives comes back with what it sends, taken
  from the line before A's codec would clip it.

The `NET_ECHO` delay has to be more than the modem at A sends ahead of
what it has received (some 20 ms for the HSF modem). Otherwise netsim
says how many samples went without their echo; an echo with gaps in it
is not one a canceller can learn.

## The fax calls and the fax modulation tests

The fax calls (`<label>_fax_*`) work like the data calls, with
`AT+FCLASS=1`. `interop_call --fax` plays the T.30 side of both fax
machines: CSI/DIS, DCS, TCF, CFR, one page (for V.17, with the short
train), EOP, MCF and DCN, at up to ten times real time (see
[Fax calls](#fax-calls)). It accepts a page with a bit error rate below
1e-3 (a fax page without ECM survives that), because the original
library makes a few bit errors with some of its own signals.

`fax_test`'s `Class1/FaxModulation.*` cases send random data through
each fax modulation over a simulated line:

- The V.17 short train codes run a long train first, so the receiver has
  an equalizer to start from.
- A few V.17 14400 cases (`_page<seed>`) send a test page's MH bits
  instead, as do `spandsp_test`'s `_page` cases from spandsp's
  transmitter.

A page's runs of the same few patterns move the receiver's loops in a way
that random data does not, so a page can lose a row where random data
gets through. The seeds chosen are pages that did lose a row before the
receiver's AGC and timing loop were slowed down in the data.
