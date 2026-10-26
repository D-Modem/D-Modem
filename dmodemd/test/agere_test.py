#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
# agere_test.py <dmodemd> <line_exchange> <fake libusb>: dmodemd on an
# Agere USB 2.0 soft modem (--usb), two of them on fake ones
# (fake_agere.cpp, a libusb in front of the real one through LD_PRELOAD)
# whose lines are line_exchange's, 1001 and 1002, at 8000 Hz:
#
# - no --usb-agere-driver: dmodemd takes the patches D-Modem ships
#   (firmware/agere: Agere's own, which the fake refuses, it wanting the
#   tests'); with none there (DMODEM_AGERE_FIRMWARE an empty directory)
#   it says so; a driver without the patches: said; a "Polaris" DAA (its
#   codec does not answer): said;
# - the patches found in a driver's file (here the tests' patches,
#   DMODEM_AGERE_TEST_PATCHES=1, in a file of other octets, in a
#   directory): a ROM 1.2.1 in ROM takes bulk_patch_v121_rom, one in
#   flash bulk_patch_v121_flash, a ROM 1.0.6 bulk_patch_v106 and the
#   clock change patch; the device streams only once the right one is
#   in (fake_agere.cpp);
# - -r 9600: 8000 Hz all the same;
# - 1001 (a full speed device) dials 1002 (ATX4: after the dial tone);
#   1002 rings (the ring bit in the IN transfers' status), shows the
#   caller ID between the first two rings (the codec hears the line on
#   hook), answers at the second ring (ATS0=2); both CONNECT, a line of
#   text goes each way; 1001 hangs up (+++ ATH), 1002 NO CARRIER;
# - 1001 dials 1002 in pulses (ATDP: codec register 0xc0 broken and
#   made): 1002 rings;
# - the exchange gone (the devices unplugged): both dmodemd say so and
#   exit.
#
# The calls run in real time (the exchange's clock).  DMODEMD_DEBUG
# dmodemd's -d (3).  The logs (exchange.log, dmodemd-<line>.log) go to
# DMODEM_TEST_DIR, without it to the temporary directory, kept with
# KEEP=1 (and printed).
import os, re, shutil, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import usb_dte  # noqa: E402

dmodemd, exchange, fake = sys.argv[1:4]
fake = os.path.abspath(fake)
run = usb_dte.command(dmodemd)
rate = "8000"
tmp = tempfile.mkdtemp(prefix="dmodem-agere-")
os.chmod(tmp, 0o755)
logs = os.environ.get("DMODEM_TEST_DIR") or tmp
debug = os.environ.get("DMODEMD_DEBUG", "3")

# the tests' patches (agere_patch.h's test_patch()), in a "driver" of
# other octets, in a directory of its own (with a file without them)
LENS = (748, 748, 1156, 508)
def test_patch(i):
    return bytes((k * 31 + i * 77 + 5 + (k >> 8)) & 0xff for k in range(LENS[i]))
driver_dir = os.path.join(tmp, "driver")
os.makedirs(os.path.join(driver_dir, "linux"))
with open(os.path.join(driver_dir, "README"), "wb") as f:
    f.write(b"no patch in here\n" * 100)
with open(os.path.join(driver_dir, "linux", "agrmodemlib.o"), "wb") as f:
    f.write(os.urandom(5000))
    for i in range(4):
        f.write(test_patch(i))
        f.write(os.urandom(333))
no_patch = os.path.join(driver_dir, "README")


def env_for(n, **more):
    e = dict(os.environ, FAKE_AGERE_SOCKET=os.path.join(tmp, n + ".sock"),
             DMODEM_AGERE_TEST_PATCHES="1", LD_PRELOAD=fake)
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
    """a dmodemd on line <n>'s Agere modem, its pty"""
    def __init__(self, n, name, tag="", args=(), driver=driver_dir, **env):
        self.log = open(os.path.join(logs, "dmodemd-%s%s.log" % (n, tag)), "wb")
        drv = ["--usb-agere-driver", driver] if driver else []
        self.p = subprocess.Popen(run + ["-n", "-d" + debug, "--usb=auto", "-r", rate]
                                  + drv + list(args) + [name],
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

    def stop(self):
        self.p.terminate()
        self.p.wait(5)

ok = True
def check(what, cond):
    global ok
    print("%s: %s" % (what, "yes" if cond else "NO"))
    ok = ok and bool(cond)

def fails(why, *args, **env):
    """dmodemd on 1001's modem fails, why in its output (-d2)"""
    p = subprocess.run(run + ["-n", "-d2", "--usb=auto", "-r", rate] + list(args) + ["dmodem40"],
                       env=env_for("1001", **env), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, timeout=60)
    return p.returncode != 0 and why in p.stdout

shipped = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                       "firmware", "agere")
empty = os.path.join(tmp, "empty")
os.makedirs(empty)

