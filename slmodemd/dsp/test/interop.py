#!/usr/bin/env python3
#
# interop.py - connect two modem_half processes back to back and make a
# call between them, e.g. one linked with the original dsplibs.o and one
# with the open data pumps:
#
#     interop.py <caller exe> <answerer exe> ["ATX3;AT+MS=122"]
#
# STREAM=<s> keeps data flowing for that long (see netsim's NET_DROP and
# NET_STEP for line hits in the middle).
# Both modems get the ';' separated AT commands (PROTO_B=... gives modemB
# its own, e.g. an older modulation to answer with), then modemB answers,
# modemA dials, data is exchanged both ways and checked.  The modem logs
# are written next to this script (modemA.log, modemB.log).
import os, re, select, subprocess, sys, time, tty, threading
exe_a, exe_b = sys.argv[1], sys.argv[2]
proto = sys.argv[3] if len(sys.argv) > 3 else ""
d = os.path.dirname(os.path.abspath(__file__))
ab_r, ab_w = os.pipe()   # A -> B
ba_r, ba_w = os.pipe()   # B -> A
# NET=<rate A>:<rate B>:<ulaw|alaw> puts the G.711 network in between
# (netsim), e.g. NET=8000:9600:ulaw for an open modem at 8000 Hz calling
# the original library at 9600 Hz
net = os.environ.get("NET")
if net:
    ra, rb, law = net.split(":")
    an_r, an_w = os.pipe()   # A -> network
    na_r, na_w = os.pipe()   # network -> A
    bn_r, bn_w = os.pipe()   # B -> network
    nb_r, nb_w = os.pipe()   # network -> B
    pn = subprocess.Popen([os.environ.get("NETSIM", os.path.join(d, "netsim")), ra, rb, law, str(an_r), str(na_w),
        str(bn_r), str(nb_w)], pass_fds=(an_r, na_w, bn_r, nb_w))
    ba_r, ab_w, ab_r, ba_w = na_r, an_w, nb_r, bn_w
def start(exe, name, rfd, wfd):
    def fds():
        a, b = os.dup(rfd), os.dup(wfd)
        os.dup2(a, 3); os.dup2(b, 4)
    env = dict(os.environ, MODEM_NAME=name)
    return subprocess.Popen([exe, "--debug=" + os.environ.get("DBG", "3")], stdout=subprocess.PIPE,
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
    os.makedirs(os.environ.get("LOG_DIR", d), exist_ok=True)
    lf = open(os.path.join(os.environ.get("LOG_DIR", d), k + ".log"), "wb")
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
    pr = os.environ.get("PROTO_B", proto) if k == "modemB" else proto
    for c in [x for x in pr.split(";") if x]:
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
# STREAM=<s>: keep the data going both ways for that long (a line hit,
# NET_DROP or NET_STEP, can come in the middle), then check all of it
stream = float(os.environ.get("STREAM", "0"))
if stream:
    sent_a = sent_b = b""
    t_end = time.time() + stream
    n = 0
    while time.time() < t_end:
        line = b"%06d " % n
        os.write(fd["modemA"], line + msg_a); sent_a += line + msg_a
        os.write(fd["modemB"], line + msg_b); sent_b += line + msg_b
        n += 1
        pump(0.5)
    msg_a, msg_b = sent_a, sent_b
else:
    os.write(fd["modemA"], msg_a); os.write(fd["modemB"], msg_b)
end = time.time() + 40 + stream
while time.time() < end and (len(buf["modemB"]) < len(msg_a) or len(buf["modemA"]) < len(msg_b)):
    pump(0.2)
print("A received %d/%d ok=%s" % (len(buf["modemA"]), len(msg_b), buf["modemA"] == msg_b))
print("B received %d/%d ok=%s" % (len(buf["modemB"]), len(msg_a), buf["modemB"] == msg_a))
done(buf["modemA"] == msg_b and buf["modemB"] == msg_a)
