#!/usr/bin/env python3
#
# interop.py - connect two modem_half processes back to back and make a
# call between them, e.g. one linked with the original dsplibs.o and one
# with the open data pumps:
#
#     interop.py <caller exe> <answerer exe> ["ATX3;AT+MS=122"]
#
# STREAM=<s> keeps data flowing for that long (see netsim's NET_DROP and
# NET_STEP for line hits in the middle); MIDCALL_A=<s>:<cmd>;... gives
# modemA AT commands that far into it (e.g. V.92 modem-on-hold).
# Both modems get the ';' separated AT commands (PROTO_B=... gives modemB
# its own, e.g. an older modulation to answer with), then modemB answers,
# modemA dials, data is exchanged both ways and checked.  The modem logs
# are written next to this script (modemA.log, modemB.log).
import os, re, select, subprocess, sys, time, tty, threading
exe_a, exe_b = sys.argv[1], sys.argv[2]
# the modems take the DTE's commands at fixed samples of the line
# (PIPE_SYNC, AGERE_SYNC; 0: as they come), so that a call meets the
# other modem at the same alignment every run
os.environ.setdefault("PIPE_SYNC", "4800")
os.environ.setdefault("AGERE_SYNC", os.environ["PIPE_SYNC"])
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
    # WAV=<file>: the line as it crosses the network (stereo, 8000 Hz:
    # A -> B left, B -> A right), from the first dial to the end of the
    # data (netsim's NET_WAV, started and ended with SIGUSR2)
    if os.environ.get("WAV"):
        os.environ["NET_WAV"] = os.path.abspath(os.environ["WAV"])
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
def wait_from(k, pat, mark, t):
    end = time.time() + t
    while time.time() < end:
        pump(0.1)
        if re.search(pat, buf[k][mark:]): return True
    return False
wav_marks = 0
def wav_mark():
    global wav_marks
    if net and os.environ.get("WAV") and wav_marks < 2:
        import signal
        pn.send_signal(signal.SIGUSR2)
        wav_marks += 1
