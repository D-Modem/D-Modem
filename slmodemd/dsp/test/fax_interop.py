#!/usr/bin/env python3
#
# fax_interop.py - a class 1 fax call between two modem_half processes
# (e.g. one linked with the original dsplibs.o and one with the open
# library), with the DTE side of T.30 played by this script:
#
#     fax_interop.py <sender exe> <receiver exe> [mod] [AT setup]
#
# The sender dials, the receiver answers.  CSI/DIS, DCS, TCF, CFR, one
# page at <mod> (V.17 codes: the page uses the short train of the same
# rate), EOP, MCF and DCN are exchanged and the page data is checked.
# The modems run in real time (MODEM_REALTIME in modem_half.c).
#
# LOGDIR=<dir> puts modemA.log / modemB.log there (default: next to this
# script), RECORD=<dir> records the line audio both ways.
import os, random, re, select, subprocess, sys, time, tty, threading
exe_a, exe_b = sys.argv[1], sys.argv[2]
mod = int(sys.argv[3]) if len(sys.argv) > 3 else 96
setup = sys.argv[4] if len(sys.argv) > 4 else "ATX3"
d = os.environ.get("LOGDIR", os.path.dirname(os.path.abspath(__file__)))
ab_r, ab_w = os.pipe()
ba_r, ba_w = os.pipe()
def start(exe, name, rfd, wfd):
    def fds():
        a, b = os.dup(rfd), os.dup(wfd)
        os.dup2(a, 3); os.dup2(b, 4)
    # the DTE (this script) has to keep up with the T.30 timers
    env = dict(os.environ, MODEM_NAME=name, MODEM_REALTIME="1")
    return subprocess.Popen([exe, "--debug=3", "-e"], stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, preexec_fn=fds, close_fds=False, env=env)
if os.environ.get("RECORD"):
    # RECORD=<dir>: record the line in both directions (16 bit, 9600 Hz)
    ra_r, ra_w = os.pipe()
    rb_r, rb_w = os.pipe()
    def relay(src, dst, name):
        f = open(os.path.join(os.environ["RECORD"], name), "wb")
        while True:
            x = os.read(src, 4096)
            if not x: break
            f.write(x); f.flush(); os.write(dst, x)
    threading.Thread(target=relay, args=(ab_r, rb_w, "a2b.raw"), daemon=True).start()
    threading.Thread(target=relay, args=(ba_r, ra_w, "b2a.raw"), daemon=True).start()
    pa = start(exe_a, "modemA", ra_r, ab_w)
    pb = start(exe_b, "modemB", rb_r, ba_w)
else:
    pa = start(exe_a, "modemA", ba_r, ab_w)
    pb = start(exe_b, "modemB", ab_r, ba_w)
ptys = {}
for k, p in (("A", pa), ("B", pb)):
    t0 = time.time()
    while time.time() - t0 < 10:
        line = p.stdout.readline().decode(errors="replace")
        m = re.search(r"created (\S+): (\S+)", line)
        if m:
            ptys[k] = m.group(2); break
    lf = open(os.path.join(d, "modem%s.log" % k), "wb")
    threading.Thread(target=lambda p=p, lf=lf: [(lf.write(l), lf.flush()) for l in p.stdout], daemon=True).start()
fd = {k: os.open(v, os.O_RDWR | os.O_NOCTTY) for k, v in ptys.items()}
for f in fd.values(): tty.setraw(f)
buf = {k: b"" for k in fd}
def pump(t):
    end = time.time() + t
    while True:
        r, _, _ = select.select(list(fd.values()), [], [], 0.02)
        for k, f in fd.items():
            if f in r: buf[k] += os.read(f, 4096)
        if time.time() >= end: break
def wait(k, pat, t):
    """wait for a result code; returns the text before it and the code"""
    end = time.time() + t
    while time.time() < end:
        pump(0.02)
        m = re.search(pat, buf[k])
        if m:
            out, buf[k] = buf[k][:m.start()], buf[k][m.end():]
            return out, m.group(1).decode()
    return buf[k], None
def stop():
    for p in (pa, pb): p.terminate()
    for p in (pa, pb):
        try: p.wait(2)
        except subprocess.TimeoutExpired: p.kill()
    time.sleep(0.2)
def fail(msg):
    print("FAIL:", msg); stop(); sys.exit(1)
def cmd(k, s, expect=b"(OK|ERROR)\r\n", t=5):
    os.write(fd[k], s.encode() + b"\r")
    out, r = wait(k, expect, t)
    print("%s: %-12s -> %s" % (k, s, r), flush=True)
    return out, r
