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

| File | Owner | From |
|---|---|---|
| `smartlink/dsplibs.o` | Smart Link Ltd. | Debian's `sl-modem` 2.9.11~20110321 |
| `agere/agrsm_core.o` | Agere Systems | agrsm-20070804 (Agere SoftModem 2.1.40) |
| `sm56/sm56.ko` | Motorola, Inc. | sm56-06.09.05 for Fedora Core 3 |
| `pctel/dsp.o`, `pctel/control.o` | PCTel, Inc. | pctel-0.9.7-9-rht-6 |
| `hsf/hsfengine.o`, `hsf/hsfmc97ich.o`, `hsf/hsfsoar.o`, `hsf/hsfmc97ich.inf`, `hsf/hsf.cty` | Conexant Systems (formerly Rockwell Semiconductor Systems) | Linuxant's hsfmodem-7.80.02.06oem |
| `intel/537core.lib`, `intel/wwh_dflt.c` | Intel Corporation | Intel537EP 2.37.50.1 for Fedora Core 1 (Intel's licence, `intel/license.txt`) |
| `broadcom/BCMSM.sys`, `broadcom/BCMSMwxp.inf` | Broadcom Corporation | Dell's "V.92 Soft Modem (Data Fax) Driver" A05 (BCM V.92 56K Modem 3.4.19) |

The same holds for Smart Link's AT command manual,
[docs/legacy/ST56ATCommands.pdf](../../docs/legacy/ST56ATCommands.pdf).
