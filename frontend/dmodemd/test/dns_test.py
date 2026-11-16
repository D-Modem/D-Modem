#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# dns_test.py <dmodemd>: a host's name found as RFC 3263 has it, against
# this script's own DNS server (SIP_DNS) and SIP peer (UDP and TCP) on
# 127.0.0.1.
#
# naptr.test: NAPTR (SIP+D2U) -> SRV _sip._udp -> our UDP port.
# srv.test: no NAPTR, SRV _sip._udp -> our UDP port.
# tcp.test: no NAPTR, SRV _sip._tcp -> our TCP port (SIP over TCP).
# Each dialled with no account (SIP_LOGIN=anonymous, ATDsip:bbs@<host>):
# the INVITE must come to the port the records name, after the queries
# RFC 3263 has (NAPTR first), and our 486 end the call at the DTE.
# An account at srv.test (-L) registers there, by its SRV record.
#
# DMODEMD_DEBUG=<level>: dmodemd's -d (its log in $TMPDIR/dns.log)
import os, random, re, shutil, socket, struct, subprocess, sys, tempfile
import threading, time, tty

dmodemd = sys.argv[1]
log = open(os.path.join(tempfile.gettempdir(), "dns.log"), "wb")

def fail(why):
    print("FAIL:", why, flush=True)
    log.flush()
    try:
        with open(log.name, "rb") as f:
            sys.stdout.write(f.read()[-6000:].decode(errors="replace"))
    except OSError:
        pass
    sys.exit(1)

# our SIP port (UDP and TCP), dmodemd's after it, both free
def free_ports():
    for _ in range(50):
        port = 17200 + random.randrange(500) * 2
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        t = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        pu = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        pt = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            u.bind(("127.0.0.1", port))
            t.bind(("127.0.0.1", port))
            pu.bind(("127.0.0.1", port + 1))
            pt.bind(("127.0.0.1", port + 1))
        except OSError:
            u.close()
            t.close()
            continue
        finally:
            pu.close()
            pt.close()
        t.listen(4)
        return u, t, port
    fail("no free UDP and TCP ports in 17200-18200")

sip, sip_tcp, SIP_PORT = free_ports()
DM_PORT = SIP_PORT + 1

# --- the DNS server: the records below, nothing else (an empty answer)
def name(n):
    return b"".join(bytes([len(l)]) + l.encode() for l in n.split(".") if l) + b"\0"
def cstr(s):
    return bytes([len(s)]) + s.encode()
T_A, T_SRV, T_NAPTR = 1, 33, 35
A = socket.inet_aton("127.0.0.1")
def srv(target, port):
    return struct.pack(">HHH", 0, 0, port) + name(target)
def naptr(services, replace):
    return struct.pack(">HH", 10, 10) + cstr("s") + cstr(services) + \
        cstr("") + name(replace)
# (name, type): [answers], [additionals]; each (name, type, rdata)
ZONE = {
    ("naptr.test", T_NAPTR): ([("naptr.test", T_NAPTR,
                               naptr("SIP+D2U", "_sip._udp.naptr.test"))], []),
    ("_sip._udp.naptr.test", T_SRV): (
        [("_sip._udp.naptr.test", T_SRV, srv("sip1.naptr.test", SIP_PORT))],
        [("sip1.naptr.test", T_A, A)]),
    ("_sip._udp.srv.test", T_SRV): (
        [("_sip._udp.srv.test", T_SRV, srv("sip1.srv.test", SIP_PORT))],
        [("sip1.srv.test", T_A, A)]),
    ("_sip._tcp.tcp.test", T_SRV): (
        [("_sip._tcp.tcp.test", T_SRV, srv("sip1.tcp.test", SIP_PORT))],
        [("sip1.tcp.test", T_A, A)]),
}
queries = []
qlock = threading.Lock()
dns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
dns.bind(("127.0.0.1", 0))
DNS_PORT = dns.getsockname()[1]
def rr(n, t, rdata):
    return name(n) + struct.pack(">HHIH", t, 1, 60, len(rdata)) + rdata
def dns_serve():
    while True:
        d, a = dns.recvfrom(4096)
        qid, flags = struct.unpack(">HH", d[:4])
        i, labels = 12, []
        while d[i]:
            labels.append(d[i + 1:i + 1 + d[i]].decode())
            i += 1 + d[i]
        qtype = struct.unpack(">H", d[i + 1:i + 3])[0]
        q = ".".join(labels).lower()
        with qlock:
            queries.append((q, qtype))
        log.write(b"DNS? %s %d\n" % (q.encode(), qtype)); log.flush()
        ans, add = ZONE.get((q, qtype), ([], []))
        body = d[12:i + 5] + b"".join(rr(*x) for x in ans) + \
            b"".join(rr(*x) for x in add)
        dns.sendto(struct.pack(">HHHHHH", qid, 0x8180, 1, len(ans), 0,
                               len(add)) + body, a)
threading.Thread(target=dns_serve, daemon=True).start()
def asked():
    with qlock:
        q = list(queries)
        queries.clear()
    return q

# --- SIP: a message over UDP, or over TCP (on its connection)
def parse(d):
    head, _, body = d.decode(errors="replace").partition("\r\n\r\n")
    lines = head.split("\r\n")
    hdrs = {}
    for l in lines[1:]:
        n, _, v = l.partition(":")
        hdrs.setdefault(n.strip().lower(), v.strip())
    log.write(b"<<< " + d + b"\n"); log.flush()
    return lines[0], hdrs, body, head
def udp_recv(t):
    sip.settimeout(t)
    try:
        d, a = sip.recvfrom(65535)
    except socket.timeout:
        return None
    return parse(d) + (a,)
