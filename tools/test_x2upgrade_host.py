#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
"""Tests of the x2 upgrade host's caller screen (x2upgrade-host.py).

Run: python3 tools/test_x2upgrade_host.py
"""
import importlib.util
import os
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    'x2upgrade_host', os.path.join(HERE, 'x2upgrade-host.py'))
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


class FakeControl:
    """A control channel: it keeps the requests, and when a request
    comes, it sends the modem's next report on the input."""

    def __init__(self, wfd, answer='OK', report=b''):
        self.wfd = wfd
        self.answer = answer
        self.report = report
        self.lines = []

    def request(self, line, timeout=5):
        self.lines.append(line)
        if self.report:
            os.write(self.wfd, self.report)
        return self.answer


class Session(unittest.TestCase):

    def run_session(self, data, ctl_answer='OK', report=b'', ctl=True,
                    timeout=2, idle=2):
        r, w = os.pipe()
        os.write(w, data)
        out = []
        control = FakeControl(w, ctl_answer, report) if ctl else None
        try:
            res = host.session(host.Input(r), out.append, control, timeout,
                               idle)
        finally:
            os.close(r)
            os.close(w)
        return res, ''.join(out), control

    def test_switch_and_apply(self):
        # Terbo and V.FC on, x2 off: the caller switches x2 on and V.FC off.
        res, out, ctl = self.run_session(
            b'\r\n+X2UP: QUERIED,"0009540034268322",015,015\r\n3\r2\rA\r',
            report=b'\r\n+X2UP: CHANGED,"0009540034268322",015,023\r\n')
        self.assertEqual(ctl.lines, ['X2UP=2,16,8'])
        self.assertEqual(res, ('CHANGED', '0009540034268322', '015', '023'))
        self.assertIn('Features now: HST, Fax, Terbo, V.FC/V.34', out)
        self.assertIn('3. x2         ON   (changed)', out)
        self.assertIn('2. V.FC/V.34  OFF  (changed)', out)
        self.assertIn('Features now: HST, Fax, Terbo, x2', out)
        self.assertIn('enabled and disabled', out)

    def test_quit(self):
        res, out, ctl = self.run_session(
            b'+X2UP: QUERIED,"1",019,019\r\n1q')
        self.assertEqual(ctl.lines, [])
        self.assertEqual(res[0], 'UNCHANGED')
        self.assertIn('you made no change', out)

    def test_apply_with_no_change(self):
        res, out, ctl = self.run_session(
            b'+X2UP: QUERIED,"1",019,019\r\n11a')
        self.assertEqual(ctl.lines, [])
        self.assertEqual(res[0], 'UNCHANGED')

    def test_idle(self):
        res, out, ctl = self.run_session(
            b'+X2UP: QUERIED,"1",019,019\r\n', idle=0.2)
        self.assertEqual(res[0], 'UNCHANGED')
        self.assertIn('No key for too long', out)

    def test_control_error(self):
        res, out, ctl = self.run_session(
            b'+X2UP: QUERIED,"1",003,003\r\n3a', ctl_answer='ERROR')
        self.assertEqual(ctl.lines, ['X2UP=2,16,0'])
        self.assertEqual(res[0], 'FAILED')
        self.assertIn('could not start the change', out)

    def test_refused(self):
        res, out, ctl = self.run_session(
            b'+X2UP: QUERIED,"1",003,003\r\n3a',
            report=b'+X2UP: REFUSED,"1",003,003\r\n')
        self.assertEqual(res[0], 'REFUSED')
        self.assertIn('ATS30=1', out)

    def test_no_control(self):
        # With no control channel: the result of AT+X2UP=2 only.
        res, out, ctl = self.run_session(
            b'+X2UP: ENABLED,"7",003,019\r\n', ctl=False)
        self.assertEqual(res, ('ENABLED', '7', '003', '019'))
        self.assertNotIn('Type 1, 2 or 3', out)
        self.assertIn('Features now: HST, Fax, x2', out)

    def test_no_reply(self):
        res, out, ctl = self.run_session(b'+X2UP: NO REPLY,""\r\n')
        self.assertEqual(res[0], 'NO REPLY')
        self.assertIn('did not reply', out)

    def test_timeout(self):
        res, out, ctl = self.run_session(b'garbage', timeout=0.2)
        self.assertEqual(res[0], 'TIMEOUT')

    def test_partial_line(self):
        # A line that is not complete yet is not a report.
        r, w = os.pipe()
        os.write(w, b'+X2UP: EN')

        def rest():
            time.sleep(0.2)
            os.write(w, b'ABLED,"7",003,019\r\n')
        t = threading.Thread(target=rest)
        t.start()
        m = host.Input(r).report(2)
        t.join()
        os.close(r)
        os.close(w)
        self.assertEqual(m.group(1), 'ENABLED')


class Helpers(unittest.TestCase):

    def test_changes(self):
        self.assertEqual(host.changes(15, 23), (16, 8))
        self.assertEqual(host.changes(19, 19), (0, 0))
        # HST and fax are not switched.
        self.assertEqual(host.changes(3, 0), (0, 0))

    def test_feature_names(self):
        self.assertEqual(host.feature_names(0), 'none')
        self.assertEqual(host.feature_names(17), 'HST, x2')


if __name__ == '__main__':
    unittest.main()
