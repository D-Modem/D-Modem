# Interop against other soft modems

A modem is only useful if it can talk to other modems. To check this,
D-Modem's tests call real modems from other companies: their original
driver software, run inside the test suite, connected to D-Modem through
a simulated phone line. Each call checks that the two modems connect and
that data (or a fax page) gets through intact.

For how each modem is run, the exact driver files, and every result,
see [the technical details](interop-details.md).

## The modems it is tested against

Most of these were "soft modems" sold with PCs and laptops in the 2000s,
where the modem's work is done by driver software rather than a chip.
The tests run each company's original driver, unchanged, on an
emulation of the hardware it expects.

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

In the V.90 and V.92 calls D-Modem plays the "digital" end, the role an
internet provider's equipment played. Every modem also connects at the
older standards (V.34, V.32bis, V.22bis, V.21 and so on), and the fax
modems send fax pages both ways.

The test suite also runs D-Modem itself on an emulated HD-audio modem
chip (the kind in laptops from about 2005 to 2010), to test the code
that drives real hardware.

## What else is tested

- **Ringing like a real exchange.** The simulated network can behave
  like a given country's telephone exchange: dial tone, dialled digits,
  ringing, busy and congestion tones.
- **Fax or data?** D-Modem answers with `AT+FAA=1` and must work out
  whether each modem calling it is a fax machine or a data modem.
- **MNP error correction.** The older MNP protocols, used by modems
  without V.42, are tested with every modem that has them.
- **V.80 synchronous mode,** used by video phones (H.324) and some other
  programs, is tested with the modems that support it.
- **Call statistics, retrains and speed changes** in the middle of a
  call, asked for by the user.

## Running the tests

Each modem has its own test label:

    ctest --test-dir build -L agere -j16

The other labels are `smartlink`, `sm56`, `pctel`, `hsf`, `intel`,
`broadcom`, `ess`, `spanpeer`, `hda` and `open` (D-Modem against
itself), plus `ring` (the exchange's tones) and `adaptive` (fax or
data?). These need a 32-bit build environment, because the old drivers
are 32-bit programs. See [Building and testing](testing.md).

The drivers belong to their makers and are kept only as test partners;
they are not part of D-Modem ([licensing notice](../dsp/test/NOTICE.md)).