RESULT = b"(?:\r\n)?(OK|CONNECT|NO CARRIER|ERROR|NO ANSWER|BUSY)\r\n"
def shield(data): return data.replace(b"\x10", b"\x10\x10") + b"\x10\x03"
def unshield(data):
    out, i = bytearray(), 0
    while i < len(data):
        c = data[i]
        if c == 0x10 and i + 1 < len(data):
            n = data[i + 1]; i += 2
            if n == 0x10: out.append(0x10)
            elif n == 0x03: return bytes(out), True
            continue
        out.append(c); i += 1
    return bytes(out), False
def send_frames(k, frames):
    """after CONNECT: send HDLC frames, the last one final"""
    for i, fr in enumerate(frames):
        os.write(fd[k], shield(fr))
        out, r = wait(k, RESULT, 10)
        want = "OK" if i == len(frames) - 1 else "CONNECT"
        print("%s: frame %s -> %s" % (k, fr[:3].hex(), r), flush=True)
        if r != want: fail("%s sending frame: %s %r" % (k, r, out))
def recv_frame(k, t=10):
    """after CONNECT: one received frame (with FCS) and the result"""
    end = time.time() + t
    while time.time() < end:
        pump(0.02)
        fr, ok = unshield(buf[k])
        if ok:
            j = 0
            while j < len(buf[k]):
                if buf[k][j] == 0x10:
                    if buf[k][j + 1:j + 2] == b"\x03": j += 2; break
                    j += 2; continue
                j += 1
            buf[k] = buf[k][j:]
            _, r = wait(k, RESULT, 5)
            print("%s: got frame %s (%d bytes) -> %s" % (k, fr[:3].hex(), len(fr), r), flush=True)
            return fr, r
    fail("%s: no frame, have %r" % (k, buf[k][:80]))
def recv_frames(k):
    """receive frames until the final one (control field 0x13); like
    fax software, retry +FRH after a false start"""
    frames, retries = [], 0
    while True:
        fr, r = recv_frame(k)
        if r == "OK":
            frames.append(fr[:-2])
            if len(fr) > 1 and fr[1] == 0x13: return frames
        elif retries < 3:
            retries += 1
        else:
            fail("%s: bad frame %r" % (k, fr))
        out, r = cmd(k, "AT+FRH=3", RESULT, 10)
        if r != "CONNECT": fail("%s FRH: %s" % (k, r))
def exchange(tx, rx, frames, tries=3):
    """tx sends HDLC frames to rx; like T.30, a command that got no
    answer is repeated"""
    os.write(fd[rx], b"AT+FRH=3\r")
    time.sleep(0.1)
    for attempt in range(tries):
        out, r = cmd(tx, "AT+FTH=3", RESULT, 10)
        if r != "CONNECT": fail("%s FTH: %s" % (tx, r))
        send_frames(tx, frames)
        got = []
        _, r = wait(rx, RESULT, 6)
        print("%s: AT+FRH=3 -> %s" % (rx, r), flush=True)
        while r == "CONNECT":
            fr, r = recv_frame(rx)
            if r != "OK": break
            got.append(fr[:-2])
            if fr[1:2] == b"\x13": break
            out, r = cmd(rx, "AT+FRH=3", RESULT, 6)
        if got == frames:
            return
        if r is not None:       # gave up on a false start: listen again
            os.write(fd[rx], b"AT+FRH=3\r")
        time.sleep(0.1)
    fail("%s did not get %r" % (rx, frames))
def bits(data):
    return [(c >> i) & 1 for c in data for i in range(8)]
def recv_data(k, t=60):
    end = time.time() + t
    while time.time() < end:
        pump(0.05)
        data, ok = unshield(buf[k])
        if ok:
            j = 0
            while j < len(buf[k]):
                if buf[k][j] == 0x10:
                    if buf[k][j + 1:j + 2] == b"\x03": j += 2; break
                    j += 2; continue
                j += 1
            buf[k] = buf[k][j:]
            _, r = wait(k, RESULT, 5)
            return data, r
    fail("%s: data did not end (%d bytes)" % (k, len(buf[k])))

V17 = mod in (73, 74, 97, 98, 121, 122, 145, 146)
long_mod = mod - 1 if mod in (74, 98, 122, 146) else mod
page_mod = long_mod + 1 if V17 else long_mod
bps = {24: 2400, 48: 4800, 72: 7200, 96: 9600, 73: 7200, 97: 9600,
       121: 12000, 145: 14400}[long_mod]

for k in fd:
    for c in ["ATZ", "AT+FCLASS=1"] + [x for x in setup.split(";") if x]:
        if cmd(k, c)[1] != "OK": fail("setup")
