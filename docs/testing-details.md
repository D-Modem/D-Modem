# Building and testing the DSP: technical details

This page gives the technical details for [Building and testing](testing.md), the plain-English overview. Read the overview first. This page keeps the full details: the names in the code, the clauses of the Recommendations, the numbers and the test lists.

## Building

    cmake -B build                            # in the top directory
    cmake --build build

i386 is not necessary. D-Modem builds natively on x86_64, ARM and
other architectures, and links with `-lm`. Clang and GCC can both build it.

Only the interop tests must have a 32-bit build (`-m32`, with 32-bit
libraries: `g++-multilib` on Debian or Ubuntu), because the original
library and the modems of the other vendors are 32-bit i386 code. Without it,
those tests do not run.

### Other C libraries

With a C library other than glibc (musl on Alpine, the C library of Windows or of Apple),
the test programs take the `rand()` sequence of glibc from
`dsp/test/libc_compat.h`. Then their noise and random fax pages are the
same values that we used to set the thresholds of the tests. With a different
sequence, some of the spandsp tests fail on musl. `libc_compat.h` is
included before each test source in `dsp/test/`. With glibc on Linux,
it is empty.

The `musl` job of CI builds and runs the offline tests on Alpine. There, the tests
must have `coreutils` (for `stdbuf`) and `tiff-dev` (for the headers of spandsp).

### Windows

A Windows build (MinGW-w64, `-DCMAKE_TOOLCHAIN_FILE=platforms/windows/mingw-w64.cmake`)
runs its tests under Wine: `ctest --test-dir build-win -L quick`. The
interop tests do not run there, because netsim is not built for Windows
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

There is one label for each peer for the interop calls:

| Label | The calls |
|---|---|
| `smartlink` | Smart Link's original library |
| `agere`, `sm56`, `pctel`, `hsf`, `broadcom`, `intel`, `ess` | the other vendors' soft modems |
| `spanpeer` | spandsp's modems |
| `hda` | the open modem on an emulated HD-audio modem codec |
| `open` | two open modems |
| `ring` | netsim putting the call through as each country's exchange does |
| `adaptive` | fax senders and data callers into the open modem answering with `AT+FAA=1` |

Other labels: `quick` (the offline tests), `spandsp` (the
transmitters and T.30 of spandsp against ours), `alsa` (dmodemd on emulated
hardware), `osmocom` (V.110 against libosmocore, when it is configured),
`flaky` and `logs` (the tests of the log fixture itself).

### The flaky tests

`cmake/flaky_tests.cmake` lists the known flaky tests. Each is an
open intermittent problem, and most fail only on a loaded machine. They
run last, and one at a time:

- All other tests must have the fixture `dmodem_steady`. Its cleanup
  test `dmodem_flaky_gate` runs after all of them have stopped.
- The flaky tests depend on `dmodem_flaky_gate` and are `RUN_SERIAL`.
- Each keeps its own labels and also has `flaky`.
- When a flaky test runs alone (`-R`), it waits for nothing.
- A separate discovery finds the flaky cases of a gtest program
  (`TEST_FILTER`, CMake 3.22 and later; with older CMake, they run with
  the others).

When you correct one of them, remove it from the list. Put a new flaky test on the
list with its task number.

### Test names

The test programs are in `dsp/test/`. The test programs of the link layer and the core are
in `link/test/` and `core/test/`. The test programs of dmodemd and the Windows driver are
in `frontend/dmodemd/test/` and `platforms/windows/winmodem/test/`.

Most of them are GoogleTest programs (the libgtest-dev of the system, or
downloaded when you configure). ctest runs each of their cases as a separate test,
with the name `<suite>/<Test>.<case>/<parameters>`, for example
`Line/PumpLoopback.DataBothWays/v34_echoes`. The names of the interop calls are
`<label>.<call>`, for example `agere.agere_v92_qc`. The other tests are
scripts and commands added directly to ctest (`doc_check`,
`v44_preset_check`, `compress_bench_check`, the `dmodemd_*` Python
tests, and other items).

To run one test alone, use `ctest -R v34_echoes`, or run the program
directly with `--gtest_filter`.

### The test programs

**The data pumps and their parts** (`dsp/test/`, label `quick` if not
noted):

