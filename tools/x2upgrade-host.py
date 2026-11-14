#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
"""The caller's screen of the x2 upgrade host (docs/x2-upgrade.md).

Run this program on the modem's DTE, for example as an exec profile of
app_dmodem or dmodem-ari, with the modem set to AT+X2UP=2. Its standard
input and output are the call. The program shows a banner, waits for the
modem's +X2UP: line, tells the caller the result and what to do next, and
then stops. When the program stops, the call ends.

Options:
  --timeout S   seconds to wait for +X2UP: (default 60)
  --log FILE    add one line for each call to FILE
"""
import argparse
import os
import re
import select
import sys
import time

BANNER = """\
\r
D-Modem x2 upgrade host\r
=======================\r
\r
This host enables x2 in a USRobotics Courier V.Everything that has the\r
x2 code. It reads the serial number and the feature record of your modem\r
through the error-corrected link. Then it sets the x2 feature bit.\r
\r
Please wait. Do not type.\r
\r
"""

AFTER = """\
To use x2, do these steps:\r
  1. Hang up.\r
  2. Type AT&F1S0=0&W and push Enter.\r
  3. Set the power switch of the modem to OFF, then to ON.\r
  4. Type ATI7. Make sure that "x2" is in the Options line.\r
"""

MESSAGES = {
    'ENABLED': "Result: x2 is now enabled in your modem.\r\n\r\n" + AFTER,
    'ALREADY': "Result: x2 was already enabled in your modem. No change was "
               "necessary.\r\n\r\n" + AFTER,
    'REFUSED': "Result: your modem did not keep the change.\r\n\r\n"
               "Your modem must permit feature upgrades. Type ATS30=1 and "
               "push Enter.\r\nThen call this host again.\r\n",
    'QUERIED': "Result: the host read your modem. It made no change.\r\n",
    'NO REPLY': "Result: your modem did not reply.\r\n\r\n"
                "This host operates only with a USRobotics Courier that "
                "has x2 code,\r\nand only with error control (V.42 LAPM). "
                "Type AT&F1 and push Enter.\r\nThen call this host again.\r\n",
}

REPORT = re.compile(r'\+X2UP: ([A-Z ]+?)(?:,"([^"]*)")?(?:,(\d+),(\d+))?\s*$')


def write(text):
    os.write(sys.stdout.fileno(), text.encode('ascii', 'replace'))


def wait_report(timeout):
    """The +X2UP: line from the modem, or None at the timeout."""
    buf = b''
    end = time.monotonic() + timeout
    fd = sys.stdin.fileno()
    while time.monotonic() < end:
        ready, _, _ = select.select([fd], [], [], max(0, end - time.monotonic()))
        if not ready:
            break
        data = os.read(fd, 256)
        buf += data
        # Only complete lines; the rest waits for more octets, or for
        # the end of the input.
        lines = re.split(rb'[\r\n]', buf)
        buf = lines.pop() if data else b''
        for line in lines + [buf] * (not data):
            m = REPORT.match(line.decode('ascii', 'replace').strip())
            if m:
                return m
        if not data:
            break
        buf = buf[-200:]
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--timeout', type=float, default=60)
    p.add_argument('--log')
    args = p.parse_args()
    write(BANNER)
    m = wait_report(args.timeout)
    if not m:
        write(MESSAGES['NO REPLY'])
        result, serial, before, after = 'TIMEOUT', '', '', ''
    else:
        result, serial, before, after = (m.group(1), m.group(2) or '',
                                         m.group(3) or '', m.group(4) or '')
        if serial:
            write('Serial number: %s\r\n' % serial)
        if before:
            write('Feature record: %s before, %s after\r\n' % (before, after))
        write('\r\n' + MESSAGES.get(result, 'Result: %s\r\n' % result))
    write('\r\nGoodbye.\r\n')
    if args.log:
        with open(args.log, 'a') as f:
            f.write('%s %s caller=%s serial=%s features=%s->%s\n' % (
                time.strftime('%Y-%m-%dT%H:%M:%S'), result,
                os.environ.get('DMODEM_CALLERID', ''), serial, before, after))
    time.sleep(3)


if __name__ == '__main__':
    main()
