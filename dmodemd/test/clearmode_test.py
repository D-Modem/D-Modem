#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# clearmode_test.py <dmodemd>: dmodemd's clear 64 kbit/s channel over SIP
# (RFC 4040's CLEARMODE, an ISDN data call's), against this script as the
# far end: a SIP peer of its own on 127.0.0.1 and, on the call's RTP, the
# far end of the clear channel pump (dsp/dp_clear.cpp: HDLC flags until
# it hears 32 of ours, 64 more, 64 octets of ones, then data) with plain
# asynchronous characters (start bit, 8 data bits, stop bit; \N0 at
# dmodemd, and AT+B2=7, the modem link, as a clear channel's default
# is X.75), each octet's first bit its most significant (I.460, RFC 4040).
#
# Out: dmodemd (AT\N0+B2=7+MS=CLEAR) dials; its offer must have CLEARMODE
# first, then PCMA and PCMU; we answer in CLEARMODE; CONNECT 64000 at
# dmodemd, and a line of text each way.
# In: dmodemd (-L, the default +MS) registers with us; we call it with
# CLEARMODE first in the offer; it answers (S0=1) in CLEARMODE, with no
# telephone-event, CONNECT 64000, and a line each way.
#
# DMODEMD_DEBUG=<level>: dmodemd's -d (its log in $TMPDIR/clearmode.log)
import os, random, re, select, shutil, socket, subprocess, sys, tempfile
import threading
import time, tty

dmodemd = sys.argv[1]
SIP_PORT = 15090 + random.randrange(500) * 2      # ours
DM_PORT = SIP_PORT + 1                            # dmodemd's
log = open(os.path.join(tempfile.gettempdir(), "clearmode.log"), "wb")
FLAG, ONES = 0x7E, 0xFF

# the reason, and the end of the log (SIP and dmodemd's output) for CI
def fail(why):
    print("FAIL:", why, flush=True)
    try:
        log.flush()
        with open(log.name, "rb") as f:
            tail = f.read()[-6000:]
        sys.stdout.write(tail.decode("latin-1"))
    except OSError:
        pass
    sys.stdout.flush()
    sys.exit(1)

# the far end of the clear channel on an RTP socket, its own thread: the
# start, then characters to send (send()) and those received (got)
class ClearEnd:
    def __init__(self, rtp, peer, pt):
        self.rtp, self.peer, self.pt = rtp, peer, pt
        self.tx_bits = []           # characters' bits waiting
        self.got = b""
        self.heard = self.open = False
        self.run = self.tail = self.mark = 0
        self.rx_bits = []
        self.stop = False
        self.lock = threading.Lock()
        threading.Thread(target=self.loop, daemon=True).start()
    # a string's characters, 8N1, LSB first
    def send(self, s):
        with self.lock:
            for c in s:
                self.tx_bits += [0] + [(c >> k) & 1 for k in range(8)] + [1]
    # the next octet to send
    def out_octet(self):
        if not self.heard or self.tail < 64:
            if self.heard:
                self.tail += 1
            return FLAG
        if self.mark < 64:
            self.mark += 1
            return ONES
        with self.lock:
            bits = self.tx_bits[:8]
            del self.tx_bits[:8]
        bits += [1] * (8 - len(bits))
        return sum(b << (7 - k) for k, b in enumerate(bits))
    # an octet received
    def in_octet(self, o):
        if not self.heard:
            self.run = self.run + 1 if o == FLAG else 0
            self.heard = self.run >= 32
            return
        if not self.open:
            self.open = o == ONES
            if not self.open:
                return
        self.rx_bits += [(o >> (7 - k)) & 1 for k in range(8)]
        while True:
            try:
                i = self.rx_bits.index(0)
            except ValueError:
                self.rx_bits = []
                return
            if len(self.rx_bits) < i + 10:
                del self.rx_bits[:i]
                return
            c = sum(self.rx_bits[i + 1 + k] << k for k in range(8))
            if self.rx_bits[i + 9] == 1:
                self.got += bytes([c])
            del self.rx_bits[:i + 10]
    def loop(self):
        seq, ts, ssrc = 0, 0, 0x1234
        nxt = time.time()
        while not self.stop:
            # 20 ms of octets out, what came in
            payload = bytes(self.out_octet() for _ in range(160))
            hdr = bytes([0x80, self.pt, seq >> 8 & 255, seq & 255]) + \
                ts.to_bytes(4, "big") + ssrc.to_bytes(4, "big")
            self.rtp.sendto(hdr + payload, self.peer)
            seq, ts = (seq + 1) & 0xFFFF, ts + 160
            nxt += 0.02
            while True:
                left = nxt - time.time()
                r, _, _ = select.select([self.rtp], [], [], max(left, 0))
                if not r:
                    break
                d, _ = self.rtp.recvfrom(2048)
                if len(d) > 12 and d[1] & 0x7F == self.pt:
                    for o in d[12:]:
                        self.in_octet(o)

