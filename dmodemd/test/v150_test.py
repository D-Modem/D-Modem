#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# v150_test.py <dmodemd>: dmodemd's V.150.1 modem relay over SIP
# (SIP_V150=yes, dmodemd/v150.h), against this script as the far end: a
# SIP peer of its own on 127.0.0.1 and a V.150.1 gateway of its own on
# the call's RTP (SSE, RFC 4733's ANSam) and SPRT (Annex B: TC2's control
# messages, TC1's data, the acknowledgements).
#
# Out: dmodemd dials; its offer must have SSE (v150fw) with its audio and
# a modem relay line (udpsprt, v150mr, mg=2).  We answer as the gateway of
# an answering modem: ANSam as RFC 4733 events until dmodemd's SSE:M (CM),
# SSE:M (p') back, INIT, JM_INFO and CONNECT (V.34, 33600); dmodemd's
# CONNECT back, CONNECT 33600 at its DTE, a line of text each way, and
# our CLEARDOWN: NO CARRIER.
# In: dmodemd (-L) registers with us; we call it as the gateway of a
# calling modem with both lines in our offer; it answers (S0=1) with
# both, sends ANSam as events, SSE:M (p') to our SSE:M (CM), INIT,
# JM_INFO and CONNECT; ours back, CONNECT 33600 at its DTE, a line each
# way.
#
# DMODEMD_DEBUG=<level>: dmodemd's -d (its log in $TMPDIR/v150.log)
import os, random, re, select, shutil, socket, subprocess, sys
import tempfile, threading, time, tty

dmodemd = sys.argv[1]
log = open(os.path.join(tempfile.gettempdir(), "v150.log"), "wb")
SSE_PT, TE_PT, SPRT_PT = 118, 101, 120
EV_ANSAM = 34
SSE_MR, RIC_CM, RIC_P = 3, 1, 19
INIT, JM_INFO, CONNECT, CLEARDOWN, I_OCTET = 1, 3, 5, 9, 18

# the reason, and the end of the log (SIP and dmodemd's output) for CI
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

