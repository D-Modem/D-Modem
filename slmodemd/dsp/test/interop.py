#!/usr/bin/env python3
#
# interop.py - connect two modem_half processes back to back and make a
# call between them, e.g. one linked with the original dsplibs.o and one
# with the open data pumps:
#
#     interop.py <caller exe> <answerer exe> ["ATX3;AT+MS=122"]
#
# Both modems get the ';' separated AT commands, then modemB answers,
# modemA dials, data is exchanged both ways and checked.  The modem logs
# are written next to this script (modemA.log, modemB.log).
import os, re, select, subprocess, sys, time, tty, threading
exe_a, exe_b = sys.argv[1], sys.argv[2]
proto = sys.argv[3] if len(sys.argv) > 3 else ""
d = os.path.dirname(os.path.abspath(__file__))
ab_r, ab_w = os.pipe()   # A -> B
ba_r, ba_w = os.pipe()   # B -> A
def start(exe, name, rfd, wfd):
    def fds():
        a, b = os.dup(rfd), os.dup(wfd)
        os.dup2(a, 3); os.dup2(b, 4)
    env = dict(os.environ, MODEM_NAME=name)
    return subprocess.Popen([exe, "--debug=" + os.environ.get("DBG", "3"), "-e"], stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, preexec_fn=fds, close_fds=False, env=env)
pa = start(exe_a, "modemA", ba_r, ab_w)
pb = start(exe_b, "modemB", ab_r, ba_w)
ptys = {}
for k, p in (("modemA", pa), ("modemB", pb)):
    t0 = time.time()
    while time.time() - t0 < 10:
        line = p.stdout.readline().decode(errors="replace")
        m = re.search(r"created (\S+): (\S+)", line)
        if m:
            ptys[k] = m.group(2); break
    lf = open(os.path.join(d, k + ".log"), "wb")
    threading.Thread(target=lambda p=p, lf=lf: [(lf.write(l), lf.flush()) for l in p.stdout], daemon=True).start()
fd = {k: os.open(v, os.O_RDWR | os.O_NOCTTY) for k, v in ptys.items()}
for f in fd.values(): tty.setraw(f)
buf = {k: b"" for k in fd}
def pump(t):
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select(list(fd.values()), [], [], 0.1)
        for k, f in fd.items():
            if f in r: buf[k] += os.read(f, 4096)
def wait(k, pat, t):
    end = time.time() + t
    while time.time() < end:
        pump(0.1)
        if re.search(pat, buf[k]): return True
    return False
def cmd(k, s): os.write(fd[k], s.encode() + b"\r")
def done(ok):
    pa.kill(); pb.kill(); print("PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)
for k in fd: cmd(k, "ATZ")
for k in fd:
    if not wait(k, b"OK", 5): print(k, buf[k]); done(False)
    buf[k] = b""
for k in fd:
    for c in [x for x in proto.split(";") if x]:
        cmd(k, c)
        if not wait(k, b"OK|ERROR", 5) or b"ERROR" in buf[k]: print(k, c, buf[k]); done(False)
        buf[k] = b""
cmd("modemB", "ATA"); cmd("modemA", "ATD")
ok = wait("modemA", b"(CONNECT[^\r]*|NO CARRIER|NO ANSWER|BUSY)\r", 90) and wait("modemB", b"(CONNECT[^\r]*|NO CARRIER)\r", 15)
print("A:", buf["modemA"]); print("B:", buf["modemB"])
if not (b"CONNECT" in buf["modemA"] and b"CONNECT" in buf["modemB"]): done(False)
buf = {k: b"" for k in fd}
msg_a = bytes(range(32, 127)) * 4 + b"\r\n"
msg_b = b"The quick brown fox jumps over the lazy dog. " * 8
os.write(fd["modemA"], msg_a); os.write(fd["modemB"], msg_b)
end = time.time() + 40
while time.time() < end and (len(buf["modemB"]) < len(msg_a) or len(buf["modemA"]) < len(msg_b)):
    pump(0.2)
print("A received %d/%d ok=%s" % (len(buf["modemA"]), len(msg_b), buf["modemA"] == msg_b))
print("B received %d/%d ok=%s" % (len(buf["modemB"]), len(msg_a), buf["modemB"] == msg_a))
done(buf["modemA"] == msg_b and buf["modemB"] == msg_a)
