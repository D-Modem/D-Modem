#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# bbs_sim.py - both ends of a test call through Asterisk: a BBS on
# BBS_PORT (served by DModem(bbs), the answering modem) and a terminal
# server on TERM_PORT (DModem(term,o), the calling modem, connects to it
# once online and so plays the user).  The user reads the banner, sends
# lines, checks the echoes, logs off; the BBS then closes, which hangs up
# the call.  Exit status 0 when all of it came through.
#
#   bbs_sim.py [timeout]          both, the BBS on TCP
#   BBS_EXEC=1 bbs_sim.py         only the terminal: the BBS is run by
#                                 DModem (mode=exec) as
#   bbs_sim.py --stdio            the BBS on stdin/stdout (the pty)
#   BBS_UNIX=path bbs_sim.py      both, the BBS on a unix socket (mode=unix,
#                                 header=yes): the call's details come
#                                 first, and must be there
#
# BBS_LINGER=1 (--stdio): after the session the program closes its pty
# but stays, deaf to SIGHUP, so that DModem has to kill it.
#
# BBS_CONNECT_DELAY=<s>: the BBS's port takes no connection until s
# seconds after the start (its accept queue full with one of our own),
# so DModem's connect to it waits in SYN retransmissions meanwhile; the
# line must go on while it does.
import os, re, socket, sys, threading, time

BBS_PORT, TERM_PORT = 2323, 2324
LINES = [b"hello from the caller %d " % i + b"x" * 40 for i in range(20)]
result = {}

# a socket's sendall, recv and close on stdout and stdin (--stdio: the
# BBS as DModem's program, on its pty)
class Stdio:
    # all of b, as many writes as it takes
    def sendall(self, b):
        while b:
            b = b[os.write(1, b):]
    # up to n bytes; b"" at end of file
    def recv(self, n):
        return os.read(0, n)
    # the pty stays: DModem hangs up when the program exits
    def close(self):
        pass

# the BBS: the banner, then each line back in capitals, until BYE
# (Goodbye, then the connection closed)
def bbs(conn):
    conn.sendall(b"Welcome to the test BBS\r\n")
    buf = b""
    while True:
        d = conn.recv(4096)
        if not d:
            break
        buf += d
        while b"\r\n" in buf:
            line, buf = buf.split(b"\r\n", 1)
            if line == b"BYE":
                conn.sendall(b"Goodbye\r\n")
                time.sleep(0.2)
                conn.close()
                return
            conn.sendall(line.upper() + b"\r\n")

# the user: reads the banner, sends LINES, checks their echoes, BYE and
# Goodbye; ok, secs and bytes (each way) into result, then waits for the
# connection to close (the call's end)
def term(conn):
    f = conn.makefile("rb")
    t0 = time.time()
    ok = f.readline() == b"Welcome to the test BBS\r\n"
    for l in LINES:
        conn.sendall(l + b"\r\n")
    for l in LINES:
        ok = ok and f.readline() == l.upper() + b"\r\n"
    conn.sendall(b"BYE\r\n")
    ok = ok and f.readline() == b"Goodbye\r\n"
    n = sum(len(l) + 2 for l in LINES)
    result["ok"] = ok
    result["secs"] = time.time() - t0
    result["bytes"] = n
    # the BBS hung up: the modem goes, the socket closes
    while conn.recv(4096):
        pass

# the header of a mode=unix (or tcp) profile with header=yes: NAME=value
# lines and an empty line, read a byte at a time (what follows is the
# caller's); checked, and the result into result["header"]
def header(conn):
    h, line = {}, b""
    while True:
        b = conn.recv(1)
        if not b:
            break
        if b != b"\n":
            line += b
            continue
        if not line:
            break
        k, _, v = line.decode().partition("=")
        h[k] = v
        line = b""
    print("bbs: header %s" % h, flush=True)
    connected = int(h.get("DMODEM_CONNECTED", "0") or 0)
    result["header"] = (
        h.get("DMODEM_PROFILE") == "bbsunix" and
        h.get("DMODEM_CHANNEL", "").startswith("Local/") and
        re.fullmatch(r"\d+/\d+", h.get("DMODEM_RATE", "")) is not None and
        "DMODEM_CALLERID" in h and
        h.get("DMODEM_DIALED") == "s" and
        abs(time.time() - connected) < 600)

# the BBS on a unix socket at path, after the header
def serve_unix(path, fn):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    s = socket.socket(socket.AF_UNIX)
    s.bind(path)
    s.listen(1)
    s.settimeout(float(sys.argv[1]) if len(sys.argv) > 1 else 120)
    c, _ = s.accept()
    c.settimeout(120)
    header(c)
    fn(c)

# one connection on 127.0.0.1:port (accept timeout: argv[1] s, else
# 120), handed to fn; none taken until delay s from now
def serve(port, fn, delay=0):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(0 if delay else 1)
    s.settimeout(float(sys.argv[1]) if len(sys.argv) > 1 else 120)
    if delay:
        # the accept queue full (one connection, backlog 0): SYNs are
        # dropped, and connect() retries, until it has room again
        filler = socket.create_connection(("127.0.0.1", port))
        time.sleep(delay)
        s.accept()[0].close()
        filler.close()
        print("bbs: taking connections at %.1f s" % delay, flush=True)
    c, _ = s.accept()
    c.settimeout(120)
    fn(c)

if sys.argv[1:] == ["--stdio"]:
    bbs(Stdio())
    if os.environ.get("BBS_LINGER"):
        import signal
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        for fd in (0, 1, 2):
            os.close(fd)
        time.sleep(60)
    sys.exit(0)
tt = threading.Thread(target=serve, args=(TERM_PORT, term), daemon=True)
tt.start()
if os.environ.get("BBS_UNIX"):
    threading.Thread(target=serve_unix, args=(os.environ["BBS_UNIX"], bbs),
                     daemon=True).start()
elif not os.environ.get("BBS_EXEC"):
    threading.Thread(target=serve, args=(BBS_PORT, bbs,
                     float(os.environ.get("BBS_CONNECT_DELAY", "0"))),
                     daemon=True).start()
tt.join()
ok = result.get("ok", False)
print("user saw banner, %d echoed lines and goodbye: %s (%.1f s for %d bytes each way)"
      % (len(LINES), "PASS" if ok else "FAIL", result.get("secs", 0), result.get("bytes", 0)))
if os.environ.get("BBS_UNIX"):
    print("the BBS on the unix socket had the call's details first: %s"
          % ("PASS" if result.get("header") else "FAIL"))
    ok = ok and result.get("header", False)
sys.exit(0 if ok else 1)
