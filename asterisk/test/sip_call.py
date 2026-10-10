#!/usr/bin/env python3
#
# sip_call.py <slmodemd> <d-modem> <number> [AT commands]: D-Modem calls
# <number> over SIP ($SIP_LOGIN) and plays the user of bbs_sim.py's BBS,
# which this script serves on its TCP port as well.
import os, re, socket, subprocess, sys, threading, time, tty

slmodemd, dmodem, number = sys.argv[1:4]
at = sys.argv[4] if len(sys.argv) > 4 else ""
LINES = [b"hello from the caller %d " % i + b"x" * 40 for i in range(20)]

# the BBS
def bbs_conn(conn):
    print("bbs: connected at %.3f" % time.time(), flush=True)
    conn.sendall(b"Welcome to the test BBS\r\n")
    buf = b""
    while True:
        d = conn.recv(4096)
        if not d:
            print("bbs: the modem side closed at %.3f" % time.time(), flush=True)
            return
        buf += d
        print("bbs: %d bytes at %.3f: %r" % (len(d), time.time(), d[:40]), flush=True)
        while b"\r\n" in buf:
            line, buf = buf.split(b"\r\n", 1)
            if line == b"BYE":
                print("bbs: BYE at %.3f" % time.time(), flush=True)
                conn.sendall(b"Goodbye\r\n")
                time.sleep(0.5)
                conn.close()
                return
            conn.sendall(line.upper() + b"\r\n")
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 2323)); srv.listen(1)
threading.Thread(target=lambda: bbs_conn(srv.accept()[0]), daemon=True).start()

p = subprocess.Popen(["stdbuf", "-oL", "-eL", slmodemd, "-d" + os.environ.get("SLMODEMD_DEBUG", "1"), "-e", dmodem], stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT)
log = open(os.environ.get("SIP_LOG", "/tmp/sip_call.log"), "wb")
pty = None
t0 = time.time()
while time.time() - t0 < 10:
    l = p.stdout.readline()
    log.write(l); log.flush()
    m = re.search(rb"TTY is `(\S+)'", l)
    if m:
        pty = m.group(1).decode(); break
threading.Thread(target=lambda: [log.write(l) or log.flush() for l in p.stdout],
                 daemon=True).start()
fd = os.open(pty, os.O_RDWR | os.O_NOCTTY)
tty.setraw(fd)
buf = b""
def expect(pat, t):
    global buf
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, buf)
        if m:
            buf = buf[m.end():]
            return m
        try:
            os.set_blocking(fd, False)
            buf += os.read(fd, 4096)
        except BlockingIOError:
            time.sleep(0.05)
    return None
def cmd(c, pat=rb"OK\r\n", t=5):
    os.write(fd, c + b"\r")
    return expect(pat, t)

ok = True
for c in [b"ATZ", b"ATX3"] + [x.encode() for x in at.split(";") if x]:
    ok = ok and cmd(c) is not None
m = cmd(b"ATD" + number.encode(), rb"(CONNECT[^\r]*|NO CARRIER|BUSY|NO ANSWER|ERROR)\r\n", 90)
print("dial:", m.group(1).decode() if m else "timeout")
ok = ok and m is not None and m.group(1).startswith(b"CONNECT")
if ok:
    t1 = time.time()
    # the timeouts are for 2400 bit/s and up
    rate = re.match(rb"CONNECT (\d+)", m.group(1))
    slow = max(1, 2400 // int(rate.group(1))) if rate else 1
    ok = expect(rb"Welcome to the test BBS\r\n", 30 * slow) is not None
    for l in LINES:
        os.write(fd, l + b"\r\n")
    for l in LINES:
        ok = ok and expect(re.escape(l.upper() + b"\r\n"), 60 * slow) is not None
    os.write(fd, b"BYE\r\n")
    ok = ok and expect(rb"Goodbye\r\n", 30 * slow) is not None
    print("session: %s in %.1f s" % ("PASS" if ok else "FAIL", time.time() - t1))
    print("hangup:", "NO CARRIER" if expect(rb"NO CARRIER", 30) else "none")
p.terminate()
if not ok:
    print("modem said:", buf[-300:])
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
