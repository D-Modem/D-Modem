#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# heard.py <file>: voice.sh's extension heard the caller: <file> (a WAV
# file, or raw 16 bit samples) has over half a second of sound, not
# silence.  Exit status 0 if so
import struct, sys, wave

if sys.argv[1].endswith(".wav"):
    w = wave.open(sys.argv[1])
    d = w.readframes(w.getnframes())
else:
    d = open(sys.argv[1], "rb").read()
x = struct.unpack("<%dh" % (len(d) // 2), d[:len(d) // 2 * 2])
rms = (sum(v * v for v in x) / max(1, len(x))) ** 0.5
print("voice: the extension heard %d samples, rms %.0f" % (len(x), rms))
sys.exit(0 if len(x) > 4000 and rms > 300 else 1)