try:
    check("no --usb-agere-driver: the shipped patches",
          fails("bulk_patch_v121_rom in " + os.path.realpath(shipped).split("/firmware/")[0])
          and fails("not bulk_patch_v121_rom"))
    check("none shipped: said", fails("no bulk patch in " + empty,
                                      DMODEM_AGERE_FIRMWARE=empty))
    check("a driver without the patch: said",
          fails("no bulk patch in " + no_patch, "--usb-agere-driver", no_patch))
    check("a Polaris DAA: said", fails('a "Polaris" DAA', "--usb-agere-driver", driver_dir,
                                       FAKE_AGERE_POLARIS="1"))

    # the ROM's patches: each variant streams once its patch is in
    for tag, env, patch in (("-flash", {"FAKE_AGERE_FLASH": "1"}, b"bulk_patch_v121_flash"),
                            ("-v106", {"FAKE_AGERE_ROM": "106"}, b"bulk_patch_v106")):
        m = Modem("1001", "dmodem40", tag, args=["-d2"], **env)
        check("%s: %s downloaded" % (tag, patch.decode()), m.expect_log(b"usb: Agere: " + patch, 5))
        if tag == "-v106":
            check("-v106: the clock change patch", m.expect_log(b"usb: Agere: clock change patch", 5))
        check("%s: ATE0" % tag, m.cmd(b"ATE0"))
        check("%s: it streams" % tag, not m.expect_log(b"streaming without the bulk patch", 2))
        m.stop()

    # command 0x10's mode (in wIndex): read back from the patch's stream
    # state; a ROM that takes it from wValue gets it again there, and streams
    m = Modem("1001", "dmodem40", "-index", args=["-d2"])
    check("the stream's state read back: streaming",
          m.expect_log(b"after command 0x10: the mailbox 00 00 00 00 02 00 00 00, "
                       b"the stream's state 2 (2: streaming)", 5))
    check("not sent again", not m.expect_log(b"its mode as wValue", 1))
    m.stop()
    m = Modem("1001", "dmodem40", "-value", args=["-d2"], FAKE_AGERE_MODE_VALUE="1")
    check("-value: not streaming, said",
          m.expect_log(b"the bulk patch has not started the stream: command 0x10 again", 5))
    check("-value: streaming, its mode as wValue",
          m.expect_log(b"Agere: streaming, its mode as wValue", 5))
    check("-value: its samples come", m.expect_log(b"Agere: 8000 samples a second", 15))
    m.stop()

    # DMODEM_AGERE_CLOCK_SWEEP: the rate at each ARM clock, then streaming
    m = Modem("1001", "dmodem40", "-sweep", args=["-d2"], DMODEM_AGERE_CLOCK_SWEEP="1")
    check("-sweep: the rate at each clock",
          all(m.expect_log(("clock sweep: divider %x, " % d).encode(), 15)
              for d in (3, 2, 4, 5, 6, 8, 0xf)))
    check("-sweep: 8000 a second (the fake's clock is its own)",
          re.search(rb"clock sweep: divider 2, \d+ MHz \(ARMCDR \w+\): (79\d\d|80\d\d) samples a second",
                    open(m.log.name, "rb").read()))
    check("-sweep: then raised", m.expect_log(b"Agere: ARM clock raised", 5))
    check("-sweep: then its samples come", m.expect_log(b"Agere: 8000 samples a second", 15))
    m.stop()

    # DMODEM_AGERE_PROBE: registers dumped, 0xcd000034 swept, then streaming
    m = Modem("1001", "dmodem40", "-probe", args=["-d2"], DMODEM_AGERE_PROBE="1")
    check("-probe: the controller's registers", m.expect_log(b"Agere: probe: cd000000: ", 10))
    check("-probe: the codec's", m.expect_log(b"Agere: probe: codec 3e0: ", 15))
    check("-probe: 0xcd000034 swept", m.expect_log(b"Agere: probe: cd000034 = 7: ", 30))
    check("-probe: put back, streaming", m.expect_log(b"Agere: probe: put back: ", 5))
    check("-probe: then its samples come", m.expect_log(b"Agere: 8000 samples a second", 15))
    m.stop()

    a = Modem("1001", "dmodem41", args=["-r", "9600"], FAKE_AGERE_FULL="1")
    b = Modem("1002", "dmodem42")
    check("-r 9600: 8000 Hz all the same", a.expect_log(b"8000 Hz, its rate (not 9600)", 5))
    check("1001 at full speed", a.expect_log(b"full speed", 5))
    check("1002 bulk_patch_v121_rom", b.expect_log(b"usb: Agere: bulk_patch_v121_rom", 5))
    for m in (a, b):
        check("%s ATE0" % m.n, m.cmd(b"ATE0"))
        check("%s ATI3: Agere" % m.n,
              m.cmd(b"ATI3", rb"Agere USB 2.0 \(Mercury\)\r\n(\r\n)?OK\r\n"))
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

    # pulse dialling: the hook (codec register 0xc0) broken and made
    check("1002 no auto answer", b.cmd(b"ATS0=0"))
    a.dte.write(b"ATDP1002\r")
    check("1002 RING (pulse)", b.expect(rb"RING\r\n", 30))
    check("pulse dialled: the digits", all(
        "1001 pulse digit %s" % d in events for d in "102"))
    check("1001 ATH (pulse)", a.cmd(b"ATH", rb"(OK|NO CARRIER)\r\n", 10))

    ex.terminate()
    ex.wait(5)
    for m in (a, b):
        try:
            r = m.p.wait(10)
        except subprocess.TimeoutExpired:
            r = None
        m.log.flush()
        check("%s exits once unplugged" % m.n, r is not None and r != 0 and
              b"Agere USB modem: unplugged" in open(m.log.name, "rb").read())
finally:
    cleanup()
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
