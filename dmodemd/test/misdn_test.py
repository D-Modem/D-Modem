#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# misdn_test.py <dmodemd> <libdmodem_fake_misdn.so>: dmodemd's --misdn line
# (misdn_line.cpp, q931.cpp) on fake mISDN cards (test/fake_misdn.cpp in
# front of the C library), two dmodemd on two S0 buses calling each other
# through this script's ISDN network: its side of layer 2 (mISDN's DL_
# primitives, the TEI and LAPD left out), Q.931's network side, and each
# call's B-channels joined, the octets each card is given sent on to the
# other at 8000 a second.
#
# A (MSN 111) calls B (MSN 222, S0=1, caller ID on):
#  1. AT+MS=CLEAR: unrestricted digital information, X.75: CONNECT 64000
#     at both, B's caller ID A's number, a line of text each way; A hangs
#     up, B's NO CARRIER.
#  2. A 3.1 kHz audio call with AT+MS=V22B at both: CONNECT 2400, text each
#     way, B hangs up this time.
#  3. 333, busy (cause 17): BUSY at A.
#  4. 444, no answer from its user (cause 19): NO ANSWER at A.
# A's audio call's first B-channel octets are its idle A-law silence bit
# reversed (0xD5 as 0xAB): the transparent mode's bit order.
#
# DMODEMD_DEBUG=<level>: dmodemd's -d (their logs in $TMPDIR/misdn.log)
import os, select, shutil, socket, struct, subprocess, sys, tempfile
import threading, time, tty, re

dmodemd, fake = sys.argv[1], sys.argv[2]
tmp = tempfile.mkdtemp(prefix="misdn")
SOCK = os.path.join(tmp, "net")
log = open(os.path.join(tempfile.gettempdir(), "misdn.log"), "wb")
lock = threading.Lock()

def note(s):
    with lock:
        log.write(s.encode() + b"\n")
        log.flush()

def fail(why):
    print("FAIL:", why, flush=True)
    try:
        log.flush()
        with open(log.name, "rb") as f:
            sys.stdout.write(f.read()[-8000:].decode("latin-1"))
    except OSError:
        pass
    sys.stdout.flush()
    os._exit(1)

# mISDN's primitives
PH_ACTIVATE_REQ, PH_DEACTIVATE_REQ, PH_DATA_REQ = 0x0101, 0x0201, 0x2001
PH_ACTIVATE_IND, PH_DEACTIVATE_IND, PH_DATA_IND = 0x0102, 0x0202, 0x2002
DL_ESTABLISH_REQ, DL_DATA_REQ = 0x1004, 0x3004
DL_ESTABLISH_CNF, DL_DATA_IND, DL_UNITDATA_IND = 0x5008, 0x3008, 0x3108
BIND = 0xFFFF0000
LAPD_TE, B_RAW = 0x10, 0x21
# Q.931's
ALERTING, CALL_PROC, SETUP, CONNECT, CONNECT_ACK = 0x01, 0x02, 0x05, 0x07, 0x0F
DISCONNECT, RELEASE, RELEASE_COMPLETE = 0x45, 0x4D, 0x5A
MSN = {"A": "111", "B": "222"}
# numbers the network itself answers: the cause of their DISCONNECT
REFUSED = {"333": 17, "444": 19}

def ie(msg, ident):
    i = 4
    while i < len(msg):
        if msg[i] & 0x80:
            i += 1
            continue
        if msg[i] == ident:
            return msg[i + 2:i + 2 + msg[i + 1]]
        i += 2 + msg[i + 1]
    return None

def cause(c):
    return bytes([0x08, 2, 0x80, 0x80 | c])

# a terminal's side of a call: the bus it is on, the call reference, whether
# the terminal made the call; the call's other side (None: the network)
class Leg:
    def __init__(self, term, cr, caller):
        self.term, self.cr, self.caller = term, cr, caller
        self.other = None
        self.connected = False

class Terminal:
    def __init__(self, name):
        self.name = name
        self.d = None
        self.b = {}                 # channel: connection
        self.active = set()         # channels activated
        self.tx = {1: bytearray(), 2: bytearray()}  # octets to the card
        self.leg = None
        self.first_b = {}           # channel: the first octets it sent

