#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
# agere_test.py <dmodemd> <line_exchange> <fake libusb>|-: dmodemd on an
# Agere USB 2.0 soft modem (--usb), two of them on fake ones
# (fake_agere.cpp, a libusb in front of the real one through LD_PRELOAD)
# whose lines are line_exchange's, 1001 and 1002, at 8000 Hz:
#
# - no patch is sent (command 9 stalls in the fake); a "Polaris" DAA
#   (its codec does not answer): said; a full speed device: said;
# - the ROM's stream (alternate setting 1, command 0x0c as Windows' driver
#   sends it): its samples come at 8000 a second; with a bulk patch's
#   entries left in the ROM's function table (FAKE_AGERE_PATCHED) they are
#   put back first, and it streams; a ROM 1.0.6 streams, its clock left
#   as it is; DMODEM_AGERE_TRANSFERS and DMODEM_AGERE_DUMP;
# - -r 9600: 8000 Hz all the same;
# - 1001 dials 1002 (ATX4: after the dial tone);
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
fake = os.path.abspath(fake) if fake != "-" else None
run = usb_dte.command(dmodemd)
rate = "8000"
tmp = tempfile.mkdtemp(prefix="dmodem-agere-")
os.chmod(tmp, 0o755)
logs = os.environ.get("DMODEM_TEST_DIR") or tmp
debug = os.environ.get("DMODEMD_DEBUG", "3")

