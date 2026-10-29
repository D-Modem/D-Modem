#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# usb_test.py <dmodemd> <line_exchange> <fake libusb>: dmodemd on a Smart
# Link SmartUSB56 (--usb), two of them on fake ST7554s (fake_st7554.cpp,
# a libusb in front of the real one through LD_PRELOAD) whose lines are
# line_exchange's, 1001 and 1002:
#
# - the international line controls (--usb-international, Windows'
#   Options.International), 1001 alone: in Australia off hook DP and RFC
#   (0x0588), RFC off 350 ms later (0x0488), a digit pulse dialled with
#   RFC set through it, and DP and RFC cleared on hook; in France DC off
#   hook (0x1488); on an ST75951 DC the other way (the USA's off hook
#   0x1488);
# - the voice codec as a handset (--usb-voice-codec=ulaw, the fake's
#   FAKE_ST7554_VOICE: its microphone a 1000 Hz tone, its earpiece's
#   octets to a file), 1001 alone: alternate setting 3, the codec on SSI2
#   powered up (0x21) and its period set; +VLS=? lists 14 ("H") and 15
#   ("HT"); +VLS=14 stays on hook, +VRX gets the microphone's tone, +VTX's
#   500 Hz tone comes out of the earpiece; +VLS=15 goes off hook, +VRX
#   gets the microphone's tone with the line (its dial tone), which the
#   earpiece gets too; +VLS=0 on hook;
#   with --usb-rhom FIFO_CONTROL2 is 0xa8a8 (RHOM for both codecs) while
#   the modem runs and 0x2828 when it stops; a chip of revision 0x0100
#   gets no RHOM;
# - 1002's fake says its codec is an ST75951/2 silicon DAA
#   (FAKE_ST7554_DAA): dmodemd starts it up with its control words
#   (register 3 = 0x050), 1001's an STLC7550;
# - 1001 dials 1002 (ATX4: after the dial tone, in DTMF); 1002 rings
#   (its ring detector's changes on the interrupt endpoint), shows the
#   caller ID between the first two rings (AT#CID=1: GPIO 5, CID1, set
#   while it listens on hook, the fake's codec hearing the line only
#   then), answers at the second ring (ATS0=2); both CONNECT, a line of
#   text goes each way; 1001 hangs up (+++ ATH), 1002 NO CARRIER;
#   1001's buzzer (--usb-buzzer, GPIO 11) on as it dials (ATM1) and off
#   at the CONNECT, 1002's (none) never; 1001's LEDs (--usb-leds) lit in
#   the call, the carrier's (GPIO 6)
#   and the data one (GPIO 13), off on hook; 1002 (no --usb-leds) never
#   sets them;
# - 1002 off hook (ATH1), 1001 dials it again: BUSY;
# - 1001 dials 1002 in pulses (ATDP): both CONNECT;
# - 1002's handset detector (--usb-handset, the fake's FAKE_ST7554_HANDSET
#   file): +VLS=? lists V.253's event 8, and the phone on its jack off
#   and on hook is <DLE>H and <DLE>h in voice mode;
# - in voice mode (+FCLASS=8) 1001 dials 1002, hears its ringing tone
#   (<DLE>r); 1002 answers (+VLS=1); 1001's +VTS=5 is 1002's <DLE>5;
#   1002 hangs up, and 1001 hears the busy tone (<DLE>b);
# - wake on ring: 1002's remote wakeup armed at the start (--usb-wake;
#   1001's not); 1002 suspended (the fake's FAKE_ST7554_SUSPEND file: its
#   transfers fail), 1001 dials it, its ring wakes it (the fake's "remote
#   wakeup"); resumed (the file gone, the device reset) dmodemd sets it up
#   again, and 1002 answers: both CONNECT;
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
tmp = tempfile.mkdtemp(prefix="dmodem-usb-")
os.chmod(tmp, 0o755)
logs = os.environ.get("DMODEM_TEST_DIR") or tmp
debug = os.environ.get("DMODEMD_DEBUG", "3")


