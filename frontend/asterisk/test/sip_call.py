#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
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
# events and the digit received as one, and the voice engine must report
# that digit to the DTE (<DLE>5 ... <DLE>~); sip.sh checks Asterisk's
# side.  VOICE_DIAL=1: D-Modem dials in voice mode (ATD with +FCLASS=8,
# S7=6): 7002 BUSY, 7003 NO ANSWER after S7, 7001 OK when Asterisk
# answers (2 s of ringing), its DTMF 7 reported, our +VTS played, and its
# hangup reported as <DLE>l alone; then with early media (S7=15) 7004's
# ringing tone reported as rings (<DLE>r) and OK at its 200 OK, 7005's
# busy tone BUSY before its 486, 7007's answer tone <DLE>a and OK
# without a 200 OK; 7006 (404) ERROR; ATH then data mode again, where a
# data call to 7002 is BUSY too.  A Windows dmodemd (<dmodemd> a .exe)
# runs under Wine, its DTE a TCP connection (--tcp) instead of the pty;
# WIN_COM=1: a COM port (--com COM9), Wine's COM9 a pty pair's far end;
# WIN_PIPE=made (--pipe) or connect (--pipe-connect): a named pipe,
# which pipe_test.exe's bridge (beside dmodemd.exe) gives us on TCP;
# WIN_DRIVER=1: the virtual modem driver's port (--driver SIPTEST), the
# driver's end of its pipe run by vmodem_test.exe's bridge, which gives
# us the port on TCP and its DCD, on during the call and off after.
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
windows = dmodemd.endswith(".exe")
# Windows: the DTE on TCP, or with WIN_COM=1 on a COM port, Wine's COM9
# made the far end of a pty pair whose near end is ours
com = None
if windows and os.environ.get("WIN_COM"):
    com, slave = os.openpty()
    tty.setraw(slave)
    link = os.path.join(os.environ.get("WINEPREFIX", os.path.expanduser("~/.wine")),
                        "dosdevices", "com9")
    if os.path.lexists(link):
        os.unlink(link)
    os.symlink(os.ttyname(slave), link)
    dte_args = ["--com", "COM9"]
elif windows and os.environ.get("WIN_DRIVER"):
    dte_args = ["--driver", "SIPTEST"]
elif windows and os.environ.get("WIN_PIPE"):
    pipe_mode = "--pipe-connect" if os.environ["WIN_PIPE"] == "connect" else "--pipe"
    pipe_name = r"\\.\pipe\dmodem-sip-test"
    dte_args = [pipe_mode, pipe_name]
else:
    dte_args = ["--tcp", "127.0.0.1:2399"]
