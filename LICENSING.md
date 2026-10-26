# Licensing

D-Modem is copyright Neo-Desktop, and is available under either of two
licences:

- the **GNU Affero General Public License, version 3 only**
  (AGPL-3.0-only), in [COPYING](COPYING); or
- a **commercial licence** from Neo-Desktop, for uses that cannot meet
  the AGPL's terms. Ask Neo-Desktop (<https://github.com/Neo-Desktop>)
  for its terms.

Each source file names its licence on an `SPDX-License-Identifier` line.

## The Asterisk module

`asterisk/app_dmodem.c` is under the GNU GPL version 2 only
(GPL-2.0-only), the licence of Asterisk, which it is built against.

`dmodem-ari` (`ari/`) serves Asterisk calls without a module: it is an
application of the Asterisk REST Interface, which Asterisk's licence says
need not be under the GPL. It is under the AGPL-3.0, or the commercial
licence, like the rest ([docs/ari.md](docs/ari.md)).

## Smart Link's ST7554 driver

`dmodemd/usb_line.cpp` (the SmartUSB56 line) has the ST7554's registers,
their values and its start-up from Smart Link's Linux driver for it
(`drivers/st7554.c` of slmodem 2.9.11, copyright 2002 Smart Link Ltd.),
under the BSD-3-Clause licence. The file keeps that licence's notice
(`SPDX-License-Identifier: AGPL-3.0-only AND BSD-3-Clause`), and it goes
with a dmodemd that is shipped. It combines with D-Modem under the AGPL
and under the commercial licence alike.

## Agere's USB modem patches

The Agere USB 2.0 soft modem (`dmodemd/usb_agere.cpp`) needs a "bulk
patch", ARM code of Agere's, downloaded into it at every start. Agere's
patches are in `firmware/agere/` (the `.bin` files), unaltered.
**Their binary contents are Agere Systems' software, subject to Agere's
licence (`firmware/agere/LICENSE`, Agere's Soft Modem End User Software
Licence Agreement), not to this repository's terms** (the AGPL, the
commercial licence, or any other licence D-Modem is offered under). They
are redistributed under that licence's section 2.3, with its notices
(`firmware/agere/NOTICE.md`). They are data files, read at run time and
sent to the device; nothing of them is compiled or linked into any
D-Modem program. The device's commands, registers and start-up were read
from Agere's drivers; the code that drives it is D-Modem's own, under
the AGPL-3.0 or the commercial licence.

## Not covered: the interop test binaries

The closed modem drivers and objects in `dsp/test/` (`smartlink/`,
`agere/`, `sm56/`, `pctel/`, `hsf/`, `broadcom/`) and Smart Link's manual
`docs/legacy/ST56ATCommands.pdf` belong to their owners and are licensed
by them, under their own terms. **They are not subject to the terms of the
rest of this repository** (the AGPL, the commercial licence, or any other
licence or permission given here), and no rights in them are granted
here. They are only used by the interop tests; see
[dsp/test/NOTICE.md](dsp/test/NOTICE.md).

## Fetched at build time

These are not in the repository; the build fetches them or takes them from
the system:

- **libre** (baresip's re), dmodemd's SIP stack: BSD-3-Clause (its
  copyright notice and licence go with a dmodemd that is shipped). It
  combines with D-Modem under the AGPL and under the commercial licence
  alike; libdmodem and the Asterisk and FreeSWITCH modules do not use it.
  The OpenSSL it links (its digests) is under the Apache License 2.0.
- **Mbed TLS**, libre's digests in OpenSSL's place when built with
  `-DDMODEM_CRYPTO=mbedtls` (the default for Android): Apache-2.0 or
  GPL-2.0-or-later, at the shipper's choice.
- **libusb** (dmodemd's `--usb`): LGPL-2.1-or-later. Linux's dmodemd
  links the system's libusb-1.0 dynamically. Windows' dmodemd.exe links
  the fetched release statically (`cmake/libusb.cmake`): a combination
  the AGPL allows. Shipped under the commercial licence, it must meet the
  LGPL's terms for libusb, its source and the means to relink dmodemd.exe
  with another (dmodemd's objects, for instance); `-DDMODEM_USB=OFF`
  builds it without.
- **GoogleTest** (the tests only): BSD-3-Clause.
- The **Windows Driver Kit**'s UMDF headers and stub library
  (`WdfDriverStubUm.lib`), which the virtual modem driver
  (`winmodem/driver.cpp`) is built with on Windows: Microsoft's, under
  the WDK's licence; the stub is linked into `dmodemvm.dll`. Where there
  is no WDK the driver is only compiled, against the UMDF headers of
  Microsoft's Windows-Driver-Frameworks (MIT). The driver's code and its
  INF are D-Modem's own (Microsoft's VirtualSerial2 sample, MS-PL, was
  read for how such a driver is installed, not copied).
- **spandsp** (the tests only): LGPL-2.1.
- The ITU-T Recommendations that `docs/specs/fetch.sh` downloads are the
  ITU's.

## History

D-Modem began as Aon's D-Modem (2021, GPL-2.0-or-later), on Smart Link's
slmodemd (Smart Link Ltd., 2001 to 2004, under a BSD licence). None of
their code is left (dmodemd's SmartUSB56 line takes the ST7554's registers
from Smart Link's BSD-licensed driver for it, above). Aon's last, the SIP user agent `dmodemd/sip.cpp`,
and Smart Link's last (the modem core in `core/`, the HDLC framing, LAPM
and V.42bis in `link/`, the data pumps' interface and two simple pumps in
`dsp/`, the pumps' test loopback, and dmodemd's main program, options and
pty) were written anew in 2026, each from a specification of what it
does, without their code.

## Contributions

Contributions can be accepted only under terms that let Neo-Desktop offer
them under both licences above. Ask before sending one.
