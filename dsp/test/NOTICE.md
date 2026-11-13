# The interop test binaries

The files below are closed drivers and objects of other companies' soft
modems. They are in this repository only so that the interop tests can run
D-Modem against those modems ([docs/interop.md](../../docs/interop.md)).

**Each of them is the property of its owner and is licensed by its owner,
under its owner's terms. None of them is subject to the terms of the rest
of this repository: not to the GNU Affero General Public License, not to
D-Modem's commercial licence, and not to any other licence or permission
given here. No rights in them are granted by this repository.**

D-Modem's own programs and libraries (dmodemd, libdmodem, the Asterisk and
FreeSWITCH modules) neither contain nor link any of them; only the test
programs in this directory load them.

`usr/courier-nvram.bin` is not a copy of anyone's part: it was made with
the emulated Courier (`half_courier`), by the firmware's own `AT&F` and
`AT&W` on an erased part, with the settings records that enable the
Courier's options written in.

| File | Owner | From |
|---|---|---|
| `smartlink/dsplibs.o` | Smart Link Ltd. | Debian's `sl-modem` 2.9.11~20110321 |
| `agere/agrsm_core.o` | Agere Systems | agrsm-20070804 (Agere SoftModem 2.1.40) |
| `sm56/sm56.ko` | Motorola, Inc. | sm56-06.09.05 for Fedora Core 3 |
| `pctel/dsp.o`, `pctel/control.o` | PCTel, Inc. | pctel-0.9.7-9-rht-6 |
| `hsf/hsfengine.o`, `hsf/hsfmc97ich.o`, `hsf/hsfsoar.o`, `hsf/hsfusbcd2.o`, `hsf/hsfmc97ich.inf`, `hsf/hsfcadmus2.inf`, `hsf/hsfcadmus2smart.inf`, `hsf/hsf.cty` | Conexant Systems (formerly Rockwell Semiconductor Systems) | Linuxant's hsfmodem-7.80.02.06oem |
| `intel/537core.lib`, `intel/wwh_dflt.c` | Intel Corporation | Intel537EP 2.37.50.1 for Fedora Core 1 (Intel's licence, `intel/license.txt`) |
| `broadcom/BCMSM.sys`, `broadcom/BCMSMwxp.inf` | Broadcom Corporation | Dell's "V.92 Soft Modem (Data Fax) Driver" A05 (BCM V.92 56K Modem 3.4.19) |
| `ess/es56hpi.sys`, `ess/mdmzn.inf` | ESS Technology, Inc. | ESS's ES56H-PI modem driver 5.43.070 (`ess54370.exe`) |
| `usr/IDSDL302.ROM` | U.S. Robotics (later 3Com); modified by Igor Diagilev (ID_SDL) | the flash image of a Courier V.Everything: ID_SDL v3.02 of 06/06/98, supervisor 7.3.14 and DSP 3.0.13 of 03/13/98 |
| `usr/c51-mask.rom` | Texas Instruments and U.S. Robotics | the on-chip ROM of a Courier's TMS320C51, read out of a 20.16 MHz board |

## The ITU's list of national tones

`tones.cpp` is generated from the ITU's list of the tones used in
national networks (Annex to ITU Operational Bulletin No. 955, position on
1 May 2010, after Recommendation E.180), which the ITU publishes freely
at https://www.itu.int/dms_pub/itu-t/opb/sp/T-SP-E.180-2010-MSW-E.doc:
each country's dial, ringing, busy and congestion tones and their
cadences, as data. The list itself is not
in this repository; `tones_gen.py` reads it.