def tcp_recv(conn, buf, t):
    conn.settimeout(t)
    while b"\r\n\r\n" not in buf[0]:
        try:
            d = conn.recv(65535)
        except socket.timeout:
            return None
        if not d:
            return None
        buf[0] += d
    head, _, rest = buf[0].partition(b"\r\n\r\n")
    m = re.search(rb"(?im)^(?:content-length|l)\s*:\s*(\d+)", head)
    n = int(m.group(1)) if m else 0
    while len(rest) < n:
        rest += conn.recv(65535)
    buf[0] = rest[n:]
    return parse(head + b"\r\n\r\n" + rest[:n])
def vias(head):
    return [l for l in head.split("\r\n")[1:] if re.match(r"(?i)via\s*:", l)]
def response(req, code, text, extra=""):
    first, hdrs, _, head = req[:4]
    to = hdrs["to"] + ("" if "tag=" in hdrs["to"] else ";tag=far1")
    return ("SIP/2.0 %d %s\r\n" % (code, text) + "\r\n".join(vias(head)) +
            "\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n%s"
            "Content-Length: 0\r\n\r\n" % (hdrs["from"], to, hdrs["call-id"],
                                          hdrs["cseq"], extra)).encode()

def as_nobody():
    if os.geteuid() == 0:
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)
def start_dmodemd(login, listen):
    env = dict(os.environ, SIP_LOGIN=login, SIP_PORT=str(DM_PORT),
               SIP_DNS="127.0.0.1:%d" % DNS_PORT)
    env.pop("SIP_TCP", None)
    pre = ["stdbuf", "-oL", "-eL"] if shutil.which("stdbuf") else []
    p = subprocess.Popen(pre + [dmodemd,
                          "-d" + os.environ.get("DMODEMD_DEBUG", "1")] +
                         (["-L"] if listen else []),
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         env=env, preexec_fn=as_nobody)
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
ENDED = rb"BUSY|NO CARRIER|NO ANSWER|ERROR"

# the INVITE for sip:bbs@<host> over UDP: its URI, the queries before it
def dial_udp(dte, host, want):
    asked()
    dte.send(b"ATDsip:bbs@%s\r" % host.encode())
    while True:
        r = udp_recv(5)
        if not r:
            fail("%s: no INVITE over UDP; DNS asked %r" % (host, asked()))
        if r[0].startswith("INVITE"):
            break
    if r[0].split()[1] != "sip:bbs@" + host:
        fail("%s: the INVITE's URI: %r" % (host, r[0]))
    q = asked()
    if q[:len(want)] != want:
        fail("%s: DNS asked %r, not %r first" % (host, q, want))
    sip.sendto(response(r, 486, "Busy Here"), r[4])
    if not dte.expect(ENDED, 5):
        fail("%s: the call did not end: %r" % (host, dte.buf))
    print("%s: %s, INVITE over UDP to our port" % (host, ", ".join(
        "%s %s" % ({T_NAPTR: "NAPTR", T_SRV: "SRV"}[t], n) for n, t in want)),
        flush=True)

def anonymous():
    p, fd = start_dmodemd("anonymous", False)
    dte = Dte(fd)
    try:
        dte.send(b"ATE0\r")
        if not dte.expect(rb"OK", 3):
            fail("no OK: %r" % dte.buf)
        dial_udp(dte, "naptr.test", [("naptr.test", T_NAPTR),
                                     ("_sip._udp.naptr.test", T_SRV)])
        dial_udp(dte, "srv.test", [("srv.test", T_NAPTR),
                                   ("_sip._tcp.srv.test", T_SRV),
                                   ("_sip._udp.srv.test", T_SRV)])
        # over TCP
        asked()
        dte.send(b"ATDsip:bbs@tcp.test\r")
        sip_tcp.settimeout(5)
        try:
            conn, _ = sip_tcp.accept()
        except socket.timeout:
            fail("tcp.test: no TCP connection; DNS asked %r" % asked())
        buf = [b""]
        r = tcp_recv(conn, buf, 5)
        if not r or not r[0].startswith("INVITE"):
            fail("tcp.test: no INVITE over TCP: %r" % (r,))
        if "SIP/2.0/TCP" not in r[1].get("via", ""):
            fail("tcp.test: the INVITE's Via: %r" % r[1].get("via"))
        q = asked()
        if q[:2] != [("tcp.test", T_NAPTR), ("_sip._tcp.tcp.test", T_SRV)]:
            fail("tcp.test: DNS asked %r" % q)
        conn.sendall(response(r, 486, "Busy Here"))
        if not dte.expect(ENDED, 5):
            fail("tcp.test: the call did not end: %r" % dte.buf)
        conn.close()
        print("tcp.test: NAPTR tcp.test, SRV _sip._tcp.tcp.test, INVITE over "
              "TCP to our port", flush=True)
    finally:
        p.kill()
        p.wait()
        os.close(fd)

def account():
    p, fd = start_dmodemd("dm:secret@srv.test", True)
    try:
        while True:
            r = udp_recv(5)
            if not r:
                fail("no REGISTER at srv.test's SRV; DNS asked %r" % asked())
            if r[0].startswith("REGISTER"):
                break
        if r[0].split()[1] != "sip:srv.test":
            fail("the REGISTER's URI: %r" % r[0])
        sip.sendto(response(r, 200, "OK", "Contact: %s;expires=600\r\n" %
                            r[1].get("contact", "")), r[4])
        print("account: REGISTER to srv.test's SRV record", flush=True)
    finally:
        p.kill()
        p.wait()
        os.close(fd)

anonymous()
account()