def env_for(n, **more):
    e = dict(os.environ, FAKE_AGERE_SOCKET=os.path.join(tmp, n + ".sock"))
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
    """a dmodemd on line <n>'s Agere modem, its pty"""
    def __init__(self, n, name, tag="", args=(), **env):
        self.log = open(os.path.join(logs, "dmodemd-%s%s.log" % (n, tag)), "wb")
        self.p = subprocess.Popen(run + ["-n", "-d" + debug, "--usb=auto", "-r", rate]
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

    def stop(self):
        self.p.terminate()
        self.p.wait(5)

ok = True
def samples_come(m, t=15):
    """m's rate check within t s: about 8000 a second (the fake's clock
    is the host's: a sample or so either way under load)"""
    end = time.time() + t
    while time.time() < end:
        m.log.flush()
        if re.search(rb"Agere: (79\d\d|80\d\d) samples a second", open(m.log.name, "rb").read()):
            return True
        time.sleep(0.2)
    return False


def led(m, which):
    """m's LED which (b"OH") as the fake last said it:
    b"on", b"off" or None"""
    m.log.flush()
    found = re.findall(rb"fake Agere: LED " + re.escape(which) + rb" (on|off)",
                       open(m.log.name, "rb").read())
    return found[-1] if found else None


def led_changes(m, which):
    """how many times the fake has said m's LED which changed"""
    m.log.flush()
    return open(m.log.name, "rb").read().count(b"fake Agere: LED " + which + b" ")


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

try:
    check("full speed: said", fails("not at high speed", FAKE_AGERE_FULL="1"))

    # the ROM's stream: from a function table as the ROM has it, or with
    # a bulk patch's entries put back first; a ROM 1.0.6's clock left alone
    for tag, env in (("-rom", {"FAKE_AGERE_NO_ACTIVE": "1"}), ("-patched", {"FAKE_AGERE_PATCHED": "1"}),
                     ("-v106", {"FAKE_AGERE_ROM": "106"})):
        m = Modem("1001", "dmodem40", tag, args=["-d2"], **env)
        check("%s: alternate setting 1" % tag,
              m.expect_log(b"Agere: isochronous out 02, in 82, 16 octets a packet", 5))
        if tag == "-rom":
            check("-rom: no active configuration said, its first taken",
                  m.expect_log(b"no active configuration said: its first", 1))
        if tag == "-patched":
            check("-patched: entry 4 put back",
                  m.expect_log(b"function table entry 4 70001588 (a patch's), put back", 5))
        if tag == "-v106":
            check("-v106: its clock left as it is",
                  m.expect_log(b"ROM 1.0.6: its clock left as it is", 5))
        check("%s: ATE0" % tag, m.cmd(b"ATE0"))
        check("%s: its samples come" % tag, samples_come(m))
        st = re.search(rb"Agere: streaming:.*", open(m.log.name, "rb").read())
        check("%s: Windows' first step (0xcd000044, codec 0x0b0)" % tag,
              st and b"cd000044 00000003" in st.group(0) and b"codec 0b0 000a" in st.group(0))
        log = open(m.log.name, "rb").read()
        check("%s: no patch sent" % tag, b"a patch (command 9" not in log)
        check("%s: none stuck" % tag, b"no samples" not in log)
        m.stop()

    # DMODEM_AGERE_TRANSFERS, DMODEM_AGERE_DUMP
    m = Modem("1001", "dmodem40", "-transfers", args=["-d1"], DMODEM_AGERE_TRANSFERS="6",
              DMODEM_AGERE_DUMP="1")
    check("-dump: the endpoints", m.expect_log(b"Agere: dump: interface 0 alt 1: endpoint 82", 10))
    check("-dump: over", m.expect_log(b"Agere: dump over", 20))
    check("-transfers: its samples come", samples_come(m))
    m.stop()

    # the LED probe: each GPIO flipped in turn, logged, then put back
    m = Modem("1001", "dmodem40", "-probe", args=["-d2"], DMODEM_AGERE_LED_PROBE="20")
    check("-probe: over", m.expect_log(b"Agere: LED probe over", 10))
    log = open(m.log.name, "rb").read()
    check("-probe: GPIO 4 high, read back", b"step 5: GPIO 4 high (wrote 0000001c, read 0000001c)" in log)
    check("-probe: GPIO 20 high", b"step 21: GPIO 20 high (wrote 0010000c" in log)
    check("-probe: OH lit by it", b"fake Agere: LED OH on" in log.split(b"LED probe over")[0])
    check("-probe: its samples come after", samples_come(m))
    m.stop()

    # 1001 a Mercury codec, 1002 a Polaris DAA (as the Hiro H50113: its rings
    # in codec register 0x0a8's bit 1, not the IN status word)
    # (its recordings' folder: dmodemd may run as another user)
    recs = os.path.join(tmp, "recs")
    os.mkdir(recs)
    os.chmod(recs, 0o777)
    a = Modem("1001", "dmodem41", args=["-r", "9600", "-d2"],
              DMODEM_USB_REC=os.path.join(recs, "line"), DMODEM_USB_TX_GAIN="auto")
    # (1002's codec's clock 300 ppm faster than its USB's: its transmit ring
    # kept by the OUT transfers' corrections, no overrun or underrun)
    b = Modem("1002", "dmodem42", args=["-d2"], FAKE_AGERE_DRIFT="300", DMODEM_USB_TX_GAIN="3",
              FAKE_AGERE_POLARIS="1", FAKE_AGERE_RING_STICKS="1")
    check("-r 9600: 8000 Hz all the same", a.expect_log(b"8000 Hz, its rate (not 9600)", 5))
    for m in (a, b):
        check("%s ATE0" % m.n, m.cmd(b"ATE0"))
        check("%s ATI3: Agere" % m.n,
              m.cmd(b"ATI3", rb"Agere USB 2.0 \(Mercury\)\r\n(\r\n)?OK\r\n"))
    check("1002 answers at the 2nd ring", b.cmd(b"ATS0=2"))
    check("1002 caller ID on", b.cmd(b"AT#CID=1"))
    check("1001 waits for the dial tone", a.cmd(b"ATX4"))

    t = time.time()
    check("1002 a Polaris, its link up", b.expect_log(b"Agere: a Polaris DAA", 1)
          and b.expect_log(b"fake Agere: Polaris link up", 1)
          and b.expect_log(b"Agere: Polaris link up", 1))
    check("1002 Polaris calibrated (13)", b.expect_log(b"Agere: Polaris calibration 13 (16 readings)", 1))
    check("1002 Polaris DAA revision 3", b.expect_log(b"Agere: Polaris DAA revision 3", 1))
    check("1001's OH not lit", led(a, b"OH") == b"off")
    check("1001's gain auto, +6 dB to start", a.expect_log(b"to the line +6.0 dB to start", 1))
    check("1002's samples 3 dB louder (DMODEM_USB_TX_GAIN)", b.expect_log(b"to the line +3.0 dB", 1))
    a.dte.write(b"ATD1002\r")
    time.sleep(0.5)
    check("1001's OH lit off hook", led(a, b"OH") == b"on")
    check("1001's rings left alone off hook", b"fake Agere: rings" not in open(a.log.name, "rb").read())
    check("1002 RING", b.expect(rb"RING\r\n", 15))
    check("1002's Polaris rang (0x0a8)", b.expect_log(b"Agere: the line rings", 1))
    n = led_changes(b, b"OH")
    time.sleep(0.6)
    check("1002's OH blinks while it rings", led_changes(b, b"OH") - n >= 2)
    cid = b.expect(rb"NMBR = (\d+)\r\n", 6)
    check("1002 caller ID 1001", cid and cid.group(1) == b"1001")
    check("1002 RING again", b.expect(rb"RING\r\n", 8))
    ca = a.expect(rb"(CONNECT[^\r]*|NO CARRIER|BUSY|NO DIALTONE)\r\n", 40)
    cb = b.expect(rb"(CONNECT[^\r]*|NO CARRIER)\r\n", 5)
    print("1001: %s, 1002: %s, %.1f s" % (ca and ca.group(1).decode(),
                                         cb and cb.group(1).decode(), time.time() - t))
    check("1001's gain from 1002's answer tone (auto)",
          a.expect_log(b"the remote's answer tone at", 1))
    check("both CONNECT", ca and cb and ca.group(1).startswith(b"CONNECT")
          and cb.group(1).startswith(b"CONNECT"))
    time.sleep(0.5)
    n = led_changes(a, b"OH")
    time.sleep(1.5)
    check("1001's OH steady online", led(a, b"OH") == b"on" and led_changes(a, b"OH") == n)
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
    time.sleep(0.5)
    check("1001's OH off on hook", led(a, b"OH") == b"off")
    for m in (a, b):
        log = open(m.log.name, "rb").read()
        check("%s's transmit ring kept (no overrun, no underrun)" % m.n,
              b"transmit ring overrun" not in log and b"transmit ring underrun" not in log)
    # the call recorded (DMODEM_USB_REC): a stereo WAV at 8000 Hz, the line's sound in it
    wavs = sorted(f for f in os.listdir(recs) if f.startswith("line-") and f.endswith(".wav"))
    w = open(os.path.join(recs, wavs[0]), "rb").read() if wavs else b""
    check("1001's call recorded", len(w) > 44 + 8000 * 4 * 10 and w[:4] == b"RIFF"
          and int.from_bytes(w[22:24], "little") == 2 and int.from_bytes(w[24:28], "little") == 8000
          and int.from_bytes(w[40:44], "little") == len(w) - 44)

    # pulse dialling: the hook (codec register 0xc0) broken and made
    # (its ring bit stuck for 10 s after the call, as the first Polaris's:
    # no ring for it, the ring's halves in 0x0c4 not changing)
    check("1002 no ring after the call (its ring bit stuck)", not b.expect(rb"RING\r\n", 4))
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
