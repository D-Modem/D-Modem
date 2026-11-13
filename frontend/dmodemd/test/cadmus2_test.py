#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# cadmus2_test.py <dmodemd> <line_exchange> <fake libusb>: dmodemd on a
# Conexant USB HSF modem (--usb: the Cadmus 2 bridge), two of them on
# fake modems (fake_cadmus2.cpp, a libusb in front of the real one through
# LD_PRELOAD, its bridge running the scripts as the harness's
# dsp/test/hw/cadmus2.hpp does) whose lines are line_exchange's, 1001 and
# 1002, at 16000 Hz (the modems' 9600):
#
# - a modem with no firmware and no DMODEM_CD2_FIRMWARE: dmodemd says
#   where Conexant's is; a SmartDAA board: not taken, said;
# - its firmware (a stand-in, in c2firmware.h's form) uploaded from
#   DMODEM_CD2_FIRMWARE, every octet, with the device unconfigured;
# - -r 8000: the modem at 9600 Hz all the same (its samples converted);
# - nobody calls: no RING (the monitor's lone report at the start);
# - 1001 dials 1002 (ATX4: after the dial tone, in DTMF); 1002 rings
#   (the monitor's reports) and answers at the second ring (ATS0=2);
#   both CONNECT, a line of text goes each way; 1001 hangs up (+++ ATH),
#   1002 NO CARRIER;
# - the exchange gone (the devices unplugged): both dmodemd say so and
#   exit.
#
# A dmodemd built with the fake in libusb's place (<fake libusb> "-")
# runs the same.  The calls run in real time (the exchange's clock).
# DMODEMD_DEBUG dmodemd's -d (3).  The logs (exchange.log,
# dmodemd-<line>.log) go to DMODEM_TEST_DIR, without it to the temporary
# directory, kept with KEEP=1 (and printed).
import os, re, shutil, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import usb_dte  # noqa: E402

dmodemd, exchange, fake = sys.argv[1:4]
fake = os.path.abspath(fake) if fake != "-" else None
run = usb_dte.command(dmodemd)
rate = "16000"   # the exchange's, the device's
mrate = "9600"   # the modem's
tmp = tempfile.mkdtemp(prefix="dmodem-cadmus2-")
os.chmod(tmp, 0o755)
logs = os.environ.get("DMODEM_TEST_DIR") or tmp
debug = os.environ.get("DMODEMD_DEBUG", "3")

# a stand-in for Conexant's firmware, as c2firmware.h has it (not
# theirs: its size, 7395 octets, and a pattern)
FW_SIZE = 7395
firmware = os.path.join(tmp, "c2firmware.h")
with open(firmware, "w") as f:
    f.write("/* a stand-in */\n\nunsigned char ROM_IMAGE[]=\n{\n")
    octets = ["0x%02X" % ((i * 7 + 3) & 0xff) for i in range(FW_SIZE)]
    f.write(",\n".join(",".join(octets[i:i + 8]) for i in range(0, FW_SIZE, 8)))
    f.write("\n};\n")


def env_for(n, **more):
    """the environment for end <n>'s dmodemd: its fake modem's socket, the fake preloaded, and more"""
    e = dict(os.environ, FAKE_CD2_SOCKET=usb_dte.socket_for(
        dmodemd, os.path.join(tmp, n + ".sock")))
    if fake:
        e["LD_PRELOAD"] = fake
    e.update(more)
    return e

procs = []
def cleanup():
    """stop every process started here, then remove the scratch directory (unless KEEP)"""
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
    """the exchange's output, line by line, into exchange.log and events"""
    for l in ex.stdout:
        ex_log.write("%.2f %s" % (time.time(), l)); ex_log.flush()
        events.append(l.strip())
threading.Thread(target=exchange_out, daemon=True).start()
t0 = time.time()
while "ready" not in events and time.time() - t0 < 5:
    time.sleep(0.05)

