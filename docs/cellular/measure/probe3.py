# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# probe3.py: the design's own symbols: the 200 Hz comb with the 20 ms
# symbol (15 ms useful, 5 ms prefix) aligned to the codec frame and 10 ms
# off it, against the 22.5 ms symbol; more codec modes; two codecs in
# tandem.  PROBE_FS picks the interface rate (8000: 300-3400 Hz).
import math, os, sys
sys.argv = [sys.argv[0], os.environ.get("CODEC_PROBE", os.path.dirname(os.path.abspath(__file__)) + "/../../../build/dsp/test/codec_probe")]
src = open(os.path.dirname(os.path.abspath(__file__)) + "/probe.py").read()
src = src.replace("\nmain()\n", "\n")
exec(compile(src, "probe.py", "exec"))
codecs = sys.stdin.read().split()
for c in codecs:
    print("=== %s" % c)
    for f0, cyc, g, m, off in ((200, 3, 5.0, 4, 0), (200, 3, 5.0, 4, 10), (200, 3, 5.0, 8, 0),
                               (200, 4, 2.5, 4, 0), (200, 2, 2.5, 4, 0), (400, 6, 5.0, 4, 0),
                               (400, 6, 5.0, 8, 0), (300, 3, 10.0, 4, 0), (100, 3, 5.0, 4, 0)):
        freqs, ser, rms, Ts = multitone(c, f0=f0, cycles=cyc, guard_ms=g, m=m, offset_ms=off)
        good = np.count_nonzero(ser < 0.02)
        print("comb %3d Hz, %d-DPSK, %4.1f ms symbol (%4.1f useful, %3.1f prefix), offset %2d ms: %2d tones, "
              "mean SER %.3f, good %2d, rms %.1f deg, raw %4d bit/s, amp %.1f dB" % (
                  f0, m, Ts, 1000.0 * cyc / f0, g, off, len(freqs), ser.mean(), good, rms.mean(),
                  int(math.log2(m) * good * 1000 / Ts), multitone.amp_db.mean()))
    for baud in (300, 600):
        ser, rms = dpsk_carrier(c, baud, fc=1700)
        print("DQPSK fc 1700 Hz %4d baud: SER %.4f rms %.1f deg" % (baud, ser, rms))
    print("FSK BER: V.21 ch1 300 bd 980/1180: %.4f  V.21 ch2 300 bd 1650/1850: %.4f  Bell103 300 bd 2025/2225: %.4f" % (
        fsk(c, 300, 1180, 980), fsk(c, 300, 1850, 1650), fsk(c, 300, 2225, 2025)))
    sys.stdout.flush()