# the far end's gateway: RTP (PCMU silence, SSE and ANSam events out;
# dmodemd's SSE and events in) and SPRT, each in a thread
class Gateway:
    def __init__(self, rtp, rtp_peer, sprt, sprt_peer):
        self.rtp, self.rtp_peer = rtp, rtp_peer
        self.sprt, self.sprt_peer = sprt, sprt_peer
        self.lock = threading.Lock()
        self.stop = False
        self.ansam = False          # our ANSam events go
        self.sse_q = []             # [payload, timestamp, copies left]
        self.sse_in = []            # dmodemd's SSEs (event, ric, info)
        self.events_in = set()      # dmodemd's RFC 4733 events
        self.seq = {1: 0, 2: 0}     # our next SEQUENCE NUMBERs
        self.rx_next = {1: 0, 2: 0} # theirs we take next (our BSN)
        self.acks = []
        self.msgs = []              # dmodemd's TC2 messages
        self.data = b""             # its TC1 octets
        threading.Thread(target=self.rtp_loop, daemon=True).start()
        threading.Thread(target=self.sprt_loop, daemon=True).start()
    def send_sse(self, event, ric, info):
        with self.lock:
            self.sse_q.append([bytes([event << 2, ric, info >> 8, info & 255]),
                               None, 3])
    def rtp_loop(self):
        seq, ts, ssrc = 0, 0, 0x5678
        ans_ts, ans_dur = None, 0
        nxt = time.time()
        def hdr(pt, t, m=False):
            nonlocal seq
            h = bytes([0x80, (0x80 if m else 0) | pt, seq >> 8 & 255,
                       seq & 255]) + t.to_bytes(4, "big") + \
                ssrc.to_bytes(4, "big")
            seq = (seq + 1) & 0xFFFF
            return h
        while not self.stop:
            with self.lock:
                for q in self.sse_q:
                    if q[1] is None:
                        q[1] = ts
                    self.rtp.sendto(hdr(SSE_PT, q[1]) + q[0], self.rtp_peer)
                    q[2] -= 1
                self.sse_q = [q for q in self.sse_q if q[2] > 0]
                ans = self.ansam
            if ans:
                first = ans_ts is None
                if first:
                    ans_ts, ans_dur = ts, 0
                ans_dur += 160
                self.rtp.sendto(hdr(TE_PT, ans_ts, first) +
                                bytes([EV_ANSAM, 12, ans_dur >> 8,
                                       ans_dur & 255]), self.rtp_peer)
            else:
                if ans_ts is not None:
                    for _ in range(3):
                        self.rtp.sendto(hdr(TE_PT, ans_ts) +
                                        bytes([EV_ANSAM, 0x80 | 12,
                                               ans_dur >> 8, ans_dur & 255]),
                                        self.rtp_peer)
                    ans_ts = None
                self.rtp.sendto(hdr(0, ts) + b"\xff" * 160, self.rtp_peer)
            ts += 160
            nxt += 0.02
            while True:
                left = nxt - time.time()
                r, _, _ = select.select([self.rtp], [], [], max(left, 0))
                if not r:
                    break
                d, _ = self.rtp.recvfrom(2048)
                if len(d) < 16:
                    continue
                pt = d[1] & 0x7F
                with self.lock:
                    if pt == SSE_PT:
                        e = (d[12] >> 2, d[13], d[14] << 8 | d[15])
                        if not self.sse_in or self.sse_in[-1] != e:
                            self.sse_in.append(e)
                    elif pt == TE_PT:
                        self.events_in.add(d[12])
    # a payload on tc (1 or 2): BSN ours, the acks owed with it
    def send(self, tc, payload):
        with self.lock:
            s = self.seq[tc]
            self.seq[tc] = (s + 1) & 0x3FFF
            self.sprt.sendto(self.packet(tc, s, payload), self.sprt_peer)
    def packet(self, tc, s, payload):
        acks, self.acks = self.acks[:3], self.acks[3:]
        bsn = self.rx_next.get(tc, 0)
        p = bytes([0, SPRT_PT, tc << 6 | s >> 8, s & 255,
                   len(acks) << 6 | bsn >> 8, bsn & 255])
        for t, q in acks:
            p += bytes([t << 6 | q >> 8, q & 255])
        return p + payload
    def sprt_loop(self):
        while not self.stop:
            r, _, _ = select.select([self.sprt], [], [], 0.03)
            with self.lock:
                if r:
                    d, _ = self.sprt.recvfrom(2048)
                    if len(d) < 6 or d[1] & 0x7F != SPRT_PT:
                        continue
                    tc, s = d[2] >> 6, (d[2] & 0x3F) << 8 | d[3]
                    noa = d[4] >> 6
                    pl = d[6 + 2 * noa:]
                    if tc in (1, 2) and pl:
                        self.acks.append((tc, s))
                        if s == self.rx_next[tc]:
                            self.rx_next[tc] = (s + 1) & 0x3FFF
                            if tc == 2:
                                self.msgs.append(pl)
                            elif pl[0] & 0x7F == I_OCTET:
                                self.data += pl[1:]
                while self.acks:
                    self.sprt.sendto(self.packet(0, 0, b""), self.sprt_peer)
    # the first of dmodemd's TC2 messages with id (None after t s)
    def msg(self, id, t):
        end = time.time() + t
        while time.time() < end:
            with self.lock:
                for m in self.msgs:
                    if m[0] & 0x7F == id:
                        return m
            time.sleep(0.02)
        return None
    # dmodemd's SSE (event, ric) (None after t s)
    def sse(self, event, ric, t):
        end = time.time() + t
        while time.time() < end:
            with self.lock:
                for e in self.sse_in:
                    if e[0] == event and e[1] == ric:
                        return e
            time.sleep(0.02)
        return None
def connect_msg(rate):
    return bytes([CONNECT, 4 << 2, 1, rate >> 8, rate & 255, rate >> 8,
                  rate & 255, 0, 0])

# SIP
def free_pair():
    for _ in range(50):
        port = 16100 + random.randrange(500) * 2
        ours = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            ours.bind(("127.0.0.1", port))
            probe.bind(("127.0.0.1", port + 1))
        except OSError:
            ours.close()
            continue
        finally:
            probe.close()
        return ours, port
    fail("no free pair of UDP ports in 16100-17100")