class Modem:
    """a dmodemd on line <n>'s Conexant USB HSF modem, its pty"""
    def __init__(self, n, name, tag="", args=(), r=mrate, **env):
        """start dmodemd as <name> on end <n>'s fake modem and wait for its pty (60 s at most)"""
        self.log = open(os.path.join(logs, "dmodemd-%s%s.log" % (n, tag)), "wb")
        self.p = subprocess.Popen(run + usb_dte.dte_args(dmodemd) + usb_dte.realtime() + ["-d" + debug, "--usb=auto", "-r", r]
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
        """the first match of pat in what the DTE has sent within t s, the text before it and itself taken off buf; None if none"""
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
        """send AT command c and wait t s for pat (OK by default): the match, or None"""
        self.dte.write(c + b"\r")
        return self.expect(pat, t)

ok = True
def check(what, cond):
    """print what was checked and whether it held; any failure fails the test"""
    global ok
    print("%s: %s" % (what, "yes" if cond else "NO"))
    ok = ok and bool(cond)

def fails(**env):
    """dmodemd on 1001's modem with env, which is to fail: its stderr"""
    p = subprocess.run(run + usb_dte.dte_args(dmodemd) + usb_dte.realtime() + ["--usb=auto", "-r", mrate, "dmodem40"],
                       env=env_for("1001", **env), capture_output=True, text=True, timeout=60)
    return p.returncode != 0, p.stderr

try:
    failed, err = fails(DMODEM_CD2_FIRMWARE="")
    check("no firmware: where Conexant's is, said", failed and "it has no firmware: DMODEM_CD2_FIRMWARE" in err)
    failed, err = fails(DMODEM_CD2_FIRMWARE=firmware, FAKE_CD2_SMART="1")
    check("a SmartDAA board: not taken, said", failed and "a SmartDAA board" in err)

    a = Modem("1001", "dmodem41", r="8000", FAKE_CD2_LOG="1", DMODEM_CD2_FIRMWARE=firmware)
    check("1001: its firmware from DMODEM_CD2_FIRMWARE",
          a.expect_log(b"its firmware, %d octets, from" % FW_SIZE, 5))
    check("1001: the firmware uploaded whole, unconfigured, and runs",
          a.expect_log(b"fake Cadmus 2: configuration -1", 5) and
          a.expect_log(b"firmware of %d octets runs" % FW_SIZE, 5) and
          a.expect_log(b"fake Cadmus 2: configuration 1", 5))
    check("1001: a plain board", a.expect_log(b"a plain board (no SmartDAA)", 5))
    check("1001: -r 8000, 9600 Hz all the same", a.expect_log(b"9600 Hz, its rate (not 8000)", 5))
    check("1001: the codec's words at the start",
          all(a.expect_log(b"codec word " + w, 5) for w in (b"2004", b"a208", b"f200", b"aae8", b"6040", b"35b4")))
    b = Modem("1002", "dmodem42", FAKE_CD2_LOADED="1")
    check("1002: its firmware already runs", b.expect_log(b"its firmware runs", 5))
    for m in (a, b):
        check("%s ATE0" % m.n, m.cmd(b"ATE0"))
        check("%s ATI3: Conexant USB HSF" % m.n,
              m.cmd(b"ATI3", rb"Conexant USB HSF \(Cadmus 2\)\r\n(\r\n)?OK\r\n"))
    check("1002 answers at the 2nd ring", b.cmd(b"ATS0=2"))
    check("1002 no ring while nobody calls",
          not b.expect_log(b"the line rings", 2) and not b.expect(rb"RING", 0.5))
    check("1001 waits for the dial tone", a.cmd(b"ATX4"))

    t = time.time()
    a.dte.write(b"ATD1002\r")
    check("1001 off hook: aac8", a.expect_log(b"codec word aac8", 10))
    check("1002 RING", b.expect(rb"RING\r\n", 15))
    check("1002 RING again", b.expect(rb"RING\r\n", 8))
    ca = a.expect(rb"(CONNECT[^\r]*|NO CARRIER|BUSY|NO DIALTONE)\r\n", 60)
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

    ex.terminate()
    ex.wait(5)
    for m in (a, b):
        try:
            r = m.p.wait(10)
        except subprocess.TimeoutExpired:
            r = None
        m.log.flush()
        check("%s exits once unplugged" % m.n, r is not None and r != 0 and
              b"Conexant USB HSF modem: unplugged" in open(m.log.name, "rb").read())
finally:
    cleanup()
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
