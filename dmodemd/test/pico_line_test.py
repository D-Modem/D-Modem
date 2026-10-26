#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
# pico_line_test.py <dmodemd> <line_exchange> <fake libusb>: dmodemd on a
# pico-serial Line stick (--usb), two of them on fake sticks
# (fake_pico_line.cpp, a libusb in front of the real one through
# LD_PRELOAD) whose lines are line_exchange's, 1001 and 1002:
#
# - a stick whose line side does not answer (FAKE_PICO_LINE_NOLINK):
#   dmodemd says so, and goes on;
# - the country's line settings written to the stick's DAA: -c UK's at
#   start-up, AT+GCI=09's (Australia) after it, and the USA's for a
#   country the Si3056 datasheet has none for;
# - Windows 7 and 8 (under Wine, whose WinUSB is theirs: no
#   isochronous transfers): a driver that can't stream said so, with what
#   to do (FAKE_USB_NO_ISO); UsbDk taken where it is installed
#   (FAKE_USB_USBDK), not with DMODEM_USBDK=0; --usb list says so;
# - 1001 dials 1002 (ATX4: after the dial tone, in DTMF); 1002 rings
#   (its ring detector's changes on the interrupt endpoint), shows the
#   caller ID between the first two rings (AT#CID=1: the stick hears the
#   line on hook, its monitor on), answers at the second ring (ATS0=2);
#   both CONNECT, a line of text goes each way; 1001 hangs up (+++ ATH),
#   1002 NO CARRIER;
# - 1002 off hook (ATH1), 1001 dials it again: BUSY;
# - 1001 dials 1002 in pulses (ATDP): both CONNECT;
# - in voice mode (+FCLASS=8) 1001 dials 1002, hears its ringing tone
#   (<DLE>r); 1002 answers (+VLS=1); 1001's +VTS=5 is 1002's <DLE>5;
#   1002 hangs up, and 1001 hears the busy tone (<DLE>b);
# - the exchange gone (the devices unplugged): both dmodemd say so and
#   exit.
#
# A Windows dmodemd (<dmodemd> a .exe, built with the fake in libusb's
# place; <fake libusb> "-") runs under Wine, its DTE TCP, the fake's
# connections to the exchange relayed from TCP (usb_dte.py).
#
# The calls run in real time (the exchange's clock).  RATE=<Hz> the
# codec's rate (9600, or 8000); DMODEMD_DEBUG dmodemd's -d (3).  The logs
# (exchange.log, dmodemd-<line>.log) go to DMODEM_TEST_DIR, without it to
# the temporary directory, kept with KEEP=1 (and printed).
import os, re, shutil, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import usb_dte  # noqa: E402

dmodemd, exchange, fake = sys.argv[1:4]
fake = os.path.abspath(fake) if fake != "-" else None
run = usb_dte.command(dmodemd)
rate = os.environ.get("RATE", "9600")
tmp = tempfile.mkdtemp(prefix="dmodem-pico-line-")
os.chmod(tmp, 0o755)
logs = os.environ.get("DMODEM_TEST_DIR") or tmp
debug = os.environ.get("DMODEMD_DEBUG", "3")


