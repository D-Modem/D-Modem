#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# anonymous_test.py <dmodemd>: SIP URIs dialled as they are, and the
# anonymous user agent (SIP_LOGIN=anonymous, no account), against this
# script as the far end: a SIP peer of its own on 127.0.0.1.
#
# Anonymous (-L): no REGISTER; ATDsip:bbs@127.0.0.1:<ours> sends its
# INVITE to us, from "Anonymous" <sip:anonymous@anonymous.invalid>; our
# 401 is not answered with credentials (it has none), and the call ends
# (NO CARRIER); ATD5551000 (a number, no account's domain to dial it at)
# ends at once with no INVITE.
# An account at another host: ATDbbs@127.0.0.1:<ours> (user@host) comes
# straight to us, from the account; our 401 gets no credentials (they
# are the account's host's, not ours).
#
# DMODEMD_DEBUG=<level>: dmodemd's -d (its log in $TMPDIR/anonymous.log)
import os, random, re, shutil, socket, subprocess, sys, tempfile
import threading, time, tty

dmodemd = sys.argv[1]
log = open(os.path.join(tempfile.gettempdir(), "anonymous.log"), "wb")

def fail(why):
    print("FAIL:", why, flush=True)
    log.flush()
    try:
        with open(log.name, "rb") as f:
            sys.stdout.write(f.read()[-6000:].decode(errors="replace"))
    except OSError:
        pass
    sys.exit(1)

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
def reply(req, code, text, a, extra=""):
    first, hdrs, _, _, head = req
    to = hdrs["to"] + ("" if "tag=" in hdrs["to"] or code == 100 else ";tag=far1")
    m = "SIP/2.0 %d %s\r\n" % (code, text) + "\r\n".join(vias(head)) + "\r\n" + \
        "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n%s" % (
            hdrs["from"], to, hdrs["call-id"], hdrs["cseq"], extra) + \
        "Content-Length: 0\r\n\r\n"
    sip_send(m, a)

def as_nobody():
    if os.geteuid() == 0:
        os.setgroups([])
        os.setgid(65534)
        os.setuid(65534)
def start_dmodemd(login, listen):
    env = dict(os.environ, SIP_LOGIN=login, SIP_PORT=str(DM_PORT))
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

ENDED = rb"NO CARRIER|BUSY|NO ANSWER|NO DIALTONE|ERROR"

# an INVITE for dial (none seen before it but what skip allows), our 401
# to it, no second INVITE with credentials, and the call's end at the DTE
def dial_401(dte, dial, check):
    dte.send(b"ATD" + dial + b"\r")
    while True:
        req = sip_recv(5)
        if not req:
            fail("no INVITE for %r" % dial)
        if req[0].startswith("REGISTER"):
            fail("a REGISTER with no account")
        if req[0].startswith("INVITE"):
            break
    check(req)
    reply(req, 401, "Unauthorized", req[3],
          'WWW-Authenticate: Digest realm="far", nonce="abc123", '
          'algorithm=MD5\r\n')
    end = time.time() + 2
    while time.time() < end:
        r = sip_recv(end - time.time())
        if not r:
            break
        if r[0].startswith("INVITE") and "authorization" in r[1]:
            fail("credentials given to a 401 they are not for: %r" % r[4])
    if not dte.expect(ENDED, 5):
        fail("the call did not end at the DTE: %r" % dte.buf)

def anonymous():
    p, fd = start_dmodemd("anonymous", True)
    dte = Dte(fd)
    try:
        dte.send(b"ATE0\r")
        if not dte.expect(rb"OK", 3):
            fail("no OK: %r" % dte.buf)
        def check(req):
            first, hdrs = req[0], req[1]
            if first.split()[1] != "sip:bbs@127.0.0.1:%d" % SIP_PORT:
                fail("the INVITE's URI: %r" % first)
            if "anonymous@anonymous.invalid" not in hdrs["from"] or \
               "Anonymous" not in hdrs["from"]:
                fail("the INVITE's From: %r" % hdrs["from"])
        dial_401(dte, b"sip:bbs@127.0.0.1:%d" % SIP_PORT, check)
        dte.send(b"ATD5551000\r")
        if not dte.expect(ENDED, 5):
            fail("a number dialled with no account did not end: %r" % dte.buf)
        r = sip_recv(1)
        if r and r[0].startswith("INVITE"):
            fail("an INVITE for a number with no account: %r" % r[0])
        print("anonymous: no REGISTER, INVITE from anonymous@anonymous.invalid,"
              " no credentials, a number refused", flush=True)
    finally:
        p.kill()
        p.wait()
        os.close(fd)

def account_elsewhere():
    # (the account's registrar: a port nothing answers)
    p, fd = start_dmodemd("dm:secret@127.0.0.1:9", False)
    dte = Dte(fd)
    try:
        dte.send(b"ATE0\r")
        if not dte.expect(rb"OK", 3):
            fail("no OK: %r" % dte.buf)
        def check(req):
            first, hdrs = req[0], req[1]
            if first.split()[1] != "sip:bbs@127.0.0.1:%d" % SIP_PORT:
                fail("the INVITE's URI: %r" % first)
            if "sip:dm@127.0.0.1:9" not in hdrs["from"]:
                fail("the INVITE's From: %r" % hdrs["from"])
        dial_401(dte, b"bbs@127.0.0.1:%d" % SIP_PORT, check)
        print("account: user@host dialled as it is, the account's"
              " credentials kept from it", flush=True)
    finally:
        p.kill()
        p.wait()
        os.close(fd)

anonymous()
account_elsewhere()
