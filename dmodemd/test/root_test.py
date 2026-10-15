#!/usr/bin/env python3
# root_test.py <dmodemd>: dmodemd started as root goes on as nobody,
# and what needs root's rights still works after: the data file is saved
# at the end, the link in /dev follows the pty made anew when the DTE
# hangs up, and goes when dmodemd ends, even killed (a process of its
# own, the link's keeper, keeps root's rights for it alone).  Not root:
# skipped (77).
#
# The modem is dmodem<n> for an n with no link yet (from 50): its
# link /dev/ttyDM<n>, its data file /var/lib/dmodem/data.dmodem<n>,
# removed after (the directory too if made here).  No SIP server: the
# modem neither calls nor registers.
import os
import grp
import re
import signal
import socket
import stat
import subprocess
import sys
import termios
import threading
import time
import tty

if os.geteuid() != 0:
    print("SKIP: not root")
    sys.exit(77)
dmodemd = sys.argv[1]
NOBODY = 65534
try:
    dialout = grp.getgrnam("dialout").gr_gid
except KeyError:
    dialout = None

n = next(n for n in range(50, 100) if not os.path.lexists("/dev/ttyDM%d" % n))
dev, link = "dmodem%d" % n, "/dev/ttyDM%d" % n
datadir = "/var/lib/dmodem"
data = "%s/data.dmodem%d" % (datadir, n)
made_dir = not os.path.isdir(datadir)
if made_dir:
    os.makedirs(datadir, 0o755)
if os.path.exists(data):
    os.unlink(data)

failed = []


def check(ok, what):
    print("%s: %s" % ("ok" if ok else "FAIL", what), flush=True)
    if not ok:
        failed.append(what)
    return ok


