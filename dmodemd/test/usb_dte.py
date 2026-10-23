#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
# usb_dte.py - what usb_test.py and pico_line_test.py share to run a
# Windows dmodemd.exe as well as Linux's dmodemd on the fake USB modems:
#
# - a Windows dmodemd (<dmodemd> a .exe) runs under Wine, built with the
#   fake in libusb's place (dmodemd_fake_st7554.exe,
#   dmodemd_fake_pico_line.exe: no LD_PRELOAD there), its DTE a TCP
#   connection (--tcp) instead of a pty;
# - Wine's Winsock has no Unix sockets, so each line's socket of the
#   exchange is relayed from TCP (Relay), the fake given tcp:<host>:<port>
#   (test/fake_net.h).
import os, re, select, socket, threading, tty


def windows(dmodemd):
    """a Windows dmodemd: run under Wine"""
    return dmodemd.endswith(".exe")


def command(dmodemd):
    """the command that runs dmodemd (Wine's for a .exe)"""
    if windows(dmodemd):
        os.environ.setdefault("WINEDEBUG", "-all")
        return ["wine", dmodemd]
    return [dmodemd]


def free_port():
    """a TCP port of 127.0.0.1's nobody has now"""
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def dte_args(dmodemd):
    """dmodemd's DTE options: none (its pty), or a TCP port of its own"""
    if windows(dmodemd):
        return ["--tcp", "127.0.0.1:%d" % free_port()]
    return []


class Relay:
    """TCP on 127.0.0.1 to the exchange's Unix socket at path: each
    connection to it a connection there, both ways"""
    def __init__(self, path):
        self.path = path
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(8)
        self.where = "tcp:127.0.0.1:%d" % self.srv.getsockname()[1]
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            u = socket.socket(socket.AF_UNIX)
            try:
                u.connect(self.path)
            except OSError:
                c.close()
                continue
            for a, b in ((c, u), (u, c)):
                threading.Thread(target=self.pump, args=(a, b),
                                 daemon=True).start()

    @staticmethod
    def pump(a, b):
        """a's octets to b until either ends (then both end)"""
        try:
            while True:
                d = a.recv(65536)
                if not d:
                    break
                b.sendall(d)
        except OSError:
            pass
        for s in (a, b):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


_relays = {}


def file_for(dmodemd, path):
    """a file at path as the fake is given it: the path, or (Windows,
    under Wine) on its Z: drive"""
    return "Z:" + path if windows(dmodemd) else path


def socket_for(dmodemd, path):
    """what the fake is given for the exchange's socket at path: the
    path, or (Windows) a relay's tcp:<host>:<port>"""
    if not windows(dmodemd):
        return path
    if path not in _relays:
        _relays[path] = Relay(path)
    return _relays[path].where


class Dte:
    """the modem's DTE as the test talks to it: dmodemd's pty, or its TCP
    port (from the line of its output that names it)"""
    def __init__(self, pty=None, tcp=None):
        self.sock = None
        if tcp:
            self.sock = socket.create_connection(tcp, timeout=10)
            self.sock.setblocking(False)
        else:
            self.fd = os.open(pty, os.O_RDWR | os.O_NOCTTY)
            tty.setraw(self.fd)

    @staticmethod
    def find(line):
        """(pty, tcp) a line of dmodemd's output names, else None"""
        m = re.search(rb"TTY is `(\S+)'", line)
        if m:
            return (m.group(1).decode(), None)
        m = re.search(rb"DTE is TCP (\S+):(\d+)", line)
        if m:
            return (None, (m.group(1).decode(), int(m.group(2))))
        return None

    def fileno(self):
        return self.sock.fileno() if self.sock else self.fd

    def write(self, b):
        if self.sock:
            self.sock.setblocking(True)
            self.sock.sendall(b)
            self.sock.setblocking(False)
        else:
            os.write(self.fd, b)

    def read(self, n):
        """what is there ("" at its end raises OSError)"""
        if self.sock:
            try:
                d = self.sock.recv(n)
            except BlockingIOError:
                return b""
            if not d:
                raise OSError("the DTE's connection ended")
            return d
        return os.read(self.fd, n)

    def wait(self, t):
        """readable within t s"""
        r, _, _ = select.select([self], [], [], t)
        return bool(r)
