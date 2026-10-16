# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# probe_nb2.py: more of the design space on the codecs: tone combs with
# other spacings (with and without integer-period tones), DQPSK at other
# carriers and symbol rates, 8-DPSK
import math
import os
import sys

sys.argv = [sys.argv[0], os.environ.get("CODEC_PROBE", os.path.dirname(os.path.abspath(__file__)) + "/../../../build/dsp/test/codec_probe")]
src = open(os.path.dirname(os.path.abspath(__file__)) + "/probe.py").read()
src = src.replace("\nmain()\n", "\n")
exec(compile(src, "probe.py", "exec"))

for c in ("amr:12.2", "amr:7.4", "amr:4.75", "amrwb:12.65", "amrwb:23.85"):
    print("=== %s" % c)
    for f0, cyc, m in ((100, 2, 4), (110, 2, 4), (90, 2, 4), (200, 2, 4),
                       (190, 2, 4), (210, 2, 4), (150, 3, 4), (250, 5, 4),
                       (200, 3, 4), (400, 8, 4), (400, 8, 8), (300, 6, 4),
                       (300, 6, 8), (200, 4, 8)):
        freqs, ser, rms, Ts = multitone(c, f0=f0, cycles=cyc, m=m)
        good = np.count_nonzero(ser < 0.02)
        print("multitone f0 %3d Hz, %d-DPSK, %4.1f ms symbols: %2d tones, "
              "mean SER %.3f, good tones %2d, rms phase err %.1f deg, "
              "raw %4d bit/s (good tones)" % (
                  f0, m, Ts, len(freqs), ser.mean(), good, rms.mean(),
                  int(math.log2(m) * good * 1000 / Ts)))
    for fc in (1200, 1700, 2100, 2400):
        for baud in (300, 600, 1200):
            ser, rms = dpsk_carrier(c, baud, fc=fc)
            print("DQPSK fc %d Hz %4d baud: SER %.4f rms %.1f deg" %
                  (fc, baud, ser, rms))
    for baud in (300, 600):
        ser, rms = dpsk_carrier(c, baud, fc=1700, m=8)
        print("8-DPSK fc 1700 Hz %4d baud: SER %.4f rms %.1f deg" %
              (baud, ser, rms))
    sys.stdout.flush()
