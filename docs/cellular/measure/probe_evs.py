# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# probe_evs.py: the signal set through the EVS reference codec (slow: the
# fixed-point reference encoder), a shorter set of measurements than
# probe3.py: the three combs with DQPSK and 8-DPSK at the 20 ms symbol,
# the 200 Hz comb at 22.5 ms, one carrier at 600 baud, FSK.  Codec specs
# on stdin ("evs:24400", "evs:13200:wb", "evsio:23850", "evs:24400@1");
# PROBE_FS the interface rate, EVS_DIR the reference code.
import math, os, sys
sys.argv = [sys.argv[0], os.environ.get("CODEC_PROBE", os.path.dirname(os.path.abspath(__file__)) + "/../../../build/dsp/test/codec_probe")]
src = open(os.path.dirname(os.path.abspath(__file__)) + "/probe.py").read()
src = src.replace("\nmain()\n", "\n")
exec(compile(src, "probe.py", "exec"))
for c in sys.stdin.read().split():
    print("=== %s" % c)
    for f0, cyc, g, m in ((200, 3, 5.0, 4), (200, 3, 5.0, 8), (300, 3, 10.0, 4), (300, 3, 10.0, 8),
                          (400, 6, 5.0, 4), (400, 6, 5.0, 8), (200, 4, 2.5, 4)):
        freqs, ser, rms, Ts = multitone(c, f0=f0, cycles=cyc, guard_ms=g, m=m)
        good = np.count_nonzero(ser < 0.02)
        print("comb %3d Hz, %d-DPSK, %4.1f ms symbol (%4.1f useful, %3.1f prefix), offset  0 ms: %2d tones, "
              "mean SER %.3f, good %2d, rms %.1f deg, raw %4d bit/s, amp %.1f dB" % (
                  f0, m, Ts, 1000.0 * cyc / f0, g, len(freqs), ser.mean(), good, rms.mean(),
                  int(math.log2(m) * good * 1000 / Ts), multitone.amp_db.mean()))
        sys.stdout.flush()
    ser, rms = dpsk_carrier(c, 600, fc=1700)
    print("DQPSK fc 1700 Hz  600 baud: SER %.4f rms %.1f deg" % (ser, rms))
    print("FSK BER: V.21 ch1 300 bd 980/1180: %.4f  V.21 ch2 300 bd 1650/1850: %.4f  Bell103 300 bd 2025/2225: %.4f" % (
        fsk(c, 300, 1180, 980), fsk(c, 300, 1850, 1650), fsk(c, 300, 2225, 2025)))
    sys.stdout.flush()