def env_for(n, **more):
    e = dict(os.environ, FAKE_ST7554_SOCKET=usb_dte.socket_for(
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
    """a dmodemd on line <n>'s SmartUSB56 (its options args, the fake's
    environment more), its pty; its log dmodemd-<n><tag>.log"""
    def __init__(self, n, name, args=(), tag="", **more):
        self.log = open(os.path.join(logs, "dmodemd-%s%s.log" % (n, tag)), "wb")
        self.p = subprocess.Popen(run + usb_dte.dte_args(dmodemd) + usb_dte.realtime() + ["-d" + debug, "--usb=auto", "-r", rate] + list(args) + [name],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  env=env_for(n, **more))
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
    p = subprocess.run(run + usb_dte.dte_args(dmodemd) + usb_dte.realtime() + ["--usb=2:9", "-r", rate, "dmodem40"],
                       env=env_for("1001"), capture_output=True, text=True, timeout=60)
    check("no SmartUSB56 at 2:9", p.returncode != 0 and "no SmartUSB56, pico-serial Line or WB, or Agere USB modem at 2:9" in p.stderr)

    def log_of(m):
        m.log.flush()
        return open(m.log.name, "rb").read()
    gpio = lambda m: [int(v, 16) for v in
                      re.findall(rb"fake st7554: 0a = ([0-9a-f]{4})", log_of(m))]
    def intl(country, **more):
        """1001 alone with --usb-international in country, the fake's
        environment more: its GPIO writes off hook (ATH1, 1 s) and on hook
        again"""
        m = Modem("1001", "dmodem40", ["--usb-international", "-c", country],
                  "-intl-" + country.lower() + ("-75951" if more else ""),
                  FAKE_ST7554_LOG="1", **more)
        m.cmd(b"ATE0")
        n = len(gpio(m))
        m.cmd(b"ATH1")
        time.sleep(1)
        m.cmd(b"ATH")
        time.sleep(0.3)
        return m, gpio(m)[n:]
    def tone(x, f):
        """the power of x (16-bit samples at 8000/s) at f Hz (Goertzel),
        to its whole power"""
        import math
        k = 2 * math.cos(2 * math.pi * f / 8000)
        a = b = 0.0
        for v in x:
            a, b = v + k * a - b, a
        e = sum(v * v for v in x) or 1
        return (a * a + b * b - k * a * b) / (len(x) / 2) / e
    def ulaw(c):
        """a mu-law octet's 16-bit sample"""
        c = ~c & 0xff
        t = (((c & 0x0f) << 3) + 0x84) << ((c & 0x70) >> 4)
        return (0x84 - t) if c & 0x80 else (t - 0x84)
    def unstuff(b):
        """+VRX's octets, <DLE><DLE> one, other <DLE> pairs out; the
        samples ((b - 128) * 256)"""
        out, i = [], 0
        while i < len(b):
            if b[i] == 0x10:
                if i + 1 < len(b) and b[i + 1] == 0x10:
                    out.append(0x10)
                i += 2
                continue
            out.append(b[i])
            i += 1
        return [(v - 128) * 256 for v in out]
    ear = os.path.join(tmp, "1001.ear")
    m = Modem("1001", "dmodem39", ["--usb-voice-codec=ulaw", "--usb-rhom"], "-voice",
              FAKE_ST7554_LOG="1", FAKE_ST7554_VOICE=usb_dte.file_for(dmodemd, ear))
    want = (b"fake st7554: alternate setting 3", b"fake st7554: 02 = 0021",
            b"fake st7554: 06 = 0027")
    end = time.time() + 10
    while time.time() < end and not all(w in log_of(m) for w in want):
        time.sleep(0.1)
    check("voice codec: alternate setting 3, SSI2 powered up, its period",
          all(w in log_of(m) for w in want))
    m.cmd(b"ATE0")
    check("voice codec: voice mode", m.cmd(b"AT+FCLASS=8"))
    check("voice codec: +VLS=? lists 14 and 15",
          m.cmd(b"AT+VLS=?", rb'14,"H",08000000,08000000,08000000\r\n'
                rb'15,"HT",0B043060,0B043060,0B043060\r\nOK\r\n'))
    fifo2 = lambda m: [int(v, 16) for v in
                       re.findall(rb"fake st7554: 1a = ([0-9a-f]{4})", log_of(m))]
    check("RHOM off while the modem stops", fifo2(m)[-1:] == [0x2828])
    n = len(gpio(m))
    check("voice codec: +VLS=14", m.cmd(b"AT+VLS=14"))
    check("RHOM on for both codecs while the modem runs (0xa8a8)",
          fifo2(m)[-1:] == [0xa8a8])
    def record(t):
        """+VRX for t s: its samples (OK after)"""
        if not m.cmd(b"AT+VRX", rb"CONNECT\r\n"):
            return []
        end, got = time.time() + t, b""
        while time.time() < end:
            if m.dte.wait(0.1):
                got += m.dte.read(65536)
        m.dte.write(b"x")
        got += m.buf
        m.buf = b""
        check("voice codec: +VRX ends", m.expect(rb"\x10\x03(\r\n)?OK\r\n", 5))
        return unstuff(got)[2000:]
    x = record(1.5)
    check("voice codec: on hook with +VLS=14", not any(v & 0x0008 for v in gpio(m)[n:]))
    check("voice codec: +VRX gets the handset's 1000 Hz (%.2f)" % (tone(x, 1000) if x else 0),
          x and tone(x, 1000) > 0.5)
    size = os.path.getsize(ear)
    check("voice codec: +VTX", m.cmd(b"AT+VTX", rb"CONNECT\r\n"))
    import math
    tx = bytes(max(0, min(255, int(128 + 60 * math.sin(2 * math.pi * 500 * i / 8000))))
               for i in range(8000))
    m.dte.write(tx.replace(b"\x10", b"\x10\x10") + b"\x10\x03")
    check("voice codec: +VTX's OK", m.expect(rb"OK\r\n", 10))
    time.sleep(0.3)
    played = [ulaw(c) for c in open(ear, "rb").read()[size:]]
    best = max((tone(played[i:i + 2000], 500), i) for i in range(0, max(1, len(played) - 2000), 1000)) if len(played) > 2000 else (0, 0)
    check("voice codec: the earpiece plays +VTX's 500 Hz (%.2f)" % best[0], best[0] > 0.5)
    n = len(gpio(m))
    check("voice codec: +VLS=15", m.cmd(b"AT+VLS=15"))
    check("voice codec: off hook with +VLS=15", any(v & 0x0008 for v in gpio(m)[n:]))
    size = os.path.getsize(ear)
    x = record(2)
    m.expect(rb"OK\r\n", 3)
    heard = [ulaw(c) for c in open(ear, "rb").read()[size:]]
    print("voice codec, +VLS=15: +VRX 1000 Hz %.2f, 350 Hz %.2f, 440 Hz %.2f; earpiece 350 Hz %.2f, 440 Hz %.2f" % (
        tone(x, 1000) if x else 0, tone(x, 350) if x else 0, tone(x, 440) if x else 0,
        tone(heard[4000:], 350) if len(heard) > 4000 else 0, tone(heard[4000:], 440) if len(heard) > 4000 else 0))
    check("voice codec: +VRX gets the handset and the line (its dial tone)",
          x and tone(x, 1000) > 0.1 and tone(x, 350) + tone(x, 440) > 0.1)
    check("voice codec: the earpiece gets the line",
          len(heard) > 4000 and tone(heard[4000:], 350) + tone(heard[4000:], 440) > 0.3)
    check("voice codec: +VLS=0", m.cmd(b"AT+VLS=0"))
    check("voice codec: on hook again", gpio(m)[-1] & 0x0008 == 0)
    check("RHOM off again", fifo2(m)[-1:] == [0x2828])
    m.p.terminate()
    m.p.wait(5)
    m = Modem("1001", "dmodem38", ["--usb-rhom"], "-rhom-0100",
              FAKE_ST7554_LOG="1", FAKE_ST7554_REVISION="0100")
    m.cmd(b"ATE0+FCLASS=8")
    m.cmd(b"AT+VLS=1")
    time.sleep(0.5)
    m.cmd(b"AT+VLS=0")
    check("revision 0x0100: no RHOM", b"no RHOM" in log_of(m) and
          all(v & 0x8000 == 0 for v in fifo2(m)))
    m.p.terminate()
    m.p.wait(5)

    m, g = intl("AUSTRALIA")
    check("Australia: off hook DP and RFC, RFC off after 350 ms, all off on hook",
          g[:3] == [0x0588, 0x0488, 0x0000])
    n = len(gpio(m))
    m.dte.write(b"ATX0DP1\r")
    end = time.time() + 10
    while time.time() < end and b"pulse dialling over" not in log_of(m):
        time.sleep(0.1)
    g = gpio(m)[n:]
    check("Australia: pulse dialled with RFC and DP set (0x0580/0x0588), RFC off after",
          0x0580 in g and 0x0588 in g and g[-1] == 0x0488)
    m.cmd(b"ATH", rb"(OK|NO CARRIER)\r\n")
    m.p.terminate()
    m.p.wait(5)
    m, g = intl("FRANCE")
    check("France: DC off hook (0x1488)", g[:2] == [0x1488, 0x0000])
    m.p.terminate()
    m.p.wait(5)
    m, g = intl("USA", FAKE_ST7554_DAA="1")
    check("an ST75951 in the USA: DC the other way (0x1488)", g[:2] == [0x1488, 0x0000])
    m.p.terminate()
    m.p.wait(5)

    # (1001's FIFO underruns with the ring detector's bit set: no RING)
    a = Modem("1001", "dmodem41", ["--usb-leds", "--usb-buzzer"], FAKE_ST7554_LOG="1",
              FAKE_ST7554_UNDERRUNS="30")
    suspend = os.path.join(tmp, "1002.suspend")
    handset = os.path.join(tmp, "1002.handset")
    b = Modem("1002", "dmodem42", ["--usb-wake", "--usb-handset"],
              FAKE_ST7554_DAA="1", FAKE_ST7554_LOG="1",
              FAKE_ST7554_SUSPEND=usb_dte.file_for(dmodemd, suspend),
              FAKE_ST7554_HANDSET=usb_dte.file_for(dmodemd, handset))
    for m in (a, b):
        check("%s ATE0" % m.n, m.cmd(b"ATE0"))
        check("%s ATI3: ST7554" % m.n, m.cmd(b"ATI3", rb"ST7554\r\n(\r\n)?OK\r\n"))
    b_log = lambda: log_of(b)
    check("1002 an ST75951/2", b"codec: ST75951/2 (silicon DAA)" in b_log())
    check("1002 its start-up (register 3 = 0x050)",
          b"fake st7554: 17 = 3050" in b_log())
    check("1001 an STLC7550", b"codec: STLC7550" in log_of(a))
    check("1002 its remote wakeup armed", b"fake st7554: remote wakeup armed" in b_log())
    check("1001 its remote wakeup not armed", b"remote wakeup armed" not in log_of(a))
    # (Linux's alone: the kernel's power/wakeup, usb_line.cpp's arm_wake())
    if not usb_dte.windows(dmodemd) and sys.platform.startswith("linux"):
        check("1002 the kernel's power/wakeup tried",
              b"1-250/power/wakeup" in b_log())
    # (the core's ring check 2.5 s after a burst's first pulse)
    t = time.time()
    while b"FIFO underrun" not in log_of(a) and time.time() - t < 10:
        time.sleep(0.1)
    check("1001's underruns logged", b"FIFO underrun" in log_of(a))
    check("1001 no RING from its underruns", not a.expect(rb"RING\r\n", 4)
          and b"bad ring" not in log_of(a))
    check("1002 answers at the 2nd ring", b.cmd(b"ATS0=2"))
    check("1002 caller ID on", b.cmd(b"AT#CID=1"))
    check("1001 waits for the dial tone", a.cmd(b"ATX4"))

    t = time.time()
    a.dte.write(b"ATD1002\r")
    check("1002 RING", b.expect(rb"RING\r\n", 15))
    cid = b.expect(rb"NMBR = (\d+)\r\n", 6)
    check("1002 caller ID 1001", cid and cid.group(1) == b"1001")
    check("1002 CID1 set on hook", b"fake st7554: 0a = 0020" in b_log())
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
    g = gpio(a)
    on = [i for i, v in enumerate(g) if v & 0x0800]
    check("1001 its buzzer (0x0800) on as it dials, off at the CONNECT",
          on and any(not v & 0x0800 for v in g[on[0]:]) and 0x00c8 in g[on[-1]:])
    check("1002 no buzzer (no --usb-buzzer)", not any(v & 0x0800 for v in gpio(b)))
    check("1001 the carrier LED (0x40) lit", 0x00c8 in gpio(a))
    check("1001 the data LED (0x2000) lit", 0x20c8 in gpio(a))
    time.sleep(1.2)
    a.dte.write(b"+++")
    check("1001 +++ OK", a.expect(rb"OK\r\n", 5))
    check("1001 ATH", a.cmd(b"ATH"))
    check("1001 its LEDs off on hook", gpio(a)[-1] & 0x20c8 == 0)
    check("1002 no LEDs (no --usb-leds)", not any(v & 0x2040 for v in gpio(b)))
    check("1001 NO CARRIER", a.expect(rb"NO CARRIER\r\n", 5))
    check("1002 NO CARRIER", b.expect(rb"NO CARRIER\r\n", 30))

    check("1002 off hook", b.cmd(b"ATH1"))
    m = a.cmd(b"ATD1002", rb"(CONNECT[^\r]*|NO CARRIER|BUSY|NO DIALTONE|NO ANSWER)\r\n", 30)
    check("1001 BUSY", m and m.group(1) == b"BUSY")
    check("1002 on hook", b.cmd(b"ATH"))

    # pulse dialling: the hook GPIO broken and made
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
    check("1002 +VLS=? lists event 8 (the local phone)",
          b.cmd(b"AT+VLS=?", rb'0,"",00800000,00800000,90800000\r\n1,"T",0B843060'))
    open(handset, "w").close()
    check("1002 the local phone off hook: <DLE>H", b.expect(rb"\x10H", 5))
    os.remove(handset)
    check("1002 the local phone on hook: <DLE>h", b.expect(rb"\x10h", 5))
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

    # wake on ring: 1002 asleep, 1001's call wakes it
    check("1001 data mode", a.cmd(b"AT+FCLASS=0"))
    check("1002 data mode, answers at the 1st ring", b.cmd(b"AT+FCLASS=0;S0=1"))
    def wait_log(m, pat, t):
        """m's log has pat (a regular expression) within t s"""
        end = time.time() + t
        while time.time() < end:
            if re.search(pat, log_of(m)):
                return True
            time.sleep(0.1)
        return False
    open(suspend, "w").close()
    check("1002 suspended", wait_log(b, b"fake st7554: suspended", 5))
    a.dte.write(b"ATD1002\r")
    check("1002's ring wakes it", wait_log(b, rb"fake st7554: remote wakeup\r?\n", 20))
    os.remove(suspend)
    check("1002 resumed", wait_log(b, b"usb: resumed", 10))
    check("1002 its remote wakeup armed again",
          log_of(b).count(b"fake st7554: remote wakeup armed") >= 2)
    check("1002 RING (woken)", b.expect(rb"RING\r\n", 20))
    ca = a.expect(rb"(CONNECT[^\r]*|NO CARRIER|BUSY|NO DIALTONE|NO ANSWER)\r\n", 60)
    cb = b.expect(rb"(CONNECT[^\r]*|NO CARRIER)\r\n", 10)
    check("woken: both CONNECT", ca and cb and ca.group(1).startswith(b"CONNECT")
          and cb.group(1).startswith(b"CONNECT"))
    time.sleep(1.2)
    a.dte.write(b"+++")
    check("1001 +++ OK (woken)", a.expect(rb"OK\r\n", 5))
    check("1001 ATH (woken)", a.cmd(b"ATH"))

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