def done(ok):
    if wav_marks == 1: wav_mark()   # (a call that failed: up to here)
    pa.kill(); pb.kill()
    # EXPECT_B=<regex>: modemB's log must have it as well (e.g. that the
    # call took a certain path)
    if ok and os.environ.get("EXPECT_B"):
        time.sleep(0.5)
        log_b = open(os.path.join(os.environ.get("LOG_DIR", d), "modemB.log"), "rb").read()
        if not re.search(os.environ["EXPECT_B"].encode(), log_b):
            print("modemB's log has no", os.environ["EXPECT_B"]); ok = False
    print("PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)
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
# CALLS=<n>: that many calls in a row, the data checked on the last one
# (the modems may remember the line from the ones before, V.92's quick
# connect); DIAL=<number> to dial
for call in range(int(os.environ.get("CALLS", "1"))):
    if call:
        # hang up: escape both (their OK, after the guard time), ATH and
        # its OK, then wait for the line to clear
        time.sleep(1.5)
        for k in fd: buf[k] = b""; os.write(fd[k], b"+++")
        for k in fd: wait(k, b"OK", 5)
        for k in fd:
            buf[k] = b""
            cmd(k, "ATH")
            if not wait(k, b"OK", 10): print(k, "ATH", buf[k]); done(False)
        time.sleep(2)
        # RESTART_B=1: modemB a new process for the next call
        if os.environ.get("RESTART_B"):
            pb.kill(); os.close(fd["modemB"])
            pb = start(exe_b, "modemB2", ab_r, ba_w)
            while True:
                m = re.search(r"created (\S+): (\S+)", pb.stdout.readline().decode(errors="replace"))
                if m: break
            lf = open(os.path.join(os.environ.get("LOG_DIR", d), "modemB2.log"), "wb")
            threading.Thread(target=lambda p=pb, lf=lf: [(lf.write(l), lf.flush()) for l in p.stdout], daemon=True).start()
            fd["modemB"] = os.open(m.group(2), os.O_RDWR | os.O_NOCTTY); tty.setraw(fd["modemB"])
            buf["modemB"] = b""
            for c in ["ATZ"] + [x for x in os.environ.get("PROTO_B", proto).split(";") if x]:
                cmd("modemB", c)
                if not wait("modemB", b"OK", 5): print("modemB", c, buf["modemB"]); done(False)
                buf["modemB"] = b""
        buf = {k: b"" for k in fd}
    if not call: wav_mark()
    cmd("modemB", "ATA"); cmd("modemA", "ATD" + os.environ.get("DIAL", ""))
    t_call = time.time()
    ok = wait("modemA", b"(CONNECT[^\r]*|NO CARRIER|NO ANSWER|BUSY)\r", 90) and wait("modemB", b"(CONNECT[^\r]*|NO CARRIER)\r", 15)
    print("A:", buf["modemA"]); print("B:", buf["modemB"])
    print("call %d: %.1f s to CONNECT" % (call + 1, time.time() - t_call))
    if not (b"CONNECT" in buf["modemA"] and b"CONNECT" in buf["modemB"]): done(False)
# NET_SYNC: netsim's NET_DROP, NET_STEP and NET_NOISE times count from here
if net and os.environ.get("NET_SYNC"):
    import signal
    pn.send_signal(signal.SIGUSR1)
# the slowest direction sets how long the data takes (V.23: 75 bit/s)
rate = min([int(r) for r in re.findall(rb"CONNECT (\d+)", buf["modemA"] + buf["modemB"])] or [1200])
buf = {k: b"" for k in fd}
msg_a = bytes(range(32, 127)) * 4 + b"\r\n"
msg_b = b"The quick brown fox jumps over the lazy dog. " * 8
# BULK=<octets>: that much each way instead, text, binary octets, long
# repeats and noise in turn (what a compressor has to get right: its
# dictionary filling up, 8 bit characters, string extensions, and the
# Agere modem's switch to transparent mode on what does not compress);
# no XON/XOFF octets
def bulk(n, seed):
    import random
    r = random.Random(seed)
    words = [b"modem", b"line", b"the", b"of", b"compression", b"V.44",
             b"dictionary", b"history", b"string", b"codeword", b"data"]
    out = b""
    while len(out) < n:
        kind = len(out) // 2048 % 4
        if kind == 0:
            out += b" ".join(r.choice(words) for _ in range(300)) + b"\r\n"
        elif kind == 1:
            out += bytes(r.randrange(128, 256) for _ in range(1024))
        elif kind == 2:
            out += bytes(r.choice(b"abc") for _ in range(16)) * 64
        else:
            out += bytes(r.randrange(256) for _ in range(2048))
    return bytes(c for c in out if c not in (0x11, 0x13))[:n]
bulk_n = int(os.environ.get("BULK", "0"))
# STREAM=<s>: keep the data going both ways for that long (a line hit,
# NET_DROP or NET_STEP, can come in the middle), then check all of it
stream = float(os.environ.get("STREAM", "0"))
if stream:
    sent_a = sent_b = b""
    t_end = time.time() + stream
    n = 0
    # MIDCALL_A=<s>:<cmd>;...: after s seconds, escape modemA, give it
    # the commands (WAIT <s> sleeps), print what it says, then ATO
    mid = os.environ.get("MIDCALL_A")
    mid_at = time.time() + float(mid.split(":", 1)[0]) if mid else None
    while time.time() < t_end:
        if mid_at and time.time() >= mid_at:
            mid_at = None
            pump(1.2)
            sent_b_len = len(buf["modemA"])
            os.write(fd["modemA"], b"+++")
            sent_a += b"+++"    # the remote gets it too
            wait_from("modemA", b"OK\r", sent_b_len, 5)
            for c in mid.split(":", 1)[1].split(";"):
                if c.startswith("WAIT "):
                    pump(float(c[5:]))
                    continue
                mark = len(buf["modemA"])
                cmd("modemA", c)
                wait_from("modemA", b"(OK|ERROR|CONNECT[^\r]*|NO CARRIER)\r", mark, 30)
                print("midcall A:", c, buf["modemA"][mark:])
            mark = len(buf["modemA"])
            cmd("modemA", "ATO")
            wait_from("modemA", b"(CONNECT[^\r]*|NO CARRIER)\r", mark, 60)
            print("midcall A: ATO", buf["modemA"][mark:])
            # what the commands put in modemA's buffer is not data
            buf["modemA"] = buf["modemA"][:sent_b_len]
        line = b"%06d " % n
        os.write(fd["modemA"], line + msg_a); sent_a += line + msg_a
        os.write(fd["modemB"], line + msg_b); sent_b += line + msg_b
        n += 1
        pump(0.5)
    msg_a, msg_b = sent_a, sent_b
elif bulk_n:
    msg_a, msg_b = bulk(bulk_n, 1), bulk(bulk_n, 2)
    todo = {"modemA": msg_a, "modemB": msg_b}
    for f in fd.values(): os.set_blocking(f, False)
    while any(todo.values()):
        for k in fd:
            if todo[k]:
                try:
                    w = os.write(fd[k], todo[k][:1024])
                    todo[k] = todo[k][w:]
                except BlockingIOError:
                    pass
        pump(0.05)
else:
    os.write(fd["modemA"], msg_a); os.write(fd["modemB"], msg_b)
# 10 bits a character, and half as much again for V.42 framing
end = time.time() + 40 + stream + 15.0 * max(len(msg_a), len(msg_b)) / rate
while time.time() < end and (len(buf["modemB"]) < len(msg_a) or len(buf["modemA"]) < len(msg_b)):
    pump(0.2)
wav_mark()
if net and os.environ.get("WAV"):
    print("wav:", os.environ["NET_WAV"])
print("A received %d/%d ok=%s" % (len(buf["modemA"]), len(msg_b), buf["modemA"] == msg_b))
print("B received %d/%d ok=%s" % (len(buf["modemB"]), len(msg_a), buf["modemB"] == msg_a))
done(buf["modemA"] == msg_b and buf["modemB"] == msg_a)
