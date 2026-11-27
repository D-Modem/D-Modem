#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
"""ami_events.py <port> <out>: log in to Asterisk's AMI on 127.0.0.1:<port>
(user test, secret test), send the action DModemFaxStats, and write each
event or response with DModem in it (an app_dmodem event, a UserEvent
of dmodem-ari, the response to the action: one block per event) to
<out> until the connection closes."""
import socket
import sys


def main():
    """Connect, log in, ask for the fax totals, and write the DModem
    events to the file until Asterisk closes the connection."""
    port, out = int(sys.argv[1]), sys.argv[2]
    for _ in range(40):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=600)
            break
        except OSError:
            import time
            time.sleep(0.5)
    else:
        sys.exit("no AMI on port %d" % port)
    s.sendall(b"Action: Login\r\nUsername: test\r\nSecret: test\r\n"
              b"Events: on\r\n\r\n"
              b"Action: DModemFaxStats\r\nActionID: stats1\r\n\r\n")
    buf = b""
    with open(out, "w") as f:
        while True:
            try:
                d = s.recv(4096)
            except OSError:
                break
            if not d:
                break
            buf += d
            while b"\r\n\r\n" in buf:
                block, buf = buf.split(b"\r\n\r\n", 1)
                text = block.decode("utf-8", "replace")
                if "DModem" in text or "stats1" in text:
                    f.write(text.replace("\r\n", "\n") + "\n\n")
                    f.flush()


if __name__ == "__main__":
    main()
