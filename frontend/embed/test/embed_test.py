#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# embed_test.py <embed_cat>: the modem of dmodem_embed.h in two programs
# (test/embed_cat.c), each on an anonymous SIP line on 127.0.0.1.
#
# 1. Each modem answers AT and ATI3 on its descriptor.
# 2. Modem A takes calls (-L, ATS0=1). Modem B dials it with
#    ATDsip:a@127.0.0.1:<A's port>. The two modems train and both give
#    CONNECT. Text that B sends after CONNECT comes out of A.
# 3. At the end of stdin, program A closes its descriptor, and program B
#    calls dmodem_embed_stop() (-s). Each modem stops with status 0
#    ("exit 0").
import os, random, socket, subprocess, sys, threading, time

embed_cat = sys.argv[1]

def fail(why, procs=()):
    """Print why the test failed and the stderr of each program, then
    exit with status 1."""
    print("FAIL:", why, flush=True)
    for p in procs:
        p.kill()
        print("--- stderr of", p.name, "---")
        sys.stdout.write(p.err[-6000:])
    sys.exit(1)

def free_port():
    """Return a UDP port on 127.0.0.1 that no program uses now."""
    for _ in range(50):
        port = 17200 + random.randrange(1000) * 2
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind(("127.0.0.1", port))
        except OSError:
            continue
        finally:
            s.close()
        return port
    fail("no free UDP port in 17200-19200")

def start(name, *args):
    """Start embed_cat with args. Two threads collect its stdout (p.out)
    and its stderr (p.err)."""
    p = subprocess.Popen([embed_cat, *args], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    p.name = name
    p.out = ""
    p.err = ""
    def pump(stream, attr):
        """Add the text of stream to the attribute attr of p, until the
        end of the stream."""
        while True:
            b = stream.read1(4096) if hasattr(stream, "read1") else stream.read(1)
            if not b:
                return
            setattr(p, attr, getattr(p, attr) + b.decode(errors="replace"))
    threading.Thread(target=pump, args=(p.stdout, "out"), daemon=True).start()
    threading.Thread(target=pump, args=(p.stderr, "err"), daemon=True).start()
    return p

def send(p, text):
    """Write text to the stdin of p (the DTE of its modem)."""
    p.stdin.write(text.encode())
    p.stdin.flush()

def wait_for(p, text, t, procs, since=0):
    """Wait t seconds at most for text in the output of p after the
    position since. Return the position after text, or fail."""
    end = time.time() + t
    while time.time() < end:
        if text in p.out[since:]:
            return p.out.index(text, since) + len(text)
        time.sleep(0.05)
    fail("%s: no %r in %.0f s; output %r" % (p.name, text, t, p.out[-400:]),
         procs)

port_a = free_port()
port_b = free_port()
# (-d 2: the modem log on stderr, so that a failure shows the cause)
a = start("A", "-p", str(port_a), "-L", "-d", "2", "-S", "4")
b = start("B", "-p", str(port_b), "-s", "-d", "2", "-S", "4")
procs = (a, b)

for p in procs:
    send(p, "AT\r")
    wait_for(p, "OK", 10, procs)
    send(p, "ATI3\r")
    wait_for(p, "OK", 10, procs)
print("AT and ATI3: OK on both")

since = len(a.out)
send(a, "ATS0=1\r")
wait_for(a, "OK", 10, procs, since)
send(b, "ATX3DTsip:a@127.0.0.1:%d\r" % port_a)
wait_for(b, "CONNECT", 90, procs)
wait_for(a, "CONNECT", 30, procs)
print("CONNECT on both:", b.out[b.out.index("CONNECT"):].split("\r")[0])
time.sleep(1)
since = len(a.out)
send(b, "hello from B\r")
wait_for(a, "hello from B", 20, procs, since)
print("data from B to A")

for p in procs:
    p.stdin.close()
for p in procs:
    try:
        p.wait(30)
    except subprocess.TimeoutExpired:
        fail("%s did not stop when its descriptor closed" % p.name, procs)
    time.sleep(0.2)
    if "exit 0" not in p.err:
        fail("%s: no 'exit 0'" % p.name, procs)
    if p.returncode != 0:
        fail("%s: exit status %d" % (p.name, p.returncode), procs)
print("PASS")
