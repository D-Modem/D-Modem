#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
# sip_call.py <dmodemd> <number> [AT commands]: D-Modem (dmodemd) calls
# <number> over SIP ($SIP_LOGIN) and plays the user of bbs_sim.py's BBS,
# which this script serves on its TCP port as well.
#
# With SIP_IN=<command>, the call goes the other way: D-Modem listens
# (dmodemd -L, ATS0=1), <command> makes Asterisk call it, and DModem(term,o)
# as the calling modem connects to this script's terminal port (2324), the
# user; the BBS is on D-Modem's pty.  CALLS=<n>: that many calls one after
# the other (D-Modem calling), in one dmodemd.  BBS_CONNECT_DELAY=<s>: the
# BBS takes no connection until s seconds after the start (as
# bbs_sim.py's), so DModem(bbs)'s connect waits meanwhile.  DIAL=<command>:
# the dial command before the number (ATD; ATDT: the call goes to the
# number all the same).  HOLD=1: the number dialed with a ';' (the SIP
# call made, OK, the modem in command state), then ATD alone starts the
# handshake.  VOICE=1 (with SIP_IN): D-Modem takes the call in voice
# mode (RING, AT+FCLASS=8, AT+VLS=1), plays DTMF digits and hook flashes
# with AT+VTS and leaves the line; the call's dialplan sends it a DTMF
# digit meanwhile.  Its log must show the flashes sent as RFC 4733
# events and the digit received as one (sip.sh checks Asterisk's side).
import os, re, socket, subprocess, sys, threading, time, tty

dmodemd, number = sys.argv[1:3]
at = sys.argv[3] if len(sys.argv) > 3 else ""
LINES = [b"hello from the caller %d " % i + b"x" * 40 for i in range(20)]

# the BBS
# one connection: the banner, each line back in capitals, BYE ends it
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
srv.bind(("127.0.0.1", 2323))
calls = int(os.environ.get("CALLS", "1"))
delay = float(os.environ.get("BBS_CONNECT_DELAY", "0"))
srv.listen(0 if delay else 1)
if delay:
    # the accept queue full with a connection of our own: SYNs dropped,
    # connect() retrying, until it is taken
    filler = socket.create_connection(("127.0.0.1", 2323))
    def take_filler():
        time.sleep(delay)
        srv.accept()[0].close()
        filler.close()
        print("bbs: taking connections at %.1f s" % delay, flush=True)
    threading.Thread(target=take_filler, daemon=True).start()
    time.sleep(0.1)
def bbs_calls():
    if delay:
        time.sleep(delay + 0.05)
    for _ in range(calls):
        bbs_conn(srv.accept()[0])
threading.Thread(target=bbs_calls, daemon=True).start()

