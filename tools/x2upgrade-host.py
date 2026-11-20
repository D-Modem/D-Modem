#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
"""The caller's screen of the x2 upgrade host (docs/x2-upgrade.md).

Run this program on the modem's DTE, for example as an exec profile of
app_dmodem or dmodem-ari. Its standard input and output are the call.

With a control channel (DMODEM_CONTROL, from the exec profile) and the
modem set to AT+X2UP=1, the program shows the features of the caller's
Courier. The caller switches Terbo, V.FC and x2 on or off, and applies the
change. The program then tells the modem to do the change, and shows the
result.

With no control channel, the modem must be set to AT+X2UP=2 (and the
features in its profile). The program then only shows the result.

When the program stops, the call ends.

Options:
  --timeout S   seconds to wait for +X2UP: (default 60)
  --idle S      seconds to wait for a key in the menu (default 300)
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
This host changes the features of an upgradable USRobotics product that\r
has the x2 code. It reads the serial number and the feature record of\r
your modem through the error-corrected link. Then it enables or disables\r
features (Terbo, V.FC, x2) in that record.\r
\r
Please wait. Do not type.\r
\r
"""

AFTER = """\
To activate the change, do these steps:\r
  1. Hang up.\r
  2. Type AT&F1S0=0&W and push Enter.\r
  3. Set the power switch of the modem to OFF, then to ON.\r
  4. Type ATI7. Make sure that the Options line shows the change.\r
"""

MESSAGES = {
    'ENABLED': "Result: the host enabled the features in your modem.\r\n\r\n"
               + AFTER,
    'DISABLED': "Result: the host disabled the features in your modem."
                "\r\n\r\n" + AFTER,
    'CHANGED': "Result: the host enabled and disabled features in your "
               "modem.\r\n\r\n" + AFTER,
    'ALREADY': "Result: your modem already had the features that this host "
               "sets.\r\nNo change was necessary.\r\n",
    'REFUSED': "Result: your modem did not keep the change.\r\n\r\n"
               "Your modem must permit feature upgrades. Type ATS30=1 and "
               "push Enter.\r\nThen call this host again.\r\n",
    'QUERIED': "Result: the host read your modem. It made no change.\r\n",
    'NO REPLY': "Result: your modem did not reply.\r\n\r\n"
                "This host operates only with a USRobotics Courier that "
                "has x2 code,\r\nand only with error control (V.42 LAPM). "
                "Type AT&F1 and push Enter.\r\nThen call this host again.\r\n",
    'UNCHANGED': "Result: you made no change.\r\n",
    'FAILED': "Result: the host could not start the change. Please call "
              "again later.\r\n",
}

# record 2's feature bits, as ATC8 has them
FEATURES = ((1, 'HST'), (2, 'Fax'), (4, 'Terbo'), (8, 'V.FC/V.34'), (16, 'x2'))

# the features that the caller can switch: key, bit, name
SWITCHES = (('1', 4, 'Terbo'), ('2', 8, 'V.FC/V.34'), ('3', 16, 'x2'))
SWITCHED = 4 | 8 | 16


def feature_names(value):
    """The names of the features in a record 2 value, for example 'HST, x2'."""
    names = [name for bit, name in FEATURES if int(value) & bit]
    return ', '.join(names) if names else 'none'


def changes(have, want):
    """The bits to enable and to disable, to go from have to want."""
    return (want & ~have & SWITCHED, have & ~want & SWITCHED)


def menu(have, want):
    """The menu text for the features have, with want chosen."""
    text = '\r\nFeatures of your modem:\r\n'
    for key, bit, name in SWITCHES:
        state = 'ON ' if want & bit else 'OFF'
        note = '  (changed)' if (want ^ have) & bit else ''
        text += '  %s. %-10s %s%s\r\n' % (key, name, state, note)
    text += ('\r\nType 1, 2 or 3 to switch a feature on or off.\r\n'
             'Type A to apply the changes. Type Q to stop with no change.\r\n'
             '> ')
    return text


REPORT = re.compile(r'\+X2UP: ([A-Z ]+?)(?:,"([^"]*)")?(?:,(\d+),(\d+))?\s*$')


