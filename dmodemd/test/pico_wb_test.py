#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
# pico_wb_test.py <dmodemd> <wb_pair> <fake libusb>: dmodemd on a
# pico-serial WB stick (--usb), two of them on fake sticks
# (fake_pico_wb.cpp, a libusb in front of the real one through
# LD_PRELOAD) at the two ends of wb_pair's simulated bare pair, 1001 and
# 1002:
#
# - a wrong <bus>:<address> refused; --usb list has the stick;
# - a rate the stick hasn't (-r 9600) refused, said so; a stick whose
#   codec does not come up (FAKE_PICO_WB_NOCODEC): said, and dmodemd goes
#   on; a gain out of range refused; -r 192000 and 48000 set the stick's
#   rate (SET_RATE 192, 48);
# - with no -r the stick's 96000 Hz (SET_RATE 96), V.wb the modulation
#   (+MS?: 200), its loopback off and the hybrid input streamed;
#   --usb-wb-tx-gain and --usb-wb-rx-gain (SET_TX_GAIN, SET_RX_GAIN);
#   ATI3;
# - AT&L1 both ends, 1002 answering by itself (S0=1): 1001's ATD, both
#   CONNECT (V.wb), the LINK LEDs lit (SET_CARRIER 1); data both ways at
#   once, byte for byte; 1001 hangs up (+++ ATH), 1002 NO CARRIER (its
#   S7, 10 s, without the carrier: a leased line has no hang-up of its
#   own), both LEDs off again (SET_CARRIER 0);
# - the pair gone (the devices unplugged): both dmodemd say so and exit.
#
# A Windows dmodemd (<dmodemd> a .exe, built with the fake in libusb's
# place; <fake libusb> "-") runs under Wine, its DTE TCP, the fake's
# connections to the pair relayed from TCP (usb_dte.py).
#
# The calls run in real time (the pair's clock).  DATA=<octets> each way
# (32768); DMODEMD_DEBUG dmodemd's -d (3).  The logs (pair.log,
# dmodemd-<end>.log) go to DMODEM_TEST_DIR, without it to the temporary
# directory, kept with KEEP=1 (and printed).
import os, random, re, shutil, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import usb_dte  # noqa: E402

dmodemd, pair, fake = sys.argv[1:4]
fake = os.path.abspath(fake) if fake != "-" else None
run = usb_dte.command(dmodemd)
tmp = tempfile.mkdtemp(prefix="dmodem-pico-wb-")
os.chmod(tmp, 0o755)
logs = os.environ.get("DMODEM_TEST_DIR") or tmp
debug = os.environ.get("DMODEMD_DEBUG", "3")
data_len = int(os.environ.get("DATA", "32768"))


