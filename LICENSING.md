# Licensing

D-Modem is copyright Neo-Desktop, and is available under either of two
licences:

- the **GNU Affero General Public License, version 3 only**
  (AGPL-3.0-only), in [COPYING](COPYING); or
- a **commercial licence** from Neo-Desktop, for uses that cannot meet
  the AGPL's terms. Ask Neo-Desktop (<https://github.com/Neo-Desktop>)
  for its terms.

Each source file names its licence on an `SPDX-License-Identifier` line.

## Smart Link's code

Some files carry or adapt code of Smart Link's slmodemd (Smart Link Ltd.,
2001 to 2004), which Smart Link published under a BSD licence, kept in
[LICENSES/BSD-3-Clause.txt](LICENSES/BSD-3-Clause.txt). Their SPDX lines
say `AGPL-3.0-only AND BSD-3-Clause`. The BSD licence allows both of the
licences above, as long as Smart Link's notice is kept.

## The Asterisk module

`asterisk/app_dmodem.c` is under the GNU GPL version 2 only
(GPL-2.0-only), the licence of Asterisk, which it is built against.

`dmodem-ari` (`ari/`) serves Asterisk calls without a module: it is an
application of the Asterisk REST Interface, which Asterisk's licence says
need not be under the GPL. It is under the AGPL-3.0, or the commercial
licence, like the rest ([docs/ari.md](docs/ari.md)).

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

- **pjproject**, dmodemd's SIP stack: GPL-2.0-or-later, or Teluu's
  commercial licence. Under the AGPL it combines with D-Modem as it is.
  A commercial licensee who ships dmodemd with its SIP user agent needs
  Teluu's commercial licence for pjproject as well; libdmodem and the
  Asterisk and FreeSWITCH modules do not use pjproject.
- **GoogleTest** (the tests only): BSD-3-Clause.
- **spandsp** (the tests only): LGPL-2.1.
- The ITU-T Recommendations that `docs/specs/fetch.sh` downloads are the
  ITU's.

## History

D-Modem began as Aon's D-Modem (2021, GPL-2.0-or-later). None of Aon's
code is left: the last of it, the SIP user agent `dmodemd/sip.cpp`, was
written anew in 2026 from a specification of what it does, without Aon's
code.

## Contributions

Contributions can be accepted only under terms that let Neo-Desktop offer
them under both licences above. Ask before sending one.
