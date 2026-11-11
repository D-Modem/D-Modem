#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# speech.py <file>: 2 s of a voice-like sound (a pitch moving 100 to 250
# Hz, its harmonics to 3.5 kHz), 16 bit at 8000 Hz, into <file>: a WAV
# file if its name ends in .wav, else raw samples (Asterisk's .sln), for
# voice.sh's caller
import math, struct, sys, wave

ph, out = 0.0, []
for i in range(16000):
    t = i / 8000.0
    pitch = 175 + 75 * math.sin(2 * math.pi * t / 0.8)
    ph += 2 * math.pi * pitch / 8000
    x, k = 0.0, 1
    while k * pitch < 3500:
        x += 3000.0 / k * math.sin(k * ph)
        k += 1
    out.append(max(-32767, min(32767, int(x))))
data = struct.pack("<%dh" % len(out), *out)
if sys.argv[1].endswith(".wav"):
    w = wave.open(sys.argv[1], "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(8000)
    w.writeframes(data)
    w.close()
else:
    open(sys.argv[1], "wb").write(data)