t0 = time.time()
os.write(fd["B"], b"ATA\r"); os.write(fd["A"], b"ATD\r")
_, r = wait("B", RESULT, 30)
print("B: ATA -> %s" % r)
if r != "CONNECT": fail("answer")
csi = b"\xff\x03\x40" + b"   4321 555 1+"[::-1].ljust(20)
dis = b"\xff\x13\x80\x00\xce\xf4\x80\x80\x81\x80\x80\x80\x18"
dcs = b"\xff\x13\x83\x00\xc6\x74"
send_frames("B", [csi, dis])
_, r = wait("A", RESULT, 30)
print("A: ATD -> %s" % r)
if r != "CONNECT": fail("dial")
got = recv_frames("A")
if got != [csi, dis]: fail("A got %r" % got)

# DCS + TCF
exchange("A", "B", [dcs])
os.write(fd["B"], b"AT+FRM=%d\r" % long_mod)
if cmd("A", "AT+FTS=8")[1] != "OK": fail("FTS")
_, r = cmd("A", "AT+FTM=%d" % long_mod, RESULT, 10)
if r != "CONNECT": fail("A FTM: %s" % r)
tcf = bytes(bps * 3 // 2 // 8)
os.write(fd["A"], shield(tcf))
_, r = wait("B", RESULT, 10)
print("B: AT+FRM=%d -> %s" % (long_mod, r))
if r != "CONNECT": fail("B FRM")
data, r = recv_data("B")
_, ra = wait("A", RESULT, 10)
# like a T.30 receiver: the longest run of zeros
run = 0
cur = 0
for c in data:
    cur = cur + 1 if c == 0 else 0
    run = max(run, cur)
print("A: TCF sent -> %s, B: TCF %d/%d bytes, %d zeros in a row -> %s"
      % (ra, len(data), len(tcf), run, r))
if os.environ.get("SHOW_TCF"):
    print("non-zero TCF bytes:", [(k, hex(c)) for k, c in enumerate(data) if c][:60])
if run < len(tcf) * 2 // 3: fail("TCF")

# CFR
exchange("B", "A", [b"\xff\x13\x84"])

# the page
random.seed(mod)
page = bytes(random.getrandbits(8) for _ in range(bps * 4 // 8))
os.write(fd["B"], b"AT+FRM=%d\r" % page_mod)
time.sleep(0.05)
if cmd("A", "AT+FTS=8")[1] != "OK": fail("FTS")
_, r = cmd("A", "AT+FTM=%d" % page_mod, RESULT, 10)
if r != "CONNECT": fail("A FTM page: %s" % r)
os.write(fd["A"], shield(page))
_, r = wait("B", RESULT, 10)
print("B: AT+FRM=%d -> %s" % (page_mod, r))
if r != "CONNECT": fail("B FRM page")
data, r = recv_data("B")
_, ra = wait("A", RESULT, 20)
if os.environ.get("RECORD"):
    open(os.path.join(os.environ["RECORD"], "page.rx"), "wb").write(data)
    open(os.path.join(os.environ["RECORD"], "page.tx"), "wb").write(page)
# the data is a bit stream: find the page at any bit offset
rb, pb_ = bits(data), bits(page)
i = next((k for k in range(min(len(rb) - 128, 4000))
          if rb[k:k + 128] == pb_[:128]), -1)
errs = sum(1 for x, y in zip(rb[i:], pb_) if x != y) if i >= 0 else -1
print("A: page sent -> %s, B: page %d/%d bytes at bit %d, %d bits wrong -> %s"
      % (ra, len(data), len(page), i, errs, r))
if i >= 0 and errs:
    pos = [k for k, (x, y) in enumerate(zip(rb[i:], pb_)) if x != y]
    print("wrong bits at", pos[:10], "...", pos[-3:], "of", len(pb_))
# the end of the page may be cut short by the carrier going off (a real
# page ends with RTC), and a page without ECM survives a few bit errors
# (the original library makes some even with itself)
tail = 64
page_ok = (i >= 0 and len(rb) - i >= len(pb_) - tail and
           sum(x != y for x, y in zip(rb[i:], pb_[:-tail])) <= len(pb_) // 1000)

# EOP, MCF, DCN
exchange("A", "B", [b"\xff\x13\x2f"])
exchange("B", "A", [b"\xff\x13\x8c"])
if cmd("A", "AT+FTH=3", RESULT, 10)[1] != "CONNECT": fail("A FTH DCN")
send_frames("A", [b"\xff\x13\xfb"])
for k in fd: cmd(k, "ATH")
print("call took %.1f s" % (time.time() - t0))
stop()
print("PASS" if page_ok else "FAIL: page")
sys.exit(0 if page_ok else 1)
