# Interop against other soft modems

A modem is useful only if it can communicate with other modems. To check this,
the tests of D-Modem call real modems from other companies. The tests use the original
driver software of these modems, which runs inside the test suite. A simulated phone line
connects the driver to D-Modem. Each call makes sure that the two modems connect and
that data (or a fax page) arrives without errors.

For how each modem runs, the exact driver files, and each result,
refer to [the technical details](interop-details.md).

## The modems it is tested against

Most of these modems were "soft modems" sold with PCs and laptops in the 2000s.
In a soft modem, driver software does the work of the modem, not a chip.
The tests run the original driver of each company, without changes, on an
emulation of the hardware that the driver expects.

| Modem | Driver used | Highest result with D-Modem |
|---|---|---|
| Smart Link | its original Linux library (the one D-Modem began from) | V.90 at 54666 bit/s, and V.92's features |
| Agere | its Linux driver | V.34 both ways, V.90 at 46666 bit/s |
| Motorola SM56 | its Linux driver | V.90 at 50666 bit/s |
| PCTel HSP | its Linux driver | V.90 at 50666 bit/s |
| Conexant HSF | its Linux driver | V.92 at 52000 down, 41333 up |
| Intel 537EP | its Linux driver | V.90 at 53333 bit/s |
| Broadcom | its Windows driver, on a small Windows kernel written for the tests | V.92 quick connect, V.90 at 50666 bit/s |
| ESS | its Windows driver, on the same small Windows kernel | V.92 at 49333 down, 41333 up |
| spandsp | the open-source library used by Asterisk and FreeSWITCH | its speeds, up to V.22bis (2400 bit/s) |
| USRobotics Courier V.Everything | the whole modem emulated, running its own firmware (a 1998 flash image) | HST at 9600 bit/s with HST's own link, the channels turning around; V.32bis at 14400 bit/s with V.42 |

In the V.90 and V.92 calls, D-Modem is the "digital" end. This is the role that the
equipment of an internet provider had. Each modem also connects at the
older standards (V.34, V.32bis, V.22bis, V.21 and other standards). The fax
modems send fax pages in the two directions.

The Courier is a desktop modem from the 1990s, not a soft modem. Its own
processors do its work. The tests emulate all of it (its
80C186 controller, its TMS320C51 signal processor and the chips between
them). They run the firmware of USRobotics on it. Thus, the HST,
V.32terbo and V.32bis of the Courier itself go on the line. Two emulated Couriers call each
other at HST 16800 and V.32terbo 21600. D-Modem calls a Courier, and a Courier
calls D-Modem, in HST, V.32bis and V.22bis. V.FC and V.34 do not
operate yet.

The test suite also runs D-Modem on an emulated HD-audio modem
chip (the type in laptops from about 2005 to 2010). This tests the code
that operates real hardware.

## What else is tested

- **Ringing like a real exchange.** The simulated network can operate
  as the telephone exchange of a given country: dial tone, dialled digits,
  ringing, busy and congestion tones.
- **Fax or data?** D-Modem answers with `AT+FAA=1`. It must find
  if each modem that calls it is a fax machine or a data modem.
- **MNP error correction.** We test the older MNP protocols, which modems
  without V.42 use, with all modems that have them.
- **V.80 synchronous mode.** Video phones (H.324) and some other
  programs use this mode. We test it with the modems that support it.
- **Call statistics, retrains and speed changes** during a
  call, when the user asks for them.

## Running the tests

Each modem has its own test label:

    ctest --test-dir build -L agere -j16

The other labels are `smartlink`, `sm56`, `pctel`, `hsf`, `intel`,
`broadcom`, `ess`, `courier`, `spanpeer`, `hda` and `open` (D-Modem
against itself). There are also `ring` (the tones of the exchange) and `adaptive` (fax or
data?). These tests must have a 32-bit build environment, because the old drivers
are 32-bit programs. Refer to [Building and testing](testing.md).

The drivers and firmware are the property of their manufacturers. We keep them only as
test partners. They are not part of D-Modem ([licensing notice](../dsp/test/NOTICE.md)).
