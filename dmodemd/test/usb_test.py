#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
# usb_test.py <dmodemd> <line_exchange> <fake libusb>: dmodemd on a Smart
# Link SmartUSB56 (--usb), two of them on fake ST7554s (fake_st7554.cpp,
# a libusb in front of the real one through LD_PRELOAD) whose lines are
# line_exchange's, 1001 and 1002:
#
# - with the fake saying its codec is an ST75951/2 (FAKE_ST7554_DAA),
#   dmodemd refuses it and exits;
# - 1001 dials 1002 (ATX4: after the dial tone, in DTMF); 1002 rings
#   (its ring detector's changes on the interrupt endpoint), answers at
#   the second ring (ATS0=2); both CONNECT, a line of text goes each
#   way; 1001 hangs up (+++ ATH), 1002 NO CARRIER;
# - 1002 off hook (ATH1), 1001 dials it again: BUSY;
# - in voice mode (+FCLASS=8) 1001 dials 1002, hears its ringing tone
#   (<DLE>r); 1002 answers (+VLS=1); 1001's +VTS=5 is 1002's <DLE>5;
#   1002 hangs up, and 1001 hears the busy tone (<DLE>b);
# - the exchange gone (the devices unplugged): both dmodemd say so and
#   exit.
#
# The calls run in real time (the exchange's clock).  RATE=<Hz> the
# codec's rate (9600, or 8000); DMODEMD_DEBUG dmodemd's -d (3).  The logs
# (exchange.log, dmodemd-<line>.log) go to DMODEM_TEST_DIR, without it to
# the temporary directory, kept with KEEP=1 (and printed).
import os, re, select, shutil, subprocess, sys, tempfile, threading, time, tty

dmodemd, exchange, fake = sys.argv[1:4]
fake = os.path.abspath(fake)
rate = os.environ.get("RATE", "9600")
tmp = tempfile.mkdtemp(prefix="dmodem-usb-")
os.chmod(tmp, 0o755)
logs = os.environ.get("DMODEM_TEST_DIR") or tmp
debug = os.environ.get("DMODEMD_DEBUG", "3")


def env_for(n, **more):
    e = dict(os.environ, LD_PRELOAD=fake,
             FAKE_ST7554_SOCKET=os.path.join(tmp, n + ".sock"))
    e.update(more)
    return e

procs = []
def cleanup():
    for p in reversed(procs):
        if p.poll() is None:
            p.terminate()
    for p in procs:
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
    if not os.environ.get("KEEP"):
        shutil.rmtree(tmp, ignore_errors=True)
    if logs != tmp or os.environ.get("KEEP"):
        print("logs in", logs)

ex_log = open(os.path.join(logs, "exchange.log"), "w")
events = []
ex = subprocess.Popen([exchange, rate, tmp, "1001", "1002"], stdin=subprocess.PIPE,
                      stdout=subprocess.PIPE, text=True)
procs.append(ex)
def exchange_out():
    for l in ex.stdout:
        ex_log.write("%.2f %s" % (time.time(), l)); ex_log.flush()
        events.append(l.strip())
threading.Thread(target=exchange_out, daemon=True).start()
t0 = time.time()
while "ready" not in events and time.time() - t0 < 5:
    time.sleep(0.05)