def env_for(n, **more):
    e = dict(os.environ, FAKE_PICO_WB_SOCKET=usb_dte.socket_for(
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

pair_log = open(os.path.join(logs, "pair.log"), "w")
events = []
px = subprocess.Popen([pair, "96000", tmp, "1001", "1002"], stdin=subprocess.PIPE,
                      stdout=subprocess.PIPE, text=True)
procs.append(px)
def pair_out():
    for l in px.stdout:
        pair_log.write("%.2f %s" % (time.time(), l)); pair_log.flush()
        events.append(l.strip())
threading.Thread(target=pair_out, daemon=True).start()
t0 = time.time()
while "ready" not in events and time.time() - t0 < 5:
    time.sleep(0.05)

class Modem:
    """a dmodemd on end <n>'s pico-serial WB, its pty"""
    def __init__(self, n, name, tag="", args=(), **env):
        self.log = open(os.path.join(logs, "dmodemd-%s%s.log" % (n, tag)), "wb")
        self.p = subprocess.Popen(run + usb_dte.dte_args(dmodemd) + ["-n", "-d" + debug, "--usb=auto"]
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
        if not self.dte.sock:
            os.set_blocking(self.dte.fd, False)
        self.buf = b""
        self.n = n

    def logged(self):
        self.log.flush()
        return open(self.log.name, "rb").read()

    def expect_log(self, text, t, count=1):
        """text in dmodemd's output (its log) within t s (count times)"""
        end = time.time() + t
        while time.time() < end:
            if self.logged().count(text) >= count:
                return True
            time.sleep(0.1)
        return False

    def take(self):
        """what the DTE has now, onto buf"""
        try:
            self.buf += self.dte.read(65536)
        except (BlockingIOError, InterruptedError):
            pass

    def expect(self, pat, t):
        end = time.time() + t
        while time.time() < end:
            m = re.search(pat, self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            if self.dte.wait(0.1):
                try:
                    self.take()
                except OSError:
                    return None
        return None

    def send_some(self, b):
        """as much of b as the DTE takes now: how much"""
        if self.dte.sock:
            try:
                return self.dte.sock.send(b)
            except BlockingIOError:
                return 0
        try:
            return os.write(self.dte.fd, b)
        except BlockingIOError:
            return 0

    def cmd(self, c, pat=rb"OK\r\n", t=5):
        self.dte.write(c + b"\r")
        return self.expect(pat, t)

ok = True
def check(what, cond):
    global ok
    print("%s: %s" % (what, "yes" if cond else "NO"))
    ok = ok and bool(cond)

def once(args, n="1001", **env):
    """dmodemd run with args until it exits (60 s at most)"""
    return subprocess.run(run + usb_dte.dte_args(dmodemd) + ["-n", "-d" + debug] + args,
                          env=env_for(n, **env), capture_output=True, text=True, timeout=60)

def text(n, seed):
    """n octets of printable text, its own for seed"""
    r = random.Random(seed)
    s = bytes(r.choice(b"abcdefghijklmnopqrstuvwxyz0123456789 .,") for _ in range(n - n // 64))
    return b"\n".join(s[i:i + 63] for i in range(0, len(s), 63))[:n].ljust(n, b".")

try:
    p = once(["--usb=2:9", "dmodem40"])
    check("nothing at 2:9", p.returncode != 0 and
          "no SmartUSB56, pico-serial Line or WB, or Agere USB modem at 2:9" in p.stderr)
    p = subprocess.run(run + ["--usb", "list"], env=env_for("1001"), capture_output=True,
                       text=True, timeout=60)
    check("--usb list: the WB stick", "1209:0003 pico-serial WB: ready" in p.stdout)
    p = once(["--usb=auto", "-r", "9600", "dmodem40"])
    check("-r 9600 refused", p.returncode != 0 and
          "pico-serial WB: no rate of 9600 Hz" in p.stdout + p.stderr)
    p = once(["--usb=auto", "--usb-wb-tx-gain=30", "dmodem40"])
    check("a transmit gain out of range refused", p.returncode != 0 and
          "--usb-wb-tx-gain: dB from -63.5 to 24" in p.stderr)
    for r, k in (("192000", b"192"), ("48000", b"48")):
        m = Modem("1001", "dmodem40", "-" + r, ["-r", r], FAKE_PICO_WB_LOG="1")
        check("-r %s: SET_RATE %s" % (r, k.decode()),
              m.expect_log(b"request 10 out " + k + b": 0", 5) and
              m.expect_log(b"usb: " + r.encode() + b" Hz", 5))
        m.p.terminate()
        m.p.wait(5)
    nc = Modem("1001", "dmodem40", "-nocodec", FAKE_PICO_WB_NOCODEC="1")
    check("a codec that does not run: said", nc.expect_log(b"its codec does not run", 5))
    nc.p.terminate()
    nc.p.wait(5)

    a = Modem("1001", "dmodem41", FAKE_PICO_WB_LOG="1")
    b = Modem("1002", "dmodem42", "", ["--usb-wb-tx-gain=-3", "--usb-wb-rx-gain=6"],
              FAKE_PICO_WB_LOG="1")
    check("no -r: the stick's 96000 Hz", a.expect_log(b"request 10 out 96: 0", 5) and
          a.expect_log(b"usb: 96000 Hz", 5))
    check("the loopback off, the hybrid streamed", a.expect_log(b"request 15 out 0: 0", 5) and
          a.expect_log(b"request 14 out 0: 0", 5))
    check("--usb-wb-tx-gain=-3: SET_TX_GAIN -6", b.expect_log(b"request 12 out 65530: 0", 5))
    check("--usb-wb-rx-gain=6: SET_RX_GAIN 12", b.expect_log(b"request 13 out 12: 0", 5))
    check("the LINK LED off at the start", a.expect_log(b"request 11 out 0: 0", 5))
    for m in (a, b):
        check("%s ATE0" % m.n, m.cmd(b"ATE0"))
        check("%s ATI3: pico-serial WB" % m.n,
              m.cmd(b"ATI3", rb"pico-serial WB \(TLV320AIC3204\)\r\n(\r\n)?OK\r\n"))
        check("%s +MS?: V.wb (200)" % m.n, m.cmd(b"AT+MS?", rb"200,\d+,\d+,\d+\r\n(\r\n)?OK\r\n"))
    check("1002 a leased line, answering by itself", b.cmd(b"AT&L1S0=1S7=10"))
    check("1001 a leased line", a.cmd(b"AT&L1"))

    # (a start-up that fails, under a loaded machine's stalls, is tried
    # again by itself: the leased line's)
    t = time.time()
    a.dte.write(b"ATD\r")
    ca = a.expect(rb"(CONNECT[^\r]*)\r\n", 60)
    cb = b.expect(rb"(CONNECT[^\r]*)\r\n", 15)
    print("1001: %s, 1002: %s, %.1f s" % (ca and ca.group(1).decode(),
                                         cb and cb.group(1).decode(), time.time() - t))
    rates = []
    for m in (ca, cb):
        r = m and re.match(rb"CONNECT (\d+)", m.group(1))
        if r:
            rates.append(int(r.group(1)))
    check("both CONNECT, V.wb's rates", len(rates) == 2 and min(rates) > 50000)
    check("both LINK LEDs lit", a.expect_log(b"request 11 out 1: 0", 5) and
          b.expect_log(b"request 11 out 1: 0", 5))

    # data both ways at once
    da, db = text(data_len, 1), text(data_len, 2)
    sa = sb = 0
    t = time.time()
    while time.time() - t < 60 and (len(b.buf) < data_len or len(a.buf) < data_len):
        if sa < len(da):
            sa += a.send_some(da[sa:sa + 4096])
        if sb < len(db):
            sb += b.send_some(db[sb:sb + 4096])
        for m in (a, b):
            try:
                m.take()
            except OSError:
                pass
        time.sleep(0.01)
    took = time.time() - t
    print("%d octets each way in %.1f s" % (data_len, took))
    check("1002 got 1001's data, byte for byte", b.buf[:data_len] == da)
    check("1001 got 1002's data, byte for byte", a.buf[:data_len] == db)
    a.buf = b.buf = b""

    time.sleep(1.2)
    a.dte.write(b"+++")
    check("1001 +++ OK", a.expect(rb"OK\r\n", 5))
    check("1001 ATH", a.cmd(b"ATH"))
    check("1002 NO CARRIER", b.expect(rb"NO CARRIER\r\n", 30))
    check("both LINK LEDs off again", a.expect_log(b"request 11 out 0: 0", 5, 2) and
          b.expect_log(b"request 11 out 0: 0", 5, 2))

    px.terminate()
    px.wait(5)
    for m in (a, b):
        try:
            r = m.p.wait(10)
        except subprocess.TimeoutExpired:
            r = None
        check("%s exits once unplugged" % m.n, r is not None and r != 0 and
              b"pico-serial WB: unplugged" in m.logged())
finally:
    cleanup()
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