# SIP
sip = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sip.bind(("127.0.0.1", SIP_PORT))
rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rtp.bind(("127.0.0.1", 0))
RTP_PORT = rtp.getsockname()[1]

# a SIP message from dmodemd (None after t s): its first line, its
# headers (lower case names, first of each), its body, where it came from
def sip_recv(t):
    sip.settimeout(t)
    try:
        d, a = sip.recvfrom(65535)
    except socket.timeout:
        return None
    head, _, body = d.decode(errors="replace").partition("\r\n\r\n")
    lines = head.split("\r\n")
    hdrs = {}
    for l in lines[1:]:
        n, _, v = l.partition(":")
        hdrs.setdefault(n.strip().lower(), v.strip())
    log.write(b"<<< " + d + b"\n"); log.flush()
    return lines[0], hdrs, body, a, head
def sip_send(msg, a):
    log.write(b">>> " + msg.encode() + b"\n"); log.flush()
    sip.sendto(msg.encode(), a)
# all the Via lines of a request
def vias(head):
    return [l for l in head.split("\r\n")[1:] if re.match(r"(?i)via\s*:", l)]
# a response to request (first, hdrs, head) with code, To tagged, a body
def reply(req, code, text, a, body="", tag="far1"):
    first, hdrs, _, _, head = req
    to = hdrs["to"] + ("" if "tag=" in hdrs["to"] or code == 100 else ";tag=" + tag)
    m = "SIP/2.0 %d %s\r\n" % (code, text) + "\r\n".join(vias(head)) + "\r\n" + \
        "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n" % (
            hdrs["from"], to, hdrs["call-id"], hdrs["cseq"]) + \
        "Contact: <sip:far@127.0.0.1:%d>\r\n" % SIP_PORT
    if body:
        m += "Content-Type: application/sdp\r\n"
    m += "Content-Length: %d\r\n\r\n%s" % (len(body), body)
    sip_send(m, a)
def sdp(fmts, rtpmaps):
    return ("v=0\r\no=far 1 1 IN IP4 127.0.0.1\r\ns=far\r\nc=IN IP4 127.0.0.1\r\n"
            "t=0 0\r\nm=audio %d RTP/AVP %s\r\n" % (RTP_PORT, fmts) +
            "".join("a=rtpmap:%s\r\n" % r for r in rtpmaps) + "a=ptime:20\r\n")
# the audio line of an SDP body: its address, port and formats, the
# rtpmaps
def audio_of(body):
    m = re.search(r"m=audio (\d+) RTP/AVP ([\d ]+)", body)
    c = re.search(r"c=IN IP4 (\S+)", body)
    return (c.group(1), int(m.group(1))), m.group(2).split(), \
        dict(re.findall(r"a=rtpmap:(\d+) (\S+)", body))