class Dmodemd:
    """dmodemd on dev, its output gathered; ready() once it runs as
    nobody"""

    def __init__(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        env = dict(os.environ, SIP_LOGIN="test:test@127.0.0.1:9",
                   SIP_PORT=str(port))
        self.p = subprocess.Popen(["stdbuf", "-oL", "-eL", dmodemd, "-n", dev],
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, env=env)
        self.out = b""
        self.lock = threading.Condition()
        threading.Thread(target=self.gather, daemon=True).start()

    def gather(self):
        for l in self.p.stdout:
            sys.stdout.write("  dmodemd: " + l.decode(errors="replace"))
            with self.lock:
                self.out += l
                self.lock.notify_all()

    # the match of pat in the output, its count'th; None after t s
    def wait(self, pat, t=10, count=1):
        end = time.time() + t
        with self.lock:
            while True:
                m = list(re.finditer(pat, self.out))
                if len(m) >= count:
                    return m[count - 1]
                if time.time() >= end:
                    return None
                self.lock.wait(end - time.time())

    def links(self, count):
        m = self.wait(rb"symbolic link `(\S+)' -> `(\S+)' created", count=count)
        return (m.group(1).decode(), m.group(2).decode()) if m else (None, None)

    def ready(self):
        return self.wait(rb"Use `\S+' as modem device") is not None


def uids(pid):
    with open("/proc/%d/status" % pid) as f:
        for l in f:
            if l.startswith("Uid:"):
                return [int(u) for u in l.split()[1:]]


def children(pid):
    r = []
    for d in os.listdir("/proc"):
        try:
            with open("/proc/%s/stat" % d) as f:
                st = f.read()
        except (OSError, ValueError):
            continue
        if int(st[st.rindex(")") + 2:].split()[1]) == pid:
            r.append(int(d))
    return r


def gone(pid):
    try:
        with open("/proc/%d/stat" % pid) as f:
            st = f.read()
        return st[st.rindex(")") + 2] in "ZX"
    except OSError:
        return True


# the slave as dmodemd's defaults have it: group dialout, mode 0660
def check_slave(path):
    st = os.stat(path) if path else None
    check(st is not None and (dialout is None or st.st_gid == dialout) and
          stat.S_IMODE(st.st_mode) == 0o660,
          "%s: group dialout, mode 0660 (%s)" %
          (path, st and "%d, %o" % (st.st_gid, stat.S_IMODE(st.st_mode))))


# the link opened as a DTE would, raw and with HUPCL (a pty's slave has
# none of its own): closed, it hangs up; the modem's OK to AT
def at_ok(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    t = termios.tcgetattr(fd)
    t[2] |= termios.HUPCL
    termios.tcsetattr(fd, termios.TCSANOW, t)
    os.write(fd, b"AT\r")
    buf, end = b"", time.time() + 5
    while b"OK\r\n" not in buf and time.time() < end:
        try:
            buf += os.read(fd, 256)
        except BlockingIOError:
            time.sleep(0.05)
    return fd, b"OK\r\n" in buf


try:
    # a pty of our own below dmodemd's: freed before the hangup, its
    # number is the new pty's
    own = os.openpty()
    m = Dmodemd()
    l, pts = m.links(1)
    check(l == link, "the link is %s (%s)" % (link, l))
    check(m.ready(), "dmodemd runs")
    check(uids(m.p.pid) == [NOBODY] * 4, "dmodemd runs as nobody")
    keepers = children(m.p.pid)
    check(len(keepers) == 1 and uids(keepers[0])[1] == 0,
          "one child of dmodemd's, root's: the keeper")
    keeper = keepers[0] if keepers else None
    if keeper:
        fds = sorted(int(f) for f in os.listdir("/proc/%d/fd" % keeper))
        check(fds == [0, 1, 2, 3] and
              all(os.readlink("/proc/%d/fd/%d" % (keeper, f)) == "/dev/null"
                  for f in (0, 1, 2)),
              "the keeper has only its socket (and /dev/null): %s" % fds)
    check(os.path.islink(link) and os.readlink(link) == pts,
          "%s -> %s" % (link, pts))
    check_slave(pts)
    fd, ok = at_ok(link)
    check(ok, "OK to AT through the link")
    ino = os.lstat(link).st_ino
    os.close(own[0])
    os.close(own[1])
    os.close(fd)                # HUPCL: a hangup, a new pty
    l2, pts2 = m.links(2)
    check(pts2 is not None and pts2 != pts, "a new pty after the hangup: %s" % pts2)
    check(os.path.islink(link) and os.readlink(link) == pts2 and
          os.lstat(link).st_ino != ino, "the link made anew: %s -> %s" % (link, pts2))
    check_slave(pts2)
    fd, ok = at_ok(link)
    check(ok, "OK to AT through the new link")
    os.close(fd)
    m.links(3)
    m.p.send_signal(signal.SIGTERM)
    check(m.p.wait(10) == 0, "dmodemd ends on SIGTERM")
    check(not os.path.lexists(link), "the link is gone")
    check(keeper is None or gone(keeper), "the keeper is gone")
    st = os.stat(data) if os.path.exists(data) else None
    check(st is not None and st.st_size > 0 and st.st_uid == 0 and
          stat.S_IMODE(st.st_mode) == 0o600,
          "the data file saved (root's, 0600): %s" % data)

    # killed: the keeper removes the link
    m = Dmodemd()
    check(m.ready() and os.path.islink(link), "dmodemd runs again, the link made")
    keepers = children(m.p.pid)
    m.p.kill()
    m.p.wait(10)
    end = time.time() + 5
    while os.path.lexists(link) and time.time() < end:
        time.sleep(0.05)
    check(not os.path.lexists(link), "the link is gone after SIGKILL")
    while keepers and not gone(keepers[0]) and time.time() < end:
        time.sleep(0.05)
    check(len(keepers) == 1 and gone(keepers[0]), "the keeper is gone after SIGKILL")
finally:
    if os.path.islink(link) and os.readlink(link).startswith("/dev/pts/"):
        os.unlink(link)
    if os.path.exists(data):
        os.unlink(data)
    if made_dir:
        os.rmdir(datadir)

print("FAIL: %d" % len(failed) if failed else "PASS")
sys.exit(1 if failed else 0)