class Input:
    """The octets from the caller and the modem, on one descriptor."""

    def __init__(self, fd):
        self.fd = fd
        self.buf = b''
        self.eof = False

    def _fill(self, end):
        """More octets into buf; False at the deadline or the end."""
        if self.eof:
            return False
        left = end - time.monotonic()
        if left <= 0:
            return False
        ready, _, _ = select.select([self.fd], [], [], left)
        if not ready:
            return False
        data = os.read(self.fd, 256)
        if not data:
            self.eof = True
            return False
        self.buf += data
        return True

    def report(self, timeout):
        """The next +X2UP: line from the modem, or None at the timeout."""
        end = time.monotonic() + timeout
        while True:
            # Only complete lines; the rest waits for more octets, or for
            # the end of the input.
            lines = re.split(rb'[\r\n]', self.buf)
            rest = lines.pop()
            if self.eof:
                lines.append(rest)
                rest = b''
            for i, line in enumerate(lines):
                m = REPORT.match(line.decode('ascii', 'replace').strip())
                if m:
                    self.buf = b'\n'.join(lines[i + 1:] + [rest])
                    return m
            self.buf = rest[-200:]
            if not self._fill(end):
                if self.eof and self.buf:
                    continue
                return None

    def key(self, timeout):
        """The next key that the caller typed (not a space or a line end),
        or None at the timeout."""
        end = time.monotonic() + timeout
        while True:
            while self.buf:
                c = self.buf[:1]
                self.buf = self.buf[1:]
                if c not in b' \r\n\0':
                    return c.decode('ascii', 'replace')
            if not self._fill(end):
                return None


class Control:
    """The control channel of the exec profile (DMODEM_CONTROL)."""

    def __init__(self, fd):
        self.fd = fd

    def request(self, line, timeout=5):
        """Send a command line; the answer (OK or ERROR), or None."""
        try:
            os.write(self.fd, (line + '\n').encode('ascii'))
        except OSError:
            return None
        end = time.monotonic() + timeout
        buf = b''
        while b'\n' not in buf:
            left = end - time.monotonic()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                return None
            data = os.read(self.fd, 64)
            if not data:
                return None
            buf += data
        return buf.split(b'\n')[0].strip().decode('ascii', 'replace')


def control_from_env():
    """The control channel that DMODEM_CONTROL names, or None."""
    try:
        fd = int(os.environ.get('DMODEM_CONTROL', ''))
        os.fstat(fd)
    except (ValueError, OSError):
        return None
    return Control(fd)


def show_result(write, m):
    """The result screen for the +X2UP: match m (None: no reply)."""
    if not m:
        write(MESSAGES['NO REPLY'])
        return
    result, serial, before, after = m.group(1), m.group(2), m.group(3), m.group(4)
    if serial:
        write('Serial number: %s\r\n' % serial)
    if before:
        write('Feature record: %s before, %s after\r\n' % (before, after))
        write('Features now: %s\r\n' % feature_names(after))
    write('\r\n' + MESSAGES.get(result, 'Result: %s\r\n' % result))


def session(inp, write, ctl, timeout, idle):
    """One call. The result for the log: (result, serial, before, after)."""
    write(BANNER)
    m = inp.report(timeout)
    if not m or m.group(1) != 'QUERIED' or not m.group(3) or not ctl:
        show_result(write, m)
        if not m:
            return ('TIMEOUT', '', '', '')
        return (m.group(1), m.group(2) or '', m.group(3) or '',
                m.group(4) or '')
    serial, have = m.group(2) or '', int(m.group(4))
    write('Serial number: %s\r\n' % serial)
    write('Features now: %s\r\n' % feature_names(have))
    want = have
    while True:
        write(menu(have, want))
        k = inp.key(idle)
        if k is None:
            write('\r\n\r\nNo key for too long.\r\n')
            k = 'Q'
        else:
            write(k + '\r\n')
        k = k.upper()
        bits = [bit for key, bit, _ in SWITCHES if key == k]
        if bits:
            want ^= bits[0]
            continue
        if k == 'Q' or (k == 'A' and want == have):
            write('\r\n' + MESSAGES['UNCHANGED'])
            return ('UNCHANGED', serial, '%03d' % have, '%03d' % have)
        if k == 'A':
            break
    enable, disable = changes(have, want)
    write('\r\nPlease wait. Do not type.\r\n\r\n')
    if ctl.request('X2UP=2,%d,%d' % (enable, disable)) != 'OK':
        write(MESSAGES['FAILED'])
        return ('FAILED', serial, '%03d' % have, '')
    m = inp.report(timeout)
    show_result(write, m)
    if not m:
        return ('TIMEOUT', serial, '%03d' % have, '')
    return (m.group(1), m.group(2) or serial, m.group(3) or '',
            m.group(4) or '')


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--timeout', type=float, default=60)
    p.add_argument('--idle', type=float, default=300)
    p.add_argument('--log')
    args = p.parse_args()

    def write(text):
        os.write(sys.stdout.fileno(), text.encode('ascii', 'replace'))

    result, serial, before, after = session(
        Input(sys.stdin.fileno()), write, control_from_env(), args.timeout,
        args.idle)
    write('\r\nGoodbye.\r\n')
    if args.log:
        with open(args.log, 'a') as f:
            f.write('%s %s caller=%s serial=%s features=%s->%s\n' % (
                time.strftime('%Y-%m-%dT%H:%M:%S'), result,
                os.environ.get('DMODEM_CALLERID', ''), serial, before, after))
    time.sleep(3)


if __name__ == '__main__':
    main()