class Network:
    def __init__(self):
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.srv.bind(SOCK)
        # (dmodemd runs as nobody when the test is root's)
        os.chmod(tmp, 0o755)
        os.chmod(SOCK, 0o777)
        self.srv.listen(8)
        self.terms = {n: Terminal(n) for n in MSN}
        self.conns = {}             # connection: (terminal, channel or 0)
        self.next_cr = 0x10
        threading.Thread(target=self.loop, daemon=True).start()

    def send(self, c, prim, data=b""):
        try:
            c.send(struct.pack("<II", prim, 0xFFFF) + data)
        except OSError:
            pass

    # a Q.931 message to a terminal's leg (flag: the network's side)
    def q931(self, leg, t, ies=b"", unitdata=False):
        flag = 0x80 if leg.caller else 0
        m = bytes([0x08, 1, leg.cr | flag, t]) + ies
        note("net > %s: %s" % (leg.term.name, m.hex(" ")))
        self.send(leg.term.d, DL_UNITDATA_IND if unitdata else DL_DATA_IND, m)

    def loop(self):
        tick = time.time()
        while True:
            r, _, _ = select.select([self.srv] + list(self.conns), [], [],
                                    max(0, tick - time.time()))
            for c in r:
                if c is self.srv:
                    conn, _ = self.srv.accept()
                    self.conns[conn] = (None, 0)
                    continue
                try:
                    d = c.recv(4096)
                except OSError:
                    d = b""
                if not d:
                    self.gone(c)
                    continue
                self.message(c, d)
            if time.time() >= tick:
                tick += 0.02
                self.clock()

    def gone(self, c):
        term, ch = self.conns.pop(c)
        if term and ch:
            term.b.pop(ch, None)
            term.active.discard(ch)

    def message(self, c, d):
        prim, ident = struct.unpack("<II", d[:8])
        data = d[8:]
        term, ch = self.conns[c]
        if prim == BIND:
            proto, chan = ident >> 16, ident & 0xFF
            term = self.terms[data.decode()]
            if proto == LAPD_TE:
                term.d = c
                self.conns[c] = (term, 0)
            elif proto == B_RAW:
                term.b[chan] = c
                term.tx[chan] = bytearray()
                self.conns[c] = (term, chan)
            return
        if prim == DL_ESTABLISH_REQ:
            self.send(c, DL_ESTABLISH_CNF)
        elif prim == DL_DATA_REQ:
            self.call_control(term, data)
        elif prim == PH_ACTIVATE_REQ:
            term.active.add(ch)
            self.send(c, PH_ACTIVATE_IND)
        elif prim == PH_DEACTIVATE_REQ:
            term.active.discard(ch)
        elif prim == PH_DATA_REQ:
            term.first_b.setdefault(ch, bytes(data[:16]))
            leg = term.leg
            if leg and leg.other and leg.connected and leg.other.connected:
                peer = leg.other.term
                q = peer.tx[1]
                q += data
                if len(q) > 4000:
                    del q[:len(q) - 4000]

    # each active B-channel's 20 ms of octets: its peer's, or ones
    def clock(self):
        for term in self.terms.values():
            for ch, c in list(term.b.items()):
                if ch not in term.active:
                    continue
                q = term.tx[ch]
                out = bytes(q[:160]) + b"\xff" * max(0, 160 - len(q))
                del q[:160]
                self.send(c, PH_DATA_IND, out)

    def call_control(self, term, m):
        note("%s > net: %s" % (term.name, m.hex(" ")))
        cr, flag, t = m[2] & 0x7F, m[2] & 0x80, m[3]
        leg = term.leg
        if t == SETUP and not flag:
            called = ie(m, 0x70)[1:].decode()
            leg = term.leg = Leg(term, cr, True)
            self.q931(leg, CALL_PROC, bytes([0x18, 1, 0x89]))
            if called in REFUSED:
                self.q931(leg, DISCONNECT, cause(REFUSED[called]))
                return
            peer = [x for x in self.terms.values() if MSN[x.name] == called]
            if not peer or peer[0].leg:
                self.q931(leg, DISCONNECT, cause(17 if peer else 1))
                return
            peer = peer[0]
            self.next_cr += 1
            other = peer.leg = Leg(peer, self.next_cr, False)
            leg.other, other.other = other, leg
            self.q931(other, SETUP, bytes([0x04]) + bytes([len(ie(m, 4))]) +
                      ie(m, 4) + bytes([0x18, 1, 0x89]) +
                      bytes([0x6C, 2 + len(MSN[term.name]), 0x01, 0x80]) +
                      MSN[term.name].encode() +
                      bytes([0x70, 1 + len(called), 0x81]) + called.encode() + b"\xa1",
                      unitdata=True)
            return
        if not leg or leg.cr != cr:
            return
        other = leg.other
        if t == ALERTING and other:
            self.q931(other, ALERTING)
        elif t == CONNECT and other:
            self.q931(leg, CONNECT_ACK)
            leg.connected = True
            self.q931(other, CONNECT)
            other.connected = True
        elif t == DISCONNECT:
            self.q931(leg, RELEASE, cause(16))
            if other:
                self.q931(other, DISCONNECT, cause(16))
                other.other = None
            leg.other = None
        elif t == RELEASE:
            self.q931(leg, RELEASE_COMPLETE)
            term.leg = None
        elif t == RELEASE_COMPLETE:
            term.leg = None

net = Network()

def as_nobody():
    if os.geteuid() == 0:
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)