argv = ["stdbuf", "-oL", "-eL"] + (["wine", dmodemd] + dte_args if windows else [dmodemd])
p = subprocess.Popen(argv + ["-d" + os.environ.get("DMODEMD_DEBUG", "1")] + (["-L"] if incoming else []), stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT)
log = open(os.environ.get("SIP_LOG", "/tmp/sip_call.log"), "wb")
pty = None
tcp = None
com_up = False
dcd = []
t0 = time.time()
while time.time() - t0 < (60 if windows else 10):
    l = p.stdout.readline()
    log.write(l); log.flush()
    m = re.search(rb"TTY is `(\S+)'", l)
    if m:
        pty = m.group(1).decode(); break
    m = re.search(rb"DTE is TCP (\S+):(\d+)", l)
    if m:
        tcp = (m.group(1).decode(), int(m.group(2))); break
    if com is not None and re.search(rb"DTE is COM9", l):
        com_up = True; break
    if re.search(rb"DTE is the virtual modem driver's port", l):
        # the driver's end: the port to us on TCP 2398, its DCD noted
        bridge = subprocess.Popen(["wine", os.path.join(os.path.dirname(dmodemd), "vmodem_test.exe"),
                                   "bridge", "SIPTEST", "2398"],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for b in bridge.stdout:
            log.write(b"vmodem_test: " + b); log.flush()
            if b"listening" in b:
                tcp = ("127.0.0.1", 2398); break
        def bridge_log():
            for b in bridge.stdout:
                log.write(b"vmodem_test: " + b); log.flush()
                m = re.search(rb"bridge: dcd (on|off)", b)
                if m:
                    dcd.append(m.group(1).decode())
        threading.Thread(target=bridge_log, daemon=True).start()
        break
    if re.search(rb"DTE is pipe ", l):
        # the bridge: the pipe to us on TCP 2398
        bridge = subprocess.Popen(["wine", os.path.join(os.path.dirname(dmodemd), "pipe_test.exe"),
                                   "bridge", pipe_mode, pipe_name, "2398"],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for b in bridge.stdout:
            log.write(b"pipe_test: " + b); log.flush()
            if b"listening" in b:
                tcp = ("127.0.0.1", 2398); break
        threading.Thread(target=lambda: [log.write(b"pipe_test: " + b) for b in bridge.stdout],
                         daemon=True).start()
        break
# the rest of dmodemd's output into the log
def copy_log():
    for l in p.stdout:
        log.write(l)
        log.flush()
threading.Thread(target=copy_log, daemon=True).start()
if pty is None and tcp is None and not com_up:
    print("FAIL: dmodemd gave no pty")
    p.terminate()
    sys.exit(1)
# the DTE's end: the pty, or the TCP connection
if tcp:
    dte = socket.create_connection(tcp)
    dte.setblocking(False)
else:
    fd = com if com is not None else os.open(pty, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    os.set_blocking(fd, False)
# what the modem sent: BlockingIOError if nothing yet, OSError when it's
# gone
def dte_read():
    if tcp:
        d = dte.recv(4096)
        if not d:
            raise OSError("closed")
        return d
    return os.read(fd, 4096)
def dte_write(b):
    if tcp:
        dte.setblocking(True)
        dte.sendall(b)
        dte.setblocking(False)
    else:
        os.write(fd, b)
buf = b""
seen = b""                  # all the modem sent, for the voice checks
# the first match of pat in what the modem sent (read from the pty), buf
# kept past it; None after t s
def expect(pat, t):
    global buf, seen
    end = time.time() + t
    while time.time() < end:
        m = re.search(pat, buf)
        if m:
            buf = buf[m.end():]
            return m
        try:
            got = dte_read()
            buf += got
            seen += got
        except BlockingIOError:
            time.sleep(0.05)
        except OSError:
            return None     # dmodemd went away (EIO)
    return None
# AT command c, then expect(pat, t)
def cmd(c, pat=rb"OK\r\n", t=5):
    dte_write(c + b"\r")
    return expect(pat, t)

ok = True
for c in [b"ATZ", b"ATX3"] + [x.encode() for x in at.split(";") if x]:
    ok = ok and cmd(c) is not None
if os.environ.get("VOICE_DIAL"):
    def timed(c, pat, t):
        t0 = time.time()
        m = cmd(c, pat, t)
        return m, time.time() - t0
    results = []
    for c in [b"AT+FCLASS=8", b"ATS7=6"]:
        ok = ok and cmd(c) is not None
    m, dt = timed(b"ATD7002", rb"(OK|BUSY|NO ANSWER|NO CARRIER|ERROR)\r\n", 15)
    print("7002: %s in %.1f s" % (m.group(1).decode() if m else "timeout", dt))
    ok = ok and m is not None and m.group(1) == b"BUSY"
    m, dt = timed(b"ATD7003", rb"(OK|BUSY|NO ANSWER|NO CARRIER|ERROR)\r\n", 15)
    print("7003: %s in %.1f s" % (m.group(1).decode() if m else "timeout", dt))
    ok = ok and m is not None and m.group(1) == b"NO ANSWER" and 5.5 < dt < 8
    time.sleep(1)
    m, dt = timed(b"ATD7001", rb"(OK|BUSY|NO ANSWER|NO CARRIER|ERROR)\r\n", 15)
    print("7001: %s in %.1f s" % (m.group(1).decode() if m else "timeout", dt))
    ok = ok and m is not None and m.group(1) == b"OK" and dt > 1.8
    d = expect(rb"\x107(\x107)*\x10~", 6)
    print("dte: DTMF 7 reported:", "yes" if d else "no")
    ok = ok and d is not None
    m = cmd(b"AT+VTS=1,2", rb"(OK|ERROR)\r\n", 5)
    print("tones:", m.group(1).decode() if m else "timeout")
    ok = ok and m is not None and m.group(1) == b"OK"
    # its hangup: <DLE>l alone (V.253 has no NO CARRIER in voice mode)
    m = expect(rb"\x10l", 10)
    expect(rb"(?!)", 1)         # what follows it, for a second
    nc = b"NO CARRIER" in seen[-50:]
    print("hangup:", ("<DLE>l" if m else "none") + (" NO CARRIER" if nc else ""))
    ok = ok and m is not None and not nc
    m = cmd(b"AT+FCLASS?", rb"(\d)\r\nOK\r\n", 5)
    ok = ok and m is not None and m.group(1) == b"8"
    # early media (183): the ringing tone heard as rings (<DLE>r), the
    # answer still the 200 OK, 7 s on; a busy tone BUSY at once, not the
    # 486 15 s on; an answer tone (<DLE>a) OK with no 200 OK
    ok = ok and cmd(b"ATS7=15") is not None
    m, dt = timed(b"ATD7004", rb"(OK|BUSY|NO ANSWER|NO CARRIER|ERROR)\r\n", 15)
    rings = seen[-200:].count(b"\x10r")
    print("7004: %d rings, %s in %.1f s" % (rings, m.group(1).decode() if m else "timeout", dt))
    ok = ok and m is not None and m.group(1) == b"OK" and dt > 6.5 and rings >= 2
    time.sleep(2)
    m, dt = timed(b"ATD7005", rb"(OK|BUSY|NO ANSWER|NO CARRIER|ERROR)\r\n", 20)
    print("7005: %s in %.1f s" % (m.group(1).decode() if m else "timeout", dt))
    ok = ok and m is not None and m.group(1) == b"BUSY" and dt < 8
    time.sleep(1)
    m, dt = timed(b"ATD7007", rb"(OK|BUSY|NO ANSWER|NO CARRIER|ERROR)\r\n", 15)
    a = b"\x10a" in seen[-100:]
    print("7007: %s%s in %.1f s" % ("<DLE>a " if a else "", m.group(1).decode() if m else "timeout", dt))
    ok = ok and m is not None and m.group(1) == b"OK" and a and dt < 3
    ok = ok and cmd(b"AT+VLS=0") is not None
    time.sleep(1)
    # a number that does not exist (404): ERROR, not NO CARRIER
    m, dt = timed(b"ATD7006", rb"(OK|BUSY|NO ANSWER|NO CARRIER|ERROR)\r\n", 15)
    print("7006: %s in %.1f s" % (m.group(1).decode() if m else "timeout", dt))
    ok = ok and m is not None and m.group(1) == b"ERROR"
    m = cmd(b"ATH")
    ok = ok and m is not None
    m = cmd(b"AT+FCLASS?", rb"(\d)\r\nOK\r\n", 5)
    print("after ATH: +FCLASS", m.group(1).decode() if m else "?")
    ok = ok and m is not None and m.group(1) == b"0"
    # a data call to a busy number: BUSY (SIP's 486), not NO CARRIER
    m, dt = timed(b"ATD7002", rb"(CONNECT|BUSY|NO ANSWER|NO CARRIER|ERROR)[^\r]*\r\n", 15)
    print("data 7002: %s in %.1f s" % (m.group(1).decode() if m else "timeout", dt))
    ok = ok and m is not None and m.group(1) == b"BUSY"
    p.terminate()
    if not ok:
        print("modem said:", buf[-300:], seen[-300:])
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)
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
    dtmf = re.search(rb"\x105(\x105)*\x10~", seen) is not None
    print("dte: DTMF 5 reported:", "yes" if dtmf else "no")
    ok = ok and dtmf
    time.sleep(0.5)
    p.terminate()
    p.wait(10)
    time.sleep(0.2)
    text = open(os.environ.get("SIP_LOG", "/tmp/sip_call.log"), "rb").read()
    for want in [rb"hook flash, 100 ms \(RFC 4733\)",
                 rb"hook flash, 300 ms \(RFC 4733\)",
                 rb"DTMF 5 \(RFC 4733 event 5\), played as its tone"]:
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
        dte_write(b"Welcome to the test BBS\r\n")
        while True:
            l = expect(rb"([^\r\n]*)\r\n", 120)
            if l is None or l.group(1) == b"BYE":
                break
            if l.group(1):
                dte_write(l.group(1).upper() + b"\r\n")
        dte_write(b"Goodbye\r\n")
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
            dte_write(l + b"\r\n")
        for l in LINES:
            ok = ok and expect(re.escape(l.upper() + b"\r\n"), 60 * slow) is not None
        dte_write(b"BYE\r\n")
        ok = ok and expect(rb"Goodbye\r\n", 30 * slow) is not None
        print("session: %s in %.1f s" % ("PASS" if ok else "FAIL", time.time() - t1))
        hung = expect(rb"NO CARRIER", 30)
        print("hangup:", "NO CARRIER" if hung else "none")
        ok = ok and (hung is not None or calls == 1)
p.terminate()
if os.environ.get("WIN_DRIVER") and windows:
    # (DCD on with the carrier, off at its end)
    print("dcd:", ", ".join(dcd) or "never")
    ok = ok and dcd[:2] == ["on", "off"]
if not ok:
    print("modem said:", buf[-300:])
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