def env_for(n, **more):
    e = dict(os.environ, FAKE_PICO_LINE_SOCKET=usb_dte.socket_for(
        dmodemd, os.path.join(tmp, n + ".sock")))
    if fake:
        e["LD_PRELOAD"] = fake
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
    """a dmodemd on line <n>'s pico-serial Line, its pty"""
    def __init__(self, n, name, tag="", args=(), **env):
        self.log = open(os.path.join(logs, "dmodemd-%s%s.log" % (n, tag)), "wb")
        self.p = subprocess.Popen(run + usb_dte.dte_args(dmodemd) + ["-n", "-d" + debug, "--usb=auto", "-r", rate]
                                  + list(args) + [name],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  env=env_for(n, **env))
        procs.append(self.p)
        where = None
        t = time.time()
        while time.time() - t < 60:
            l = self.p.stdout.readline()
            if not l:
                break
            self.log.write(l); self.log.flush()
            where = usb_dte.Dte.find(l)
            if where:
                break
        if where is None:
            raise RuntimeError("dmodemd %s gave no pty" % n)
        threading.Thread(target=lambda: [self.log.write(l) or self.log.flush()
                                         for l in self.p.stdout], daemon=True).start()
        self.dte = usb_dte.Dte(*where)
        self.buf = b""
        self.n = n

    def expect_log(self, text, t):
        """text in dmodemd's output (its log) within t s"""
        end = time.time() + t
        while time.time() < end:
            self.log.flush()
            if text in open(self.log.name, "rb").read():
                return True
            time.sleep(0.1)
        return False

    def expect(self, pat, t):
        end = time.time() + t
        while time.time() < end:
            m = re.search(pat, self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            if self.dte.wait(0.1):
                try:
                    self.buf += self.dte.read(4096)
                except OSError:
                    return None
        return None

    def cmd(self, c, pat=rb"OK\r\n", t=5):
        self.dte.write(c + b"\r")
        return self.expect(pat, t)

ok = True
def check(what, cond):
    global ok
    print("%s: %s" % (what, "yes" if cond else "NO"))
    ok = ok and bool(cond)

try:
    p = subprocess.run(run + usb_dte.dte_args(dmodemd) + ["-n", "--usb=2:9", "-r", rate, "dmodem40"],
                       env=env_for("1001"), capture_output=True, text=True, timeout=60)
    check("nothing at 2:9", p.returncode != 0 and "no SmartUSB56, pico-serial Line or Agere USB modem at 2:9" in p.stderr)
    nl = Modem("1001", "dmodem40", "-nolink", FAKE_PICO_LINE_NOLINK="1")
    check("a line side that doesn't answer: said", nl.expect_log(b"line side does not answer", 5))
    nl.p.terminate()
    nl.p.wait(5)

    # Windows 7 and 8 (Wine's WinUSB has no isochronous transfers, as
    # theirs): a driver that can't stream (WinUSB) said so, with what to
    # do; UsbDk taken where it is installed (DMODEM_USBDK=0: not)
    if usb_dte.windows(dmodemd):
        p = subprocess.run(run + usb_dte.dte_args(dmodemd) + ["-n", "--usb=auto", "-r", rate, "dmodem40"],
                           env=env_for("1001", FAKE_USB_NO_ISO="1"), capture_output=True,
                           text=True, timeout=60)
        check("WinUSB before 8.1: can't stream, libusbK or UsbDk",
              p.returncode != 0 and "libusbK with Zadig, or install UsbDk" in p.stderr)
        dk = Modem("1001", "dmodem40", "-usbdk", FAKE_USB_USBDK="1")
        check("UsbDk installed: taken", dk.expect_log(b"usb: through UsbDk", 5))
        dk.p.terminate()
        dk.p.wait(5)
        nd = Modem("1001", "dmodem40", "-nousbdk", FAKE_USB_USBDK="1", DMODEM_USBDK="0")
        check("DMODEM_USBDK=0: UsbDk not taken", nd.cmd(b"ATE0") and
              not nd.expect_log(b"through UsbDk", 1))
        nd.p.terminate()
        nd.p.wait(5)
        p = subprocess.run(run + ["--usb", "list"], env=env_for("1001"), capture_output=True,
                           text=True, timeout=60)
        check("--usb list: this WinUSB can't stream", "WinUSB can't stream" in p.stdout)

    # the country's line settings in the stick's DAA (a Si3018 line side:
    # ACT/ACT2, no register 30), as Table 15 of the Si3056 datasheet has
    # them: -c UK's at start-up, Australia's after AT+GCI=09, and the
    # USA's for a country it has no row for (Tunisia)
    uk = Modem("1001", "dmodem40", "-country", ["-c", "UK"], FAKE_PICO_LINE_LOG="1")
    check("-c UK: its line settings",
          uk.expect_log(b"UK's line: registers 16 80, 26 c2, 30 -, 31 08, 59 00", 5))
    check("-c UK: written to the stick (register 16 = 0x80)",
          uk.expect_log(b"request 21 out 4224: 0", 5))
    check("UK: ATE0", uk.cmd(b"ATE0"))
    check("AT+GCI=09 (Australia)", uk.cmd(b"AT+GCI=09"))
    check("Australia's line settings",
          uk.expect_log(b"AUSTRALIA's line: registers 16 c0, 26 50, 30 -, 31 00, 59 50", 5))
    check("AT+GCI=AD (Tunisia)", uk.cmd(b"AT+GCI=AD"))
    check("Tunisia: none in the datasheet, the USA's",
          uk.expect_log(b"no Si3056 line settings for TUNISIA: the USA's", 5) and
          uk.expect_log(b"TUNISIA's line: registers 16 00, 26 c0, 30 -, 31 00, 59 00", 5))
    uk.p.terminate()
    uk.p.wait(5)

    a = Modem("1001", "dmodem41")
    b = Modem("1002", "dmodem42")
    for m in (a, b):
        check("%s ATE0" % m.n, m.cmd(b"ATE0"))
        check("%s ATI3: pico-serial Line" % m.n,
              m.cmd(b"ATI3", rb"pico-serial Line \(Si3056\)\r\n(\r\n)?OK\r\n"))
    check("1002 answers at the 2nd ring", b.cmd(b"ATS0=2"))
    check("1002 caller ID on", b.cmd(b"AT#CID=1"))
    check("1001 waits for the dial tone", a.cmd(b"ATX4"))

    t = time.time()
    a.dte.write(b"ATD1002\r")
    check("1002 RING", b.expect(rb"RING\r\n", 15))
    cid = b.expect(rb"NMBR = (\d+)\r\n", 6)
    check("1002 caller ID 1001", cid and cid.group(1) == b"1001")
    check("1002 RING again", b.expect(rb"RING\r\n", 8))
    ca = a.expect(rb"(CONNECT[^\r]*|NO CARRIER|BUSY|NO DIALTONE)\r\n", 40)
    cb = b.expect(rb"(CONNECT[^\r]*|NO CARRIER)\r\n", 5)
    print("1001: %s, 1002: %s, %.1f s" % (ca and ca.group(1).decode(),
                                         cb and cb.group(1).decode(), time.time() - t))
    check("both CONNECT", ca and cb and ca.group(1).startswith(b"CONNECT")
          and cb.group(1).startswith(b"CONNECT"))
    check("the exchange heard 1002 dialled", "1001 dialled 1002" in events)
    a.dte.write(b"hello from 1001\r\n")
    check("1002 gets 1001's line", b.expect(rb"hello from 1001", 10))
    b.dte.write(b"hello from 1002\r\n")
    check("1001 gets 1002's line", a.expect(rb"hello from 1002", 10))
    time.sleep(1.2)
    a.dte.write(b"+++")
    check("1001 +++ OK", a.expect(rb"OK\r\n", 5))
    check("1001 ATH", a.cmd(b"ATH"))
    check("1001 NO CARRIER", a.expect(rb"NO CARRIER\r\n", 5))
    check("1002 NO CARRIER", b.expect(rb"NO CARRIER\r\n", 30))

    check("1002 off hook", b.cmd(b"ATH1"))
    m = a.cmd(b"ATD1002", rb"(CONNECT[^\r]*|NO CARRIER|BUSY|NO DIALTONE|NO ANSWER)\r\n", 30)
    check("1001 BUSY", m and m.group(1) == b"BUSY")
    check("1002 on hook", b.cmd(b"ATH"))

    # pulse dialling: the hook broken and made (SET_HOOK)
    a.dte.write(b"ATDP1002\r")
    check("1002 RING (pulse)", b.expect(rb"RING\r\n", 30))
    ca = a.expect(rb"(CONNECT[^\r]*|NO CARRIER|BUSY|NO DIALTONE)\r\n", 60)
    cb = b.expect(rb"(CONNECT[^\r]*|NO CARRIER)\r\n", 10)
    check("pulse dialled: the digits", all(
        "1001 pulse digit %s" % d in events for d in "102"))
    check("pulse dialled: both CONNECT", ca and cb and ca.group(1).startswith(b"CONNECT")
          and cb.group(1).startswith(b"CONNECT"))
    time.sleep(1.2)
    a.dte.write(b"+++")
    check("1001 +++ OK (pulse)", a.expect(rb"OK\r\n", 5))
    check("1001 ATH (pulse)", a.cmd(b"ATH"))
    check("1002 NO CARRIER (pulse)", b.expect(rb"NO CARRIER\r\n", 30))

    check("1002 no auto answer", b.cmd(b"ATS0=0"))
    check("1001 voice mode", a.cmd(b"AT+FCLASS=8"))
    check("1002 voice mode", b.cmd(b"AT+FCLASS=8"))
    t = time.time()
    a.dte.write(b"ATD1002\r")
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
              b"pico-serial Line: unplugged" in open(m.log.name, "rb").read())
finally:
    cleanup()
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