def start(name):
    env = dict(os.environ, LD_PRELOAD=fake, FAKE_MISDN=SOCK,
               FAKE_MISDN_TERMINAL=name)
    pre = ["stdbuf", "-oL", "-eL"] if shutil.which("stdbuf") else []
    p = subprocess.Popen(pre + [dmodemd, "-n", "-d" +
                                os.environ.get("DMODEMD_DEBUG", "2"),
                                "--misdn", "0", "--misdn-msn", MSN[name],
                                "misdn" + name],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         env=env, preexec_fn=as_nobody)
    pty = None
    t0 = time.time()
    while time.time() - t0 < 10:
        l = p.stdout.readline()
        if not l:
            break
        note(name + ": " + l.decode(errors="replace").rstrip())
        m = re.search(rb"TTY is `(\S+)'", l)
        if m:
            pty = m.group(1).decode()
            break
    if not pty:
        p.kill()
        fail("dmodemd %s gave no pty" % name)
    def drain():
        for l in p.stdout:
            note(name + ": " + l.decode(errors="replace").rstrip())
    threading.Thread(target=drain, daemon=True).start()
    fd = os.open(pty, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    os.set_blocking(fd, False)
    return p, Dte(fd, name)

class Dte:
    def __init__(self, fd, name):
        self.fd, self.name, self.buf = fd, name, b""
    def send(self, b):
        note("%s DTE > %r" % (self.name, b))
        os.write(self.fd, b)
    def expect(self, pat, t):
        end = time.time() + t
        while time.time() < end:
            m = re.search(pat, self.buf)
            if m:
                note("%s DTE < %r" % (self.name, self.buf[:m.end()]))
                self.buf = self.buf[m.end():]
                return m
            try:
                self.buf += os.read(self.fd, 4096)
            except BlockingIOError:
                time.sleep(0.02)
        return None
    def must(self, pat, t, what):
        m = self.expect(pat, t)
        if not m:
            fail("%s: %s: %r" % (self.name, what, self.buf))
        return m

def text_both_ways(a, b):
    time.sleep(0.5)
    a.send(b"hello from A\r\n")
    b.send(b"hello from B\r\n")
    b.must(rb"hello from A\r\n", 10, "A's text")
    a.must(rb"hello from B\r\n", 10, "B's text")

pa, a = start("A")
pb, b = start("B")
try:
    a.send(b"ATE0X3\\N0\r")
    a.must(rb"OK", 3, "no OK")
    b.send(b"ATE0\\N0S0=1+VCID=1\r")
    b.must(rb"OK", 3, "no OK")

    # 1. the clear channel
    a.send(b"AT+MS=CLEAR;D222\r")
    b.must(rb"RING", 5, "no RING")
    b.must(rb"NMBR = 111", 3, "no caller ID")
    b.must(rb"CONNECT 64000", 10, "no CONNECT 64000")
    a.must(rb"CONNECT 64000", 10, "no CONNECT 64000")
    text_both_ways(a, b)
    time.sleep(0.5)
    a.send(b"+++")
    a.must(rb"OK", 5, "no OK after +++")
    a.send(b"ATH\r")
    a.must(rb"OK", 5, "no OK after ATH")
    b.must(rb"NO CARRIER", 10, "no NO CARRIER")
    print("1. clear channel: CONNECT 64000, caller ID, data both ways",
          flush=True)
    time.sleep(1)

    # 2. 3.1 kHz audio, V.22bis
    net.terms["A"].first_b.clear()
    b.send(b"AT+MS=V22B\r")
    b.must(rb"OK", 3, "no OK")
    a.send(b"AT+MS=V22B;D222\r")
    b.must(rb"CONNECT 2400", 30, "no CONNECT 2400")
    a.must(rb"CONNECT 2400", 30, "no CONNECT 2400")
    first = net.terms["A"].first_b.get(1, b"")
    if not first.startswith(b"\xab\xab\xab\xab"):
        fail("A's first B-channel octets: %s" % first.hex(" "))
    text_both_ways(a, b)
    time.sleep(0.5)
    b.send(b"+++")
    b.must(rb"OK", 5, "no OK after +++")
    b.send(b"ATH\r")
    b.must(rb"OK", 5, "no OK after ATH")
    a.must(rb"NO CARRIER", 15, "no NO CARRIER")
    print("2. 3.1 kHz audio: CONNECT 2400, data both ways", flush=True)
    time.sleep(1)

    # 3., 4. refused
    a.send(b"ATD333\r")
    a.must(rb"BUSY", 10, "no BUSY")
    a.send(b"ATD444\r")
    a.must(rb"NO ANSWER", 10, "no NO ANSWER")
    print("3., 4. BUSY, NO ANSWER", flush=True)
finally:
    for p in (pa, pb):
        p.kill()
        p.wait()
    shutil.rmtree(tmp, ignore_errors=True)
print("PASS", flush=True)