sip, SIP_PORT = free_pair()
DM_PORT = SIP_PORT + 1
def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    return s, s.getsockname()[1]

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
def vias(head):
    return [l for l in head.split("\r\n")[1:] if re.match(r"(?i)via\s*:", l)]
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
# our SDP: PCMU, telephone-event and SSE on rtp_port, SPRT on sprt_port
def sdp(rtp_port, sprt_port):
    return ("v=0\r\no=far 1 1 IN IP4 127.0.0.1\r\ns=far\r\nc=IN IP4 127.0.0.1\r\n"
            "t=0 0\r\nm=audio %d RTP/AVP 0 %d %d\r\n" % (rtp_port, TE_PT, SSE_PT) +
            "a=rtpmap:0 PCMU/8000\r\na=rtpmap:%d telephone-event/8000\r\n"
            "a=fmtp:%d 0-16,32-35\r\na=rtpmap:%d v150fw/8000\r\na=ptime:20\r\n" % (
                TE_PT, TE_PT, SSE_PT) +
            "m=audio %d udpsprt %d\r\na=sprtmap:%d v150mr/8000\r\n"
            "a=fmtp:%d mr=1;mg=2;CDSCselect=1;mrmods=1,3-4,12;jmdelay=no\r\n" % (
                sprt_port, SPRT_PT, SPRT_PT, SPRT_PT))
# dmodemd's SDP: its RTP and SPRT addresses; both lines checked
def lines_of(body):
    c = re.search(r"c=IN IP4 (\S+)", body).group(1)
    a = re.search(r"m=audio (\d+) RTP/AVP ([\d ]+)", body)
    s = re.search(r"m=audio (\d+) udpsprt (\d+)", body)
    if not a or not s or s.group(1) == "0":
        fail("no audio and modem relay lines: %r" % body)
    if not re.search(r"a=rtpmap:(\d+) v150fw/8000", body):
        fail("no SSE (v150fw): %r" % body)
    if not re.search(r"a=sprtmap:%s v150mr/8000" % s.group(2), body):
        fail("no sprtmap: %r" % body)
    if not re.search(r"a=fmtp:%s [^\r\n]*mg=2" % s.group(2), body):
        fail("no mg=2: %r" % body)
    return (c, int(a.group(1))), (c, int(s.group(1)))

def as_nobody():
    if os.geteuid() == 0:
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)
def start_dmodemd(listen):
    env = dict(os.environ, SIP_LOGIN="dm:secret@127.0.0.1:%d" % SIP_PORT,
               SIP_PORT=str(DM_PORT), SIP_V150="yes")
    env.pop("SIP_SRTP", None)
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

# CONNECT 33600 at the DTE, a line each way
def data_both_ways(dte, gw):
    m = dte.expect(rb"CONNECT (\d+)", 10)
    if not m or m.group(1) != b"33600":
        fail("no CONNECT 33600: %r" % dte.buf)
    time.sleep(0.2)
    dte.send(b"hello from dmodemd\r\n")
    gw.send(1, bytes([I_OCTET]) + b"hello from the far end\r\n")
    end = time.time() + 5
    while time.time() < end and b"hello from dmodemd\r\n" not in gw.data:
        time.sleep(0.05)
    if b"hello from dmodemd\r\n" not in gw.data:
        fail("the far end got %r" % gw.data)
    if not dte.expect(rb"hello from the far end\r\n", 5):
        fail("dmodemd's DTE got %r" % dte.buf)