| Program | What it tests |
|---|---|
| `pump_test` | The data pumps back to back (`loopback.cpp`): each modulation on clean and noisy lines, at 8000 Hz, with clock offsets, gains and echoes; the answering automode. A side passes when it is connected and has carried a minimum of 90% of the bits that its rate permits from the CONNECT, at a maximum bit error rate of 1e-3 |
| `bits_test`, `g711_test`, `pcm_cp_test` | The fields and CRCs of `bits.h`; the two laws of `g711.h` (each octet back to itself, sign and magnitude); the PCM encoders on CPs that cannot carry their rate |
| `voice_codec_test` | The sample formats of `+VSM` (`voice_codec.h`) |
| `dtmf_test` | The DTMF detector of `dsp.h` on the digits of the generator, through G.711, with twist, and by length |
| `call_tone_test`, `ring_detect_test` | The call progress tones that a voice dial hears (ringing, busy, answer tones) and the ring signal that a modem codec hears on hook (the `CallToneDetect` and `RingDetect` of `dsp.h`) |
| `ring_tones_test` | The tones of each country (`tones.cpp`, from the list of national tones of the ITU: dial, ringing, busy and congestion tones, the ring signal): the table, and the ring signal through `RingDetect` to the core, with a RING for each ring and the auto answer at S0=2 |
| `hw_test` | The hardware that the harnesses emulate (`hw/`), on a separate host: the bus masters of the ICH, and the commands and streams of the HD Audio controller with the Si3054 on it |
| `rs_test` | The Reed-Solomon code (`rs.cpp`) |
| `k56flex_test` | The data mode encoder of K56flex into its decoder at each rate, pad, law and a set of robbed-bit patterns (refer to [K56flex](dsp/k56flex-details.md#5-data-mode)) |
| `k56flex_sm56` | The same encoder into the decoder of the SM56 client of Motorola, and our training against the digital-side transmitter that it carries (label `sm56`, 32 bit builds) |
| `sms_pdu_test` | The messages of fixed-line SMS (`sms_pdu.cpp`): SMS-SUBMIT and SMS-DELIVER against known encodings, the 7-bit alphabet with its escapes, UCS2, user data headers, the length limits |
| `vcvm_test`, `vcvm_startup_test`, `vcvm_data_test` | The signal set, start-up and data mode of V.cvm, through the codecs where the AMR libraries are installed (refer to [V.cvm](dsp/vcvm-details.md#tests)) |
| `vwb_test` | V.wb, the wideband mode (`vwb.h`): its FFT, QAM and interpolator. Then a caller and an answerer on a simulated bare pair at 96000 Hz (`vwb_line.cpp`: 24 AWG cable loss k·sqrt(f)·length, the transformers, white noise, interferers, -15 dB of hybrid echo, the clocks 100 ppm apart, a dropout). No errors on 0.5 to 5 km, with the rate lower when the length increases; through interferers; for two minutes with the clock offset in each direction; a retrain after the dropout; 48000 and 192000 Hz. The test shows the rates of each pair (`VWB_DEBUG=1` adds the logs of the modems) |

**The fax** (`dsp/test/`):

| Program | What it tests |
|---|---|
| `fax_test` | The class 1 modulations, T.4 and TIFF (also against the `tiffcp` of libtiff), and two T.30 machines back to back |
| `t38_test` | T.38: the encodings of the IFP packets (the two versions), the redundancy of UDPTL, and two T.30 machines over T.38 with lost packets |
| `class1_t38_test` | Class 1 over T.38: a DTE of the test goes through T.30 with the commands of the engine against our T.30 machine over T.38. It sends and receives, with ECM, the two packet versions, and T.38 after some time on the audio. The fields and times of the packets |
| `spandsp_test` (label `spandsp`) | The V.17 transmitter and T.4 encoder of spandsp into our receivers; our T.30 against the T.30 of spandsp, on the audio and over T.38 (its T.38 terminal) |

The details of the fax tests are in [Fax](dsp/fax-details.md).

**libdmodem** (`dsp/test/`, label `quick`):

| Program | What it tests |
|---|---|
| `dmodem_test` | Pairs of modems of libdmodem: retrains and renegotiations in data, V.91 across G.711 networks, the law of the network for each call, `dmodem.hpp` |
| `pos_test` | A point of sale transaction, the host and the terminal in turn (SDLC frames through the framed sub-mode of V.80, or the plain characters of VISA II), over V.29 FastPOS, V.22 and Bell 212A fast connect and plain V.22, with its time |
| `dmodem_fax_test` | The fax of libdmodem: a data modem that answers gives the call to a fax receiver; a fax over T.38 through the API, in UDPTL with lost packets, and in IFP packets (`t38_ifp`, as the channels of Asterisk have them) of a maximum of 72 octets |
| `leased_call_test` | Leased lines under libdmodem at 9600 Hz on a simulated circuit with no exchange ([leased.md](leased.md)). Each voice band modulation on `AT&L1`; V.29, V.33 and V.27ter at each rate on `AT&L2`; the faster end that comes down; a dropout under LAPM; noise that brings V.33 down; a late answerer; a long cut and the call again; a reroute through which V.34 and V.32bis retrain (V.32bis does not with `DSP_V32_LOST=0`); a burst of noise. The loops 2 and 3 of V.54 with the self-test of V.52 (`AT&T`), refused loops, S18. The simulated carrier of V.13 (S48). V.91 on a 4-wire digital circuit (`AT&L2+MS=V91`, G.711 octets at 8000 Hz): the two laws, robbed bits, a late answerer, a short and a long cut. `LEASED_DEBUG=<level>` gives the log of libdmodem |
| `clear_call_test` | The clear 64 kbit/s channel (`AT+MS=CLEAR`): two modems on a simulated digital channel. CONNECT 64000 in the two directions; data through X.75 and through the link of the modem itself; a late answerer, no answerer, a nailed-up channel (`AT&L2`) that calls again; X.75 as T.90 and isdn4linux have it, through a dropout; the frames of PPP. `CLEAR_DEBUG=<level>` gives the log of libdmodem |
| `v110_osmo_test` (labels `quick`, `osmocom`) | V.110 against the implementation of libosmocore. Built only when `-DOSMOCORE_SRC=<libosmocore source tree>` (1.14 or later) is given |
| `vwb_call_test` | V.wb under libdmodem at 96000 Hz with `AT&L1` (S0=1 answers with no ring): bulk data in the two directions through V.42, a retrain under LAPM, NO CARRIER and the call again automatically, each end behind the buffers of a host (the USB line of dmodemd: 20 ms frames, 4672 samples ahead), and refusal at 9600 Hz |

**The link layer and the core:**

| Test | What it tests |
|---|---|
| `g729a_test` (label `quick`) | The G.729 Annex A and B of D-Modem: the packing of the frames (speech and SID), a round trip, DTX on noise and speech bursts, and each Annex A and Annex B test vector bit for bit, encoder and decoder (does not run without the vectors of `docs/specs/fetch_g729.sh`) ([v70-details.md](v70-details.md#g729-annex-a)) |
| `g729a_bcg729_test` (label `quick`, built only where libbcg729 is installed) | The coder against bcg729 in the two directions: each decodes the frames of the other as well as its own frames. bcg729 is GPL; only this test links it |
| `dsvd_audio_test` (label `quick`, not on Windows) | The `--dsvd-audio` command of dmodemd: `cat` gives back what it receives, a command that stops immediately |
| `link_test`, `core_test`, `core_equiv` (label `quick`) | The data link layer; the core; and the transcripts of the core against the `.expect` files of the scripts (from the old core of slmodemd, recorded from it; refer to [core.md](core.md)) |
| `compress_bench_check` (`compress_bench --check`) | The round trips of the compressors on text, binary, ANSI and noise at three flush intervals. `compress_bench <file>...` measures them ([link-details.md](link-details.md#compression-benchmark)) |
| `zstd_ref_test` (label `quick`) | zstd against the reference libzstd, in the two directions; built where libzstd is found |
| `v44_preset_check` (label `quick`) | The preset dictionary of V.cvm in `link/v44_preset.cpp` is Annex E of the Recommendation |
| `doc_check` (label `quick`) | Each function in the code has a comment above it |

**The interop calls** (`dsp/test/`, one program for each label):

| Program | What it tests |
|---|---|
| `smartlink_test`, `agere_test`, `sm56_test`, `pctel_test`, `hsf_test`, `broadcom_test`, `intel_test`, `ess_test`, `spanpeer_test`, `open_test`, `hda_test`, `ring_test`, `adaptive_test` | Calls between this modem and the others ([interop.md](interop.md)). `hda` is the open modem on an emulated HD-audio modem codec. In `ring`, netsim puts the call through as the exchange of each country does (dial tone, digits, ringing, busy and congestion tones). `adaptive` sends fax senders and data callers into the open modem, which answers with `AT+FAA=1`. All the calls are in one table in `interop_test.cpp`. Each program runs the calls of one label |

**dmodemd and the hardware** (`frontend/dmodemd/test/`):

| Test | What it tests |
|---|---|
| `dmodemd_alsa` (`alsa_test.py`, label `alsa`) | Two dmodemd on fake ALSA modem cards (alsa-lib plugins), which call each other through a simulated exchange: dial tone, DTMF, rings, caller ID, V.34, busy, a stall, a voice call ([hardware.md](hardware.md)). A fake HD-audio hwdep device (`fake_hda_hwdep.cpp`, by `LD_PRELOAD`) checks the line settings of the Si3054 (`-c UK`, `AT+GCI`, RZ cleared off hook, a different chip not changed). Must have libasound2-dev |
| `dmodemd_usb` (`usb_test.py`, label `alsa`) | Two dmodemd on fake SmartUSB56s (`fake_st7554.cpp`, a libusb put in front of the real one by `LD_PRELOAD`) on the same exchange: the checks of the device, rings, V.34, busy, a voice call, disconnection ([hardware.md](hardware.md)). Must have libusb-1.0-0-dev |
| `dmodemd_agere` (`agere_test.py`, label `alsa`) | dmodemd on two fake Agere USB 2.0 soft modems (`fake_agere.cpp`) on the same exchange: the stream of the ROM, rings and caller ID, a call in each direction with text, pulse dialling, disconnection |
| `dmodemd_pico_line` (`pico_line_test.py`, label `alsa`) | The same on fake pico-serial Line sticks (`fake_pico_line.cpp`), with caller ID on hook, and the line settings of the country written to the DAA of the stick (`-c UK`, then `AT+GCI`) ([hardware.md](hardware-details.md#pico-serial-line)). Must have libusb-1.0-0-dev |
| `dmodemd_pico_wb` (`pico_wb_test.py`, labels `alsa` and `quick`) | Two dmodemd on fake pico-serial WB sticks (`fake_pico_wb.cpp`) at the two ends of a simulated bare pair, in real time at 96 kHz (`wb_pair.cpp`, built on `dsp/test/vwb_line.cpp`): the rates and gains of the stick, V.wb by default, `AT&L1`, a CONNECT above 50000 bit/s (148656 on this pair), the LINK LEDs, 32 kB in each direction through V.42, the hang-up, disconnection. It takes about 20 s and runs alone (`RUN_SERIAL`) ([hardware.md](hardware-details.md#pico-serial-wb-wideband-bare-pair)). Must have libusb-1.0-0-dev |
| `dmodemd_*_builtin` (label `alsa`) | On macOS (which has no `LD_PRELOAD`), and on Linux with `-DDMODEM_TEST_BUILTIN_FAKES=ON`: the USB, pico Line, pico WB and Agere tests with each fake linked into a separate dmodemd (`dmodemd_fake_st7554`, ...) instead of libusb. On Linux, their names are `dmodemd_usb_builtin` and similar names. On macOS, they keep the plain names |
| `dmodemd_usb_windows`, `dmodemd_pico_line_windows` (label `alsa`, a Windows build under Wine) | The two tests above with dmodemd.exe and the fake linked instead of libusb (`dmodemd_fake_*.exe`), its DTE on TCP, and the connections of the fakes relayed over TCP to the exchange of a Linux build (`usb_dte.py`, `fake_net.h`) ([windows.md](windows-details.md#usb-modems---usb)) |
| `dmodemd_clearmode` (`clearmode_test.py`, label `quick`) | The clear 64 kbit/s channel of dmodemd over SIP (the CLEARMODE of RFC 4040) against the SIP peer and clear channel of the script, with calls out and in |
| `dmodemd_srtp` (`clearmode_test.py --srtp`, label `quick`) | The same calls in SRTP; does not run in a build without SRTP |
| `dmodemd_red` (`clearmode_test.py --red`, label `quick`) | The same calls over a lossy trunk with RFC 2198 redundancy (the assured mode of V.152): `red/8000` and `a=maxmptime` in the offer and answer, one packet in three lost in each direction, and each lost packet made again from the subsequent packets |
| `dmodemd_v150` (`v150_test.py`, label `quick`) | dmodemd with `SIP_V150=yes` against the SIP peer and V.150.1 gateway (SSE, SPRT) of the script, with calls out and in: the SDP lines, ANSam as events, `SSE:M`, `INIT`, `JM_INFO`, `CONNECT 33600`, a line in each direction, `CLEARDOWN` ([v150-details.md](v150-details.md#tests)) |
| `V8bis.*` (`v8bis_test.cpp`, label `quick`) | The messages of V.8 bis against their octets, the parameter trees parsed past the parts that are not read, the signals through their detector, the messages through V.21 and HDLC ([v8bis-details.md](dsp/v8bis-details.md#tests)) |
| `Core.Dsvd*` (`core_test.cpp`, label `quick`) | Two modems with `AT+A8E=1,1,,1+DSVD=1`: V.8 bis selects DSVD, the data channel of V.76 carries the characters of the DTEs in the two directions, and the hang-up closes it; DSVD at the caller only: data and V.42; `Core.DsvdVoice`: with a voice path at the two ends, each end hears the tone of the other through G.729 Annex A together with the data, and the voice channel closes at hang-up ([v70-details.md](v70-details.md#tests)) |
| `Core.V8bis*` (`core_test.cpp`, label `quick`) | Two modems with `AT+A8E=1,1,,1` at the two ends, or at one end: the transaction (MRe, CRd, CL, MS, ACK(1)) then V.8, or V.8 without it ([v8bis-details.md](dsp/v8bis-details.md#tests)) |
| `RtpRed.*` (`rtp_red_test.cpp`, label `quick`) | RFC 2198 redundancy (`rtp_red.h`): RED payloads built and read back, the layout of the RFC itself, timestamps that wrap, blocks that a header cannot hold, malformed payloads, and `a=fmtp` values |
| `V150.*` (`v150_test.cpp`, label `quick`) | The codings of V.150.1, SPRT over a lossy network (sequence, acknowledgements, windows, retransmission) and two modem relay sessions: the start, the selected modulation, data in the two directions, the hang-up, compression refused ([v150-details.md](v150-details.md#tests)) |
| `dmodemd_misdn` (`misdn_test.py`, label `quick`) | Two dmodemd on fake mISDN cards, which call each other through the ISDN network of the script: a clear channel call, an audio call at V.22bis, busy, no answer |
| `dmodemd_root` (`root_test.py`, label `quick`) | dmodemd started as root: it continues as nobody, saves its data file, and the group and link of its pty (kept by its own process) follow the new pty after the DTE hangs up, and are removed when it stops, also when it is killed. Does not run if ctest does not run as root (CI runs it with sudo) |
| `hda_modem_setup` (`hda_modem_setup_test.py`, label `quick`) | `hda-modem-setup` on a fake sysfs and kernel configuration: the modem codecs listed with their drivers; the early patch (the vendor ID of the Si3054) and its module option written for a codec with no driver outside the list of snd-hda-codec-si3054, or for a named codec; refused without the patch loader; a warning for a different `patch=` option; the two files removed |
| `Si3056.*` (`si3056_test.cpp`, label `quick`) | The country settings of the Si3056 datasheet (`si3056.h`): the names, and the registers that they give on a Si3018 and a Si3019 line side, with the other bits kept |
| `Si3054.*` (`si3054_test.cpp`, label `quick`) | The country settings of the Si3054 datasheet (`si3054.h`): the names, the chip ID check, the registers on and off hook, with the other bits kept |

**The Windows driver** (`platforms/windows/winmodem/test/`):

| Test | What it tests |
|---|---|
| `PortTest.*`, `Wire.*` (`port_test.cpp`, label `quick`) | The COM port of the virtual modem driver (`platforms/windows/winmodem/port.h`): reads under each type of serial timeout, writes held and timed, the events of `WaitCommEvent`, the lines of the modem and the DTR and RTS of the application, `LSRMST_INSERT`, purges, the settings; the frames to dmodemd ([windows.md](windows-details.md#the-virtual-modem-driver---driver)) |
| `VirtualModem.*` (`vmodem_test.cpp`, label `quick`, Windows under Wine) | dmodemd.exe on the pipe of the driver (`--driver`), with the end of the driver (port and pipe) in the test: AT commands and the settings of the INF, DSR that follows dmodemd, the driver that comes back, `--driver auto`, a name that is already used; its `bridge` for the `WIN_DRIVER=1` of `frontend/asterisk/test/sip.sh` |
| `umdf1_test` (label `quick`, Windows) | The driver for Windows 7 and 8 (`dmodemvm7.dll`, UMDF 1) in a host of the test, with dmodemd.exe on its pipe |
| `winmodem_inf`, `dmodem_driver_list` (label `quick`) | `dmodem.inf` as `platforms/windows/winmodem/make_inf.py` makes it from the result codes of the core; `dmodem-driver.exe list` (Windows) |

## Test logs

If a ctest run has a failure, it keeps the logs of the failed tests in
a separate directory under `build/test-logs/`. The name of the directory is the start
time of the run (`latest` points to the newest). A run without a failure does not keep
logs there. Skipped tests are not failures.

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

This operates with plain ctest, also with `-R`, `-L` or `--repeat`. A
fixture does it (`cmake/test_logs.cmake`):

1. `test_logs_begin` makes the directory of the run before the first test.
2. Each test runs under `cmake/test_run.sh`, which keeps its output and
   also gives it to ctest. A test finds its directory in
   `DMODEM_TEST_DIR`.
3. When a test passes, the launcher deletes its directory immediately. A test
   without its `result.txt` was killed (for example at its ctest timeout)
   and counts as failed.
4. `test_logs_end` writes the summary after the last test. If the run
   stopped without a failure, it deletes the directory of the run and points
   `latest` at the newest run that stays.

Settings:

- `DMODEM_TEST_KEEP_PASSED=1` keeps all logs (approximately 100 MB for a full
  run).
- The 20 latest runs are kept (`-DDMODEM_TEST_LOG_KEEP=<n>`).
- `-DDMODEM_TEST_LOG_ROOT=<dir>` puts them in a different location.

### Debug levels

Each run records much log data. The launcher sets the debug level of each
modem and library that the tests run to `DMODEM_TEST_DEBUG` (5 by default),
where it is not already set:

- `DSP_DEBUG` (the pumps), `CORE_DEBUG`, `DMODEM_DEBUG` (libdmodem),
  `DBG` (the modems of the interop calls), `DMODEMD_DEBUG` (the `-d` of dmodemd);
- `LINK_DEBUG` and `SPANDSP_DEBUG` are enabled.

At level 5, the modems log each step of a call, with the values of their
equalizers and retrains. Levels 6 and higher log each
bit and symbol (`DSP_DEBUG=6 ctest -R ...` for one test).
`DMODEM_TEST_DEBUG=0` keeps the levels of the tests themselves. With all of this, a
full run takes about a tenth more time.

A call that you run manually (`interop_call`, or a test program outside ctest)
writes its modem logs to `LOG_DIR`, or to `build/dsp/test/logs/<test>/`
for the test programs.

## How the calls keep time

The interop calls run as fast as the modems can. Each modem keeps time
by the samples of its line. The Agere, SM56, PCTel, HSF and Broadcom harnesses
count jiffies and timers in samples. Our core, under the open pumps
or the original library, does this with `PIPE_SYNC`. Thus, a call goes as it
would in real time, only sooner, and a busy machine only makes it slower.

The seconds of the call itself (`STREAM`, `MIDCALL`, `WAIT`, `ANSWER_AFTER`,
`HANGUP_WAIT`, and the time that the driver waits for the messages of the modems)
are then the seconds of the line, from the clock of netsim (`NET_CLOCK_FD`). They
run faster or slower than the wall clock, as the machine permits. The
wall clock is used only while the line does not move (a modem has stopped).

Three things keep such a call the same from run to run, also when the
driver answers late:

- **On hook, our modem and the original library always run in real time**
  (`PIPE_SYNC`). Thus, the commands of the driver meet the samples that they would meet in
  real time. The call itself, from the dial or the answer, runs as fast
  as it can. Their line also runs from the start, as the lines of the other harnesses
  do: the lead silence is written when they start, not at the first
  `ATD` or `ATA`. A line that did not move until then accepted the commands as
  they came, and the call started at the positions where the scheduler had the two
  modems at that time.
- **The driver waits 100 ms of the line after a CONNECT before it sends
  data** (`DATA_WAIT`). The SM56 shows CONNECT a little before it accepts
  data, and echoes as a command line all data that comes sooner.
- **The data around the mid-call commands of a call (`MIDCALL`) is kept**:
  before the OK of the escape, and immediately after the CONNECT of `ATO`.

### Fax calls

The fax calls (`interop_call --fax`) keep time by the relay of the line by the driver
(the samples of A at `LINE_RATE`: 8000, or 9600 with the original
library). They go at a maximum of ten times as fast as real time (`MODEM_SPEED=10`,
for `half_open` and `half_smartlink`):

- The fax modems use so little processor time that the line would
  otherwise run some hundred times as fast. Then the milliseconds that the driver
  takes to answer a modem (it does T.30 for the two machines) would be
  most of a second of the line, more than the turnarounds of T.30.
- The modems accept the commands of the DTE each 192 samples (`PIPE_SYNC=192`:
  20 ms at 9600 Hz, the frame of the original library). The V.17 receiver of the original
  library accepts a short train or not because of how its frames
  fall on it. Thus, our training must start at the same point of those
  frames each time.

Thus, the calls use the same turnarounds as the real-time calls, and give
the same TCF and pages, in a tenth of the time.

The relay also holds the line while the driver has not yet answered a
message of a modem (from the output of the modem until the next poll or
write of the driver). Also when the scheduler lets the driver run late, its answer meets
the line at the position where an immediate answer would meet it. (Before this, the `+FTH` of the Agere modem
sometimes stopped with ERROR, because its frame arrived some seconds of
line time after its CONNECT.)

### Parallel runs

The calls run a little nicer (`nice` 10) than the remaining processes of the machine.
Each takes about one processor while it runs, but not during the moments on
hook in real time. Thus, `-j` can be about four times the number of
processors. On 4 processors, the full suite takes approximately 75 s at `-j16`
(105 s at `-j8`; 13 minutes when all calls ran in real time). With much more
than that (`-j32` on 4), the calls on hook become slower than real time.
None of the calls must run alone.

`MODEM_REALTIME=1 ctest ...` runs all calls in real time, as they all
did before (this helps when you compare a failure).

## Tools for trying a line by hand

The test programs are in `build/dsp/test/`.

### interop_call

The calls go through `interop_call.cpp`. The `interop_call` program
makes one call manually:

    interop_call <caller> <answerer> ["ATX3;AT+MS=122"]
    interop_call --fax <sender> <receiver> [mod] [AT setup]

The settings of the call come from the environment (`interop_call.h`: `NET`,
`STREAM`, `BULK`, `CALLS`, `EXPECT_B`, ...; `NETSIM` for the path of netsim
and `LOG_DIR` for the logs, by default the current directory).

A call connects two modem processes with pipes: `half_open` (this code on
the modem core, `../core`) and the harness of a different modem, or a second
`half_open` (the `open` label). Each goes through the full controller of the modem:
AT commands, V.42/LAPM detection and data in the two
directions. `half_open8` is the open modem at 8000 Hz (the original
library does not operate at 8000 Hz). The other modems, their harnesses and
the results of calls with them are in [interop.md](interop.md): the original
library of Smart Link (`half_smartlink`), the Agere, SM56, PCTel, HSF,
Broadcom, Intel and ESS soft modems, and the modems of spandsp.

### dsp_loopback

    dsp_loopback <dp> [snr dB] [seconds] [srate] [line gain] [clock ppm]
                 [echo dB] [far echo dB]

`dsp_loopback` runs a caller and an answerer through a simulated line
with delay, a low-pass filter, white noise and an optional sample clock
offset. Then it checks the bit error rate in the two directions. It is the
simulation that `pump_test` uses, to try a line from the command line.

| Setting | Effect |
|---|---|
| `[echo dB]` | An echo of the signal of each modem (the echo of the hybrid, about 2 ms) at that level |
| `[far echo dB]` | The hybrid echo of the far end, after a round trip over the line |
| `FAR_DELAY=<samples>` | Delays the far echo more, as over VoIP |
| `LINE_DELAY=<samples>[:<samples>]` | More delay toward the answerer (and toward the caller, if a second value is given) |
| `CALLER_DP=<id>` | Gives the caller a different pump from the answerer (automode) |
| `FLAT=1` | Removes the low-pass filter |
| `DUMP=<prefix>` | Records what each side hears |
| `CODEC=amr:12.2` | Puts a speech codec on the line in each direction (at 8000 Hz, where the AMR libraries are installed) |

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
law at 8000 Hz. A modem at 9600 Hz connects through a converter that
operates as a codec on an analogue line: a raised cosine at 4000 Hz. Thus,
the PCM samples stay free of intersymbol interference. `NET_PLAIN=1`
uses the short resampler of dsp_util instead, which is not free of it. Thus,
V.90 falls back to V.34 through it.

### Analogue modems at 8000 Hz

`NET_ANALOG=a` (or `b`) with `NET=8000:8000` makes A (or B) an analogue
modem with its codec at 8000 Hz (the raised cosine sampled at
`NET_PHASE`, `NET_PPM`). `MODEM_ANALOG=<name>` (or `1`) gives that
`half_open` an analogue line (`Host::analog_line()`: it dials there, and
its pumps are the pumps of the analogue modem).

### Mobile speech codecs

`NET_CODEC=amr:12.2` (AMR at 4.75 to 12.2 kbit/s) or
`NET_CODEC=amrwb:23.85` (AMR-WB at 6.6 to 23.85 kbit/s) puts the speech codec of a mobile
network on the 8000 Hz line instead of G.711. Each
direction is encoded and decoded frame by frame
(`dsp/test/codec_sim.cpp`, on the OpenCORE and VisualOn libraries:
the libopencore-amrnb-dev, libopencore-amrwb-dev and
libvo-amrwbenc-dev of Debian; without them, netsim tells this).

- `:dtx` after the rate enables the discontinuous transmission of the codecs.
- `NET_CODEC_LOSS=<percent>` erases that percentage of the frames at random
  (`NET_CODEC_SEED`), as lost packets would.
- `codec_probe` runs a file of samples through the same codec, to
  measure its effect on a signal.

The effects of the modulations and the codecs on each other are in
[cellular/](cellular/v_cvm.tex), a proposed Recommendation for data over
the speech paths of mobile telephony ([v_cvm.pdf](v_cvm.pdf)).

### Impairments toward the 9600 Hz modem

- `NET_GAIN` sets an analogue gain on the path to the 9600 Hz modem.
- `NET_PAD=<dB>` puts a digital loss pad on that path: each code is
  encoded again that much lower.
- `NET_RBS=<mask>` adds robbed bit signalling. In the samples whose count
  mod 6 has its bit set in the mask, the least significant bit of the octet is
  forced to 1, as on T1 trunks.

The DIL of V.90 finds pads and robbed bits ([v90a.md](dsp/v90a.md)).

The codec of the 9600 Hz modem is on the clock of the network, with its samples on
the symbols of the network, except in these conditions:

- `NET_PHASE=<fraction>`: its samples are that fraction of a network
  sample late; or
- `NET_PPM=<ppm>`: its clock is that many parts per million fast, in the two
  directions.

This models a converter whose output times move with the clock (the
raised cosine of netsim toward the modem, a 3900 Hz low pass from it). The PCM receiver of V.90
must follow it.

### Echoes

The modem must add its own echo from the hybrid of its line, because
netsim has the samples in blocks:

- `DSP_TEST_ECHO=<dB>:<ms>[a|c]` makes an open modem at 9600 Hz receive
  what it sent `<ms>` before, `<dB>` down, through a high pass at 300 Hz
  and a low pass at 3400 Hz (`a` or `c`: only the answerer or the
  caller).
- `NET_ECHO=<dB>:<ms>` adds the echo of a hybrid at the line of A, in the
  sample clock of A: what A receives comes back with what it sends. netsim takes it
  from the line before the codec of A would clip it.

The `NET_ECHO` delay must be more than the time by which the modem at A sends ahead of
what it has received (approximately 20 ms for the HSF modem). If not, netsim
tells how many samples went without their echo. A canceller cannot learn an echo
with gaps in it.

## The fax calls and the fax modulation tests

The fax calls (`<label>_fax_*`) operate as the data calls, with
`AT+FCLASS=1`. `interop_call --fax` does the T.30 side of the two fax
machines: CSI/DIS, DCS, TCF, CFR, one page (for V.17, with the short
train), EOP, MCF and DCN, at up to ten times real time (refer to
[Fax calls](#fax-calls)). It accepts a page with a bit error rate below
1e-3 (a fax page without ECM can accept that), because the original
library makes some bit errors with some of its own signals.

The `Class1/FaxModulation.*` cases of `fax_test` send random data through
each fax modulation over a simulated line:

- The V.17 short train codes run a long train first. Thus, the receiver has
  an equalizer from which to start.
- Some V.17 14400 cases (`_page<seed>`) send the MH bits of a test page
  instead, as do the `_page` cases of `spandsp_test` from the transmitter of spandsp.

The series of the same few patterns on a page move the loops of the receiver in a way
that random data does not. Thus, a page can lose a row where random data
goes through. The selected seeds are pages that lost a row before we made the
AGC and timing loop of the receiver slower in the data.