class Modem:
    """a dmodemd on line <n>'s SmartUSB56, its pty"""
    def __init__(self, n, name):
        self.log = open(os.path.join(logs, "dmodemd-%s.log" % n), "wb")
        self.p = subprocess.Popen([dmodemd, "-n", "-d" + debug, "--usb=auto", "-r", rate, name],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  env=env_for(n))
        procs.append(self.p)
        pty = None
        t = time.time()
        while time.time() - t < 10:
            l = self.p.stdout.readline()
            if not l:
                break
            self.log.write(l); self.log.flush()
            m = re.search(rb"TTY is `(\S+)'", l)
            if m:
                pty = m.group(1).decode(); break
        if pty is None:
            raise RuntimeError("dmodemd %s gave no pty" % n)
        threading.Thread(target=lambda: [self.log.write(l) or self.log.flush()
                                         for l in self.p.stdout], daemon=True).start()
        self.fd = os.open(pty, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        self.buf = b""
        self.n = n

    def expect(self, pat, t):
        end = time.time() + t
        while time.time() < end:
            m = re.search(pat, self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    self.buf += os.read(self.fd, 4096)
                except OSError:
                    return None
        return None

    def cmd(self, c, pat=rb"OK\r\n", t=5):
        os.write(self.fd, c + b"\r")
        return self.expect(pat, t)

ok = True
def check(what, cond):
    global ok
    print("%s: %s" % (what, "yes" if cond else "NO"))
    ok = ok and bool(cond)

try:
    p = subprocess.run([dmodemd, "-n", "--usb=1:7", "-r", rate, "dmodem40"],
                       env=env_for("1001", FAKE_ST7554_DAA="1"),
                       capture_output=True, text=True, timeout=20)
    check("an ST75951/2 refused", p.returncode != 0 and "ST75951/2" in p.stderr)
    p = subprocess.run([dmodemd, "-n", "--usb=2:9", "-r", rate, "dmodem40"],
                       env=env_for("1001"), capture_output=True, text=True, timeout=20)
    check("no SmartUSB56 at 2:9", p.returncode != 0 and "no SmartUSB56 at 2:9" in p.stderr)

    a = Modem("1001", "dmodem41")
    b = Modem("1002", "dmodem42")
    for m in (a, b):
        check("%s ATE0" % m.n, m.cmd(b"ATE0"))
        check("%s ATI3: ST7554" % m.n, m.cmd(b"ATI3", rb"ST7554\r\n(\r\n)?OK\r\n"))
    check("1002 answers at the 2nd ring", b.cmd(b"ATS0=2"))
    check("1001 waits for the dial tone", a.cmd(b"ATX4"))

    t = time.time()
    os.write(a.fd, b"ATD1002\r")
    check("1002 RING", b.expect(rb"RING\r\n", 15))
    check("1002 RING again", b.expect(rb"RING\r\n", 8))
    ca = a.expect(rb"(CONNECT[^\r]*|NO CARRIER|BUSY|NO DIALTONE)\r\n", 40)
    cb = b.expect(rb"(CONNECT[^\r]*|NO CARRIER)\r\n", 5)
    print("1001: %s, 1002: %s, %.1f s" % (ca and ca.group(1).decode(),
                                         cb and cb.group(1).decode(), time.time() - t))
    check("both CONNECT", ca and cb and ca.group(1).startswith(b"CONNECT")
          and cb.group(1).startswith(b"CONNECT"))
    check("the exchange heard 1002 dialled", "1001 dialled 1002" in events)
    os.write(a.fd, b"hello from 1001\r\n")
    check("1002 gets 1001's line", b.expect(rb"hello from 1001", 10))
    os.write(b.fd, b"hello from 1002\r\n")
    check("1001 gets 1002's line", a.expect(rb"hello from 1002", 10))
    time.sleep(1.2)
    os.write(a.fd, b"+++")
    check("1001 +++ OK", a.expect(rb"OK\r\n", 5))
    check("1001 ATH", a.cmd(b"ATH"))
    check("1001 NO CARRIER", a.expect(rb"NO CARRIER\r\n", 5))
    check("1002 NO CARRIER", b.expect(rb"NO CARRIER\r\n", 30))

    check("1002 off hook", b.cmd(b"ATH1"))
    m = a.cmd(b"ATD1002", rb"(CONNECT[^\r]*|NO CARRIER|BUSY|NO DIALTONE|NO ANSWER)\r\n", 30)
    check("1001 BUSY", m and m.group(1) == b"BUSY")
    check("1002 on hook", b.cmd(b"ATH"))

    check("1002 no auto answer", b.cmd(b"ATS0=0"))
    check("1001 voice mode", a.cmd(b"AT+FCLASS=8"))
    check("1002 voice mode", b.cmd(b"AT+FCLASS=8"))
    t = time.time()
    os.write(a.fd, b"ATD1002\r")
    check("1002 RING (voice)", b.expect(rb"RING\r\n", 20))
    check("1002 answers: +VLS=1", b.cmd(b"AT+VLS=1", t=5))
    m = a.expect(rb"(\x10r)?(OK|BUSY|NO ANSWER|ERROR)\r\n", 30)
    print("1001 voice dial: %s in %.1f s" % (m and m.group(2).decode(), time.time() - t))
    check("1001 heard the ringing tone and OK", m and m.group(1) and m.group(2) == b"OK")
    check("1001 +VTS=5", a.cmd(b"AT+VTS=5"))
    check("1002 <DLE>5", b.expect(rb"\x105", 5))
    check("1002 hangs up", b.cmd(b"AT+VLS=0"))
    check("1001 <DLE>b (the busy tone)", a.expect(rb"\x10b", 10))
    check("1001 ATH", a.cmd(b"ATH"))

    ex.terminate()
    ex.wait(5)
    for m in (a, b):
        try:
            r = m.p.wait(10)
        except subprocess.TimeoutExpired:
            r = None
        m.log.flush()
        check("%s exits once unplugged" % m.n, r is not None and r != 0 and
              b"SmartUSB56: unplugged" in open(m.log.name, "rb").read())
finally:
    cleanup()
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
