# Building and testing

This page tells how to build D-Modem and run its tests. For the full
list of test programs, the settings of the simulated phone network and how the
test logs operate, refer to [the technical details](testing-details.md).

## Building

From the top folder of the project:

    cmake -B build
    cmake --build build

D-Modem builds on any processor: x86, ARM and others. Only one part
must have a 32-bit build environment: the tests against the modems of other
companies (refer to the text below). For these tests, on Debian or Ubuntu, install `g++-multilib`.
Without it, these tests do not run.

D-Modem also builds for [Windows](windows.md), [macOS](macos.md) and
[Haiku](haiku.md).

## Running the tests

    ctest --test-dir build -L quick       # the fast, offline tests: a few minutes at most
    ctest --test-dir build -j16           # everything: about 1.5 minutes on 4 cores

You can also run groups of tests by label:

| Label | What it tests |
|---|---|
| `quick` | the modem against itself, the library, fax |
| `spandsp` | against the spandsp open-source modem (must have `libspandsp-dev`) |
| `smartlink` | against the original modem library of Smart Link (must have a 32-bit build) |
| `agere`, `sm56`, `pctel`, `hsf`, `broadcom`, ... | against the soft modems of other companies ([interop](interop.md)) |
| `open` | two D-Modems that call each other |
| `ring`, `adaptive` | calls that the exchange of each country rings; fax and data callers into one D-Modem that finds the difference between them |
| `alsa` | the daemon on simulated PC modem chips and USB modems |
| `flaky` | only the tests that we know fail at random times |

To run one test alone, give part of its name:
`ctest --test-dir build -R v34_echoes`.

`ctest --test-dir build -R doc_check` makes sure that each function in the
code has a comment that tells what it does.

## What the tests do

Most tests connect two modems through a **simulated phone
line or network**. The simulation can add these items:

- Noise, echoes, delay and clock drift.
- The coding of the digital phone network (G.711).
- The speech codec of a mobile phone.

Then each test makes sure that the call connected at
the expected speed, and that the data arrived without errors.

The tests cover these items:

- all modem standards, on clean and noisy lines;
- error correction and compression;
- fax, which includes fax over the internet (T.38);
- calls against real soft modems from Agere, Motorola, PCTel, Conexant,
  Broadcom and others, which run their original drivers (refer to
  [Interop against other soft modems](interop.md));
- the daemon on emulated hardware: PC modem chips, USB modems and the
  pico-serial sticks;
- leased lines, the wideband mode, and the dial and ring tones of each country.

Where possible, the calls run faster than real time. Thus, the full suite
takes about one and a half minutes.

## When a test fails

If a test fails, ctest keeps the logs of the failed tests in
`build/test-logs/`, in a folder with the start time of the run as its name
(`build/test-logs/latest` is the newest). For each failed test, the folder has its output
and the logs of the two modems. Runs with no failures do not keep logs.

## Trying a line by hand

`dsp_loopback` runs two modems through a simulated line. You give the properties of
the line on the command line (noise level, echo, clock offset). It reports the error
rate. `interop_call` makes one call between any two of the test modems.
The two programs are in `build/dsp/test/`. The
[technical details](testing-details.md) tell about their settings.