# out: dmodemd dials, we are the answering modem's gateway
def outgoing():
    p, fd = start_dmodemd(False)
    dte = Dte(fd)
    rtp, rtp_port = udp()
    sprt, sprt_port = udp()
    try:
        dte.send(b"ATE0X3\r")
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
        dm_rtp, dm_sprt = lines_of(body)
        if not re.search(r"a=fmtp:\d+ 0-16,32-35", body):
            fail("telephone-event without the answer tones: %r" % body)
        reply(req, 100, "Trying", a)
        reply(req, 200, "OK", a, sdp(rtp_port, sprt_port))
        while True:
            r = sip_recv(5)
            if not r:
                fail("no ACK")
            if r[0].startswith("ACK"):
                break
        gw = Gateway(rtp, dm_rtp, sprt, dm_sprt)
        gw.ansam = True
        cm = gw.sse(SSE_MR, RIC_CM, 10)
        if not cm:
            fail("no SSE:M (CM) from dmodemd: %r" % gw.sse_in)
        if cm[2] != 0x5808:
            fail("CM's modulations %04x" % cm[2])
        gw.ansam = False
        gw.send_sse(SSE_MR, RIC_P, 0)
        gw.send(2, bytes([INIT, 0, 0]))
        if not gw.msg(INIT, 3):
            fail("no INIT from dmodemd")
        gw.send(2, bytes([JM_INFO, 0x86, 0x00, 0xA8, 0x01, 0x58, 0x00]))
        gw.send(2, connect_msg(33600))
        c = gw.msg(CONNECT, 3)
        if not c or c[1] >> 2 != 4 or (c[3] << 8 | c[4]) != 33600:
            fail("dmodemd's CONNECT: %r" % (c,))
        data_both_ways(dte, gw)
        gw.send(2, bytes([CLEARDOWN, 1, 0, 0]))
        if not dte.expect(rb"NO CARRIER", 5):
            fail("no NO CARRIER after our CLEARDOWN: %r" % dte.buf)
        gw.stop = True
        print("out: modem relay as the caller, CONNECT 33600, data both "
              "ways, cleared down", flush=True)
    finally:
        p.kill()
        p.wait()
        os.close(fd)

# in: we call dmodemd -L as the calling modem's gateway
def incoming():
    p, fd = start_dmodemd(True)
    dte = Dte(fd)
    rtp, rtp_port = udp()
    sprt, sprt_port = udp()
    try:
        req = sip_recv(5)
        if not req or not req[0].startswith("REGISTER"):
            fail("no REGISTER: %r" % (req,))
        reply(req, 200, "OK", req[3])
        dte.send(b"ATE0S0=1\r")
        if not dte.expect(rb"OK", 3):
            fail("no OK: %r" % dte.buf)
        dm = ("127.0.0.1", DM_PORT)
        call_id = "v150%d@127.0.0.1" % random.randrange(10**9)
        body = sdp(rtp_port, sprt_port)
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
        dm_rtp, dm_sprt = lines_of(ok[2])
        sip_send("ACK sip:dm@127.0.0.1:%d SIP/2.0\r\n%s\r\nMax-Forwards: 70\r\n"
                 "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 ACK\r\n"
                 "Content-Length: 0\r\n\r\n" % (
                     DM_PORT, via.replace("branch=z9hG4bK", "branch=z9hG4bKa"),
                     frm, ok[1]["to"], call_id), dm)
        gw = Gateway(rtp, dm_rtp, sprt, dm_sprt)
        end = time.time() + 5
        while time.time() < end and EV_ANSAM not in gw.events_in:
            time.sleep(0.05)
        if EV_ANSAM not in gw.events_in:
            fail("no ANSam (event 34) from dmodemd: %r" % gw.events_in)
        gw.send_sse(SSE_MR, RIC_CM, 0x5808)
        if not gw.sse(SSE_MR, RIC_P, 3):
            fail("no SSE:M (p') from dmodemd: %r" % gw.sse_in)
        gw.send(2, bytes([INIT, 0, 0]))
        if not gw.msg(INIT, 3):
            fail("no INIT from dmodemd")
        jm = gw.msg(JM_INFO, 3)
        if not jm or jm[3:5] != bytes([0xAB, 0x01]):
            fail("dmodemd's JM_INFO: %r" % (jm,))
        c = gw.msg(CONNECT, 3)
        if not c or c[1] >> 2 != 4 or (c[3] << 8 | c[4]) != 33600:
            fail("dmodemd's CONNECT: %r" % (c,))
        gw.send(2, connect_msg(33600))
        data_both_ways(dte, gw)
        gw.stop = True
        print("in: modem relay as the answerer, ANSam sent, CONNECT 33600, "
              "data both ways", flush=True)
    finally:
        p.kill()
        p.wait()
        os.close(fd)

outgoing()
incoming()
print("PASS", flush=True)