incoming = os.environ.get("SIP_IN")
p = subprocess.Popen(["stdbuf", "-oL", "-eL", dmodemd, "-d" + os.environ.get("DMODEMD_DEBUG", "1")] + (["-L"] if incoming else []), stdout=subprocess.PIPE,
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
# the rest of dmodemd's output into the log
threading.Thread(target=lambda: [log.write(l) or log.flush() for l in p.stdout],
                 daemon=True).start()
if pty is None:
    print("FAIL: dmodemd gave no pty")
    p.terminate()
    sys.exit(1)
fd = os.open(pty, os.O_RDWR | os.O_NOCTTY)
tty.setraw(fd)
buf = b""
# the first match of pat in what the modem sent (read from the pty), buf
# kept past it; None after t s
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
        except OSError:
            return None     # dmodemd went away (EIO)
    return None
# AT command c, then expect(pat, t)
def cmd(c, pat=rb"OK\r\n", t=5):
    os.write(fd, c + b"\r")
    return expect(pat, t)

ok = True
for c in [b"ATZ", b"ATX3"] + [x.encode() for x in at.split(";") if x]:
    ok = ok and cmd(c) is not None
if incoming and os.environ.get("VOICE"):
    time.sleep(2)               # dmodemd registers
    subprocess.Popen(incoming, shell=True)
    ok = expect(rb"RING\r\n", 30) is not None
    print("ring:", "RING" if ok else "none")
    for c in [b"AT+FCLASS=8", b"AT+VLS=1", b"AT+VTD=10"]:
        ok = ok and cmd(c) is not None
    time.sleep(2)               # Asterisk's digit comes meanwhile
    m = ok and cmd(b"AT+VTS=1,!,{!,30},2", rb"(OK|ERROR)\r\n", 10)
    print("tones:", m.group(1).decode() if m else "timeout")
    ok = ok and m is not None and m.group(1) == b"OK"
    time.sleep(1)
    ok = ok and cmd(b"AT+VLS=0") is not None
    time.sleep(0.5)
    p.terminate()
    p.wait(10)
    time.sleep(0.2)
    text = open(os.environ.get("SIP_LOG", "/tmp/sip_call.log"), "rb").read()
    for want in [rb"hook flash, 100 ms \(RFC 4733\)",
                 rb"hook flash, 300 ms \(RFC 4733\)",
                 rb"DTMF 5 \(RFC 4733\), played as its tone"]:
        found = re.search(want, text) is not None
        print("log: %s: %s" % (want.decode().replace("\\", ""),
                               "yes" if found else "no"))
        ok = ok and found
    if not ok:
        print("modem said:", buf[-300:])
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)
if incoming:
    # the user, on the terminal port DModem(term,o) connects to
    term = socket.socket()
    term.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    term.bind(("127.0.0.1", 2324)); term.listen(1); term.settimeout(120)
    user_ok = {}
    # the user on the terminal port: the banner, LINES and their echoes,
    # BYE and Goodbye; the result into user_ok
    def user():
        try:
            c, _ = term.accept()
        except OSError:
            return
        c.settimeout(60)
        b = b""
        # reads until pat is in b, b kept past it; False if the
        # connection closes
        def want(pat):
            nonlocal b
            while not re.search(pat, b):
                d = c.recv(4096)
                if not d:
                    return False
                b += d
            b = b[re.search(pat, b).end():]
            return True
        try:
            r = want(rb"Welcome to the test BBS\r\n")
            for l in LINES:
                c.sendall(l + b"\r\n")
            for l in LINES:
                r = r and want(re.escape(l.upper() + b"\r\n"))
            c.sendall(b"BYE\r\n")
            r = r and want(rb"Goodbye\r\n")
        except OSError:
            r = False
        user_ok["ok"] = r
        c.close()
    ut = threading.Thread(target=user, daemon=True); ut.start()
    ok = ok and cmd(b"ATS0=1") is not None
    time.sleep(2)               # dmodemd registers
    subprocess.Popen(incoming, shell=True)
    m = expect(rb"(CONNECT[^\r]*|NO CARRIER)\r\n", 90)
    print("answer:", m.group(1).decode() if m else "timeout")
    if m and m.group(1).startswith(b"CONNECT"):
        # the BBS on the pty
        t1 = time.time()
        # the calling modem may come online after us (FSK: a second
        # later); a greeting sent before would be lost
        time.sleep(1.5)
        os.write(fd, b"Welcome to the test BBS\r\n")
        while True:
            l = expect(rb"([^\r\n]*)\r\n", 120)
            if l is None or l.group(1) == b"BYE":
                break
            if l.group(1):
                os.write(fd, l.group(1).upper() + b"\r\n")
        os.write(fd, b"Goodbye\r\n")
        ut.join(60)
        ok = user_ok.get("ok", False)
        print("session: %s in %.1f s" % ("PASS" if ok else "FAIL", time.time() - t1))
        print("hangup:", "NO CARRIER" if expect(rb"NO CARRIER", 30) else "none")
    else:
        ok = False
    p.terminate()
    if not ok:
        print("modem said:", buf[-300:])
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)
for call in range(calls):
    if not ok:
        break
    if calls > 1:
        print("call %d:" % (call + 1))
    dial = os.environ.get("DIAL", "ATD").encode() + number.encode()
    if os.environ.get("HOLD"):
        m = cmd(dial + b";", rb"(OK|NO CARRIER|ERROR)\r\n", 10)
        print("dial ;:", m.group(1).decode() if m else "timeout")
        ok = ok and m is not None and m.group(1) == b"OK"
        dial = b"ATD"
    m = cmd(dial, rb"(CONNECT[^\r]*|NO CARRIER|BUSY|NO ANSWER|ERROR)\r\n", 90)
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
        hung = expect(rb"NO CARRIER", 30)
        print("hangup:", "NO CARRIER" if hung else "none")
        ok = ok and (hung is not None or calls == 1)
p.terminate()
if not ok:
    print("modem said:", buf[-300:])
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