# dmodemd and its pty.  Run as root (CI's containers), it goes on as
# nobody from the start: as root it would make /dev/ttyDM0 and save
# /var/lib/dmodem/data.dmodem0, which root_test.py (run beside it)
# counts as its own
def as_nobody():
    if os.geteuid() == 0:
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)
def start_dmodemd(listen):
    env = dict(os.environ, SIP_LOGIN="dm:secret@127.0.0.1:%d" % SIP_PORT,
               SIP_PORT=str(DM_PORT))
    # (its lines as they come, where stdbuf is: not busybox's)
    pre = ["stdbuf", "-oL", "-eL"] if shutil.which("stdbuf") else []
    p = subprocess.Popen(pre + [dmodemd,
                          "-d" + os.environ.get("DMODEMD_DEBUG", "1")] +
                         (["-L"] if listen else []),
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                         preexec_fn=as_nobody)
    pty = None
    t0 = time.time()
    while time.time() - t0 < 10:
        l = p.stdout.readline()
        log.write(l); log.flush()
        m = re.search(rb"TTY is `(\S+)'", l)
        if m:
            pty = m.group(1).decode()
            break
    if not pty:
        p.kill()
        fail("dmodemd gave no pty")
    def drain():
        for l in p.stdout:
            log.write(l); log.flush()
    threading.Thread(target=drain, daemon=True).start()
    fd = os.open(pty, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    os.set_blocking(fd, False)
    return p, fd
class Dte:
    def __init__(self, fd):
        self.fd, self.buf = fd, b""
    def send(self, b):
        os.write(self.fd, b)
    # the first match of pat (None after t s)
    def expect(self, pat, t):
        end = time.time() + t
        while time.time() < end:
            m = re.search(pat, self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            try:
                self.buf += os.read(self.fd, 4096)
            except BlockingIOError:
                time.sleep(0.02)
        return None

def data_both_ways(dte, far):
    m = dte.expect(rb"CONNECT (\d+)", 5)
    if not m or m.group(1) != b"64000":
        fail("no CONNECT 64000: %r" % dte.buf)
    time.sleep(0.2)
    dte.send(b"hello from dmodemd\r\n")
    far.send(b"hello from the far end\r\n")
    end = time.time() + 5
    while time.time() < end and b"hello from dmodemd\r\n" not in far.got:
        time.sleep(0.05)
    if b"hello from dmodemd\r\n" not in far.got:
        fail("the far end got %r" % far.got)
    if not dte.expect(rb"hello from the far end\r\n", 5):
        fail("dmodemd's DTE got %r" % dte.buf)

# out: dmodemd dials with +MS=CLEAR
def outgoing():
    p, fd = start_dmodemd(False)
    dte = Dte(fd)
    try:
        dte.send(b"ATE0\\N0X3+B2=7;+MS=CLEAR\r")
        if not dte.expect(rb"OK", 3):
            fail("no OK: %r" % dte.buf)
        dte.send(b"ATD1234\r")
        while True:
            req = sip_recv(5)
            if not req:
                fail("no INVITE")
            if req[0].startswith("INVITE"):
                break
        first, hdrs, body, a, head = req
        _, fmts, maps = audio_of(body)
        names = [maps.get(f, {"0": "PCMU/8000", "8": "PCMA/8000"}.get(f, "")).upper()
                 for f in fmts]
        if names[:3] != ["CLEARMODE/8000", "PCMA/8000", "PCMU/8000"]:
            fail("the offer's formats: %r" % names)
        dm_rtp, _, _ = audio_of(body)
        reply(req, 100, "Trying", a)
        reply(req, 200, "OK", a, sdp(fmts[0], [fmts[0] + " CLEARMODE/8000"]))
        while True:
            r = sip_recv(5)
            if not r:
                fail("no ACK")
            if r[0].startswith("ACK"):
                break
        far = ClearEnd(rtp, dm_rtp, int(fmts[0]))
        data_both_ways(dte, far)
        far.stop = True
        print("out: CONNECT 64000 in CLEARMODE, data both ways", flush=True)
    finally:
        p.kill()
        p.wait()
        os.close(fd)

# in: dmodemd -L answers our CLEARMODE call
def incoming():
    p, fd = start_dmodemd(True)
    dte = Dte(fd)
    try:
        # its REGISTER: 200 OK
        req = sip_recv(5)
        if not req or not req[0].startswith("REGISTER"):
            fail("no REGISTER: %r" % (req,))
        reply(req, 200, "OK", req[3])
        dte.send(b"ATE0\\N0S0=1+B2=7\r")
        if not dte.expect(rb"OK", 3):
            fail("no OK: %r" % dte.buf)
        dm = ("127.0.0.1", DM_PORT)
        call_id = "clear%d@127.0.0.1" % random.randrange(10**9)
        body = sdp("97 8 0", ["97 CLEARMODE/8000", "8 PCMA/8000", "0 PCMU/8000"])
        via = "Via: SIP/2.0/UDP 127.0.0.1:%d;branch=z9hG4bK%d;rport" % (
            SIP_PORT, random.randrange(10**9))
        frm = "<sip:far@127.0.0.1:%d>;tag=far2" % SIP_PORT
        to = "<sip:dm@127.0.0.1:%d>" % DM_PORT
        sip_send("INVITE sip:dm@127.0.0.1:%d SIP/2.0\r\n%s\r\nMax-Forwards: 70\r\n"
                 "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 INVITE\r\n"
                 "Contact: <sip:far@127.0.0.1:%d>\r\nContent-Type: application/sdp\r\n"
                 "Content-Length: %d\r\n\r\n%s" % (DM_PORT, via, frm, to, call_id,
                                                  SIP_PORT, len(body), body), dm)
        ok = None
        end = time.time() + 15
        while time.time() < end:
            r = sip_recv(1)
            if r and r[0].startswith("SIP/2.0 200"):
                ok = r
                break
        if not ok:
            fail("no 200 OK")
        dm_rtp, fmts, maps = audio_of(ok[2])
        if fmts[0] != "97" or maps.get("97", "").upper() != "CLEARMODE/8000":
            fail("the answer is not CLEARMODE: %r" % ok[2])
        if any("telephone-event" in v for v in maps.values()):
            fail("telephone-event on a clear channel: %r" % ok[2])
        to_tagged = ok[1]["to"]
        sip_send("ACK sip:dm@127.0.0.1:%d SIP/2.0\r\n%s\r\nMax-Forwards: 70\r\n"
                 "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 ACK\r\n"
                 "Content-Length: 0\r\n\r\n" % (
                     DM_PORT, via.replace("branch=z9hG4bK", "branch=z9hG4bKa"),
                     frm, to_tagged, call_id), dm)
        far = ClearEnd(rtp, dm_rtp, 97)
        data_both_ways(dte, far)
        far.stop = True
        print("in: answered in CLEARMODE, CONNECT 64000, data both ways", flush=True)
    finally:
        p.kill()
        p.wait()
        os.close(fd)

outgoing()
incoming()
print("PASS", flush=True)
