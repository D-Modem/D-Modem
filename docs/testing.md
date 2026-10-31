# Building and testing

This page explains how to build D-Modem and run its tests. For the full
list of test programs, the simulated phone network's settings and how the
test logs work, see [the technical details](testing-details.md).

## Building

From the top folder of the project:

    cmake -B build
    cmake --build build

D-Modem builds on any processor: x86, ARM and others. The only part that
needs a 32-bit build environment is the set of tests against other
companies' modems (see below); on Debian or Ubuntu install `g++-multilib`
for those. Without it they are skipped.

D-Modem also builds for [Windows](windows.md), [macOS](macos.md) and
[Haiku](haiku.md).

## Running the tests

    ctest --test-dir build -L quick       # the fast, offline tests: a few minutes at most
    ctest --test-dir build -j16           # everything: about 1.5 minutes on 4 cores

You can also run groups of tests by label:

| Label | What it tests |
|---|---|
| `quick` | the modem against itself, the library, fax |
| `spandsp` | against the spandsp open-source modem (needs `libspandsp-dev`) |
| `smartlink` | against Smart Link's original modem library (needs a 32-bit build) |
| `agere`, `sm56`, `pctel`, `hsf`, `broadcom`, ... | against other companies' soft modems ([interop](interop.md)) |
| `open` | two D-Modems calling each other |
| `ring`, `adaptive` | calls put through as each country's exchange rings them; fax and data callers into one D-Modem that tells them apart |
| `alsa` | the daemon on simulated PC modem chips and USB modems |
| `flaky` | only the tests known to fail now and then |

To run one test on its own, give part of its name:
`ctest --test-dir build -R v34_echoes`.

`ctest --test-dir build -R doc_check` checks that every function in the
code has a comment saying what it does.

## What the tests do

Most tests connect two modems to each other through a **simulated phone
line or network**. The simulation can add noise, echoes, delay, clock
drift, the digital phone network's coding (G.711), and even a mobile
phone's speech codec. Each test then checks that the call connected at
the expected speed and that the data arrived intact.

The tests cover:

- every modem standard, on clean and noisy lines;
- error correction and compression;
- fax, including fax over the internet (T.38);
- calls against real soft modems from Agere, Motorola, PCTel, Conexant,
  Broadcom and others, running their original drivers (see
  [Interop against other soft modems](interop.md));
- the daemon on emulated hardware: PC modem chips, USB modems and the
  pico-serial sticks;
- leased lines, the wideband mode, and each country's dial and ring tones.

The calls run faster than real time where they can, so the whole suite
takes about a minute and a half.

## When a test fails

If any test fails, ctest keeps the logs of the failed tests in
`build/test-logs/`, in a folder named after when the run started
(`build/test-logs/latest` is the newest). Each failed test has its output
and both modems' logs. Runs with no failures leave nothing behind.

## Trying a line by hand

`dsp_loopback` runs two modems through a simulated line you describe on
the command line (noise level, echo, clock offset) and reports the error
rate. `interop_call` makes one call between any two of the test modems.
Both are in `build/dsp/test/`; the
[technical details](testing-details.md) explain their settings.
