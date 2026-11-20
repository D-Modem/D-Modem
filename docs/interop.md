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
| 3Com Megahertz TC110 PC Card | its signal processor emulated, running the 3Com flash firmware | V.34 at 33600 bit/s (with `S33=0`), 31200 bit/s and 4800 bit/s; V.42 and V.42bis; V.32bis at 14400 bit/s; ETC with a cellular cable |

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
calls D-Modem, in HST, V.32bis and V.22bis. D-Modem also calls a Courier in
V.FC. The start-up completes and the two modems send CONNECT 26400. But the
Courier cannot read the data of D-Modem yet. V.34 does not operate yet.

The 3Com TC110 is a PC Card modem with a TMS320C5x signal processor. The
tests emulate this processor and run the 3Com flash firmware on it. D-Modem
calls the TC110 in V.34, and the TC110 calls D-Modem. The two modems
complete the training and send CONNECT. The factory setting of the TC110 is `S33=32`. This setting stops
the 3429 baud symbol rate, thus the maximum rate is 31200 bit/s. With
`ATS33=0`, the two modems connect at 33600 bit/s, the maximum rate of the
TC110. At 33600 bit/s and at 4800 bit/s, the tests send data in the two
directions, and the two modems receive the data correctly. When `AT+MS`
sets a maximum rate, the MP sequence of D-Modem shows only the rates up to
that maximum. If the MP sequence shows faster rates, the TC110 starts a
rate renegotiation every 6 seconds to get a faster rate, and some data is
lost each time.

With error correction, the two modems connect with V.42 (LAPM) and
V.42bis in the two directions. When the TC110 answers, it waits for the
detection patterns (ODP) of V.42 after V.8. Thus, D-Modem sends them
also after V.8. With `S32=60` (no V.34), the two modems connect in
V.32bis at 14400 bit/s in the two directions. The 4800 to 12000 bit/s
rates of V.32bis also operate.

Without error correction, the D-Modem receiver now gives ones to the DTE
for the B1 signal at the start of the data (V.34 and V.32bis). Before,
the first frames from the TC110 gave two or three incorrect octets. The
TC110 itself gives about seven incorrect octets to its DTE at the start
of V.32bis data. This also occurs between two TC110s, thus it is the
operation of its firmware. To disable error correction on D-Modem, use
`\N0`. `&M0` does not change it. With `&M0`, D-Modem sends the
detection patterns of V.42 (0x11 and 0x91) for T400, and a remote modem
with no error correction gives them to its DTE.

The tests can also attach an emulated cellular cable to the TC110
(`TC110_CABLE=200000`, the ID of a Motorola cable in the EEPROM of the
cable). Then the TC110 shows "Motorola Cable" and goes into the
cellular mode of ETC (V.32bis only, error correction necessary). The
TC110 with the cable calls D-Modem in `AT&F6` (the land side of ETC).
D-Modem in `AT&F5` (the cellular side, with its 1500 Hz calling tone)
calls the TC110 without the cable, and the TC110 finds the calling
tone. In the two calls, the modems connect in V.32bis at 14400 bit/s
with V.42 and frames of 64 octets (N401), and the TC110 shows
`CONNECT 14400/ARQ/ETC/V32/LAPM/V42BIS`.

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
`broadcom`, `ess`, `courier`, `3com`, `spanpeer`, `hda` and `open` (D-Modem
against itself). There are also `ring` (the tones of the exchange) and `adaptive` (fax or
data?). These tests must have a 32-bit build environment, because the old drivers
are 32-bit programs. Refer to [Building and testing](testing.md).

The drivers and firmware are the property of their manufacturers. We keep them only as
test partners. They are not part of D-Modem ([licensing notice](../dsp/test/NOTICE.md)).
