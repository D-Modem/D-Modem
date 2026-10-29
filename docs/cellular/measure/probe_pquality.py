# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# probe_pquality.py: the parametric mode's normalized autocorrelation as a
# measure of the codec's mode (clause sec:retrain of the Recommendation:
# whether it can replace the periodic retrain of a DCE in the parametric
# mode).  The P20 alphabet's symbols through each codec; per codec the
# symbol error ratio and, over the symbols, the decided lag's normalized
# autocorrelation c (its mean, its 10th percentile and its spread over
# 2 s windows), the margin over the next lag's, and the most tones a
# narrowband comb loads through the same codec (whether the DCE would
# leave the parametric mode for a comb there).
#
# Codec specs on stdin; PROBE_FS (8000), EVS_DIR, CODEC_PROBE as
# probe_pitch.py.
import math
import os
import sys

import numpy as np

sys.argv = [sys.argv[0], os.environ.get("CODEC_PROBE", os.path.dirname(
    os.path.abspath(__file__)) + "/../../../build/dsp/test/codec_probe")]
src = open(os.path.dirname(os.path.abspath(__file__)) + "/probe_pitch.py").read()
src = src.replace("\nmain()\n", "\n")
exec(compile(src, "probe_pitch.py", "exec"))

LAGS = list(range(40, 71, 2))


def corr(y, a, b, L):
    x1, x2 = y[a:b - L], y[a + L:b]
    d = math.sqrt(float(np.dot(x1, x1)) * float(np.dot(x2, x2))) + 1e-9
    return float(np.dot(x1, x2)) / d


def quality(codec, n_sym=300):
    rng = np.random.default_rng(13)
    sps = FS * 20 // 1000
    lags = [L * FS // 8000 for L in LAGS]
    seq = rng.integers(0, len(lags), n_sym)
    x = pitch_signal(seq, lags, sps, True)
    lead = pitch_signal(np.zeros(2, dtype=int), lags, sps, True)
    y, _ = through(codec, np.concatenate([lead, x, np.zeros(FS // 4)]))
    y = y[len(lead):len(lead) + len(x)]
    cs, ms, bad = [], [], 0
    for s in range(2, n_sym):
        a, b = s * sps, (s + 1) * sps
        c = np.array([corr(y, a, b, L) for L in lags])
        d = detect(y, a, b, lags)
        bad += d != seq[s]
        cs.append(c[d])
        o = np.delete(c, d)
        ms.append(c[d] - o.max())
    cs, ms = np.array(cs), np.array(ms)
    win = [cs[i:i + 100].mean() for i in range(0, len(cs) - 99, 50)]   # 2 s
    return bad / (n_sym - 2), cs.mean(), np.percentile(cs, 10), \
        (max(win) - min(win)) if win else 0.0, ms.mean()


def comb_tones(codec):
    """the most tones a narrowband comb (N200, N300, N400 with DQPSK and
    the 20 ms symbol) loads through the codec: a tone symbol error ratio
    under 2 per cent (the loading's four tones decide between the comb
    and the parametric mode)"""
    best = 0
    for f0, cyc, g in ((200, 3, 5.0), (300, 3, 10.0), (400, 6, 5.0)):
        _, ser, _, _ = multitone(codec, f0=f0, cycles=cyc, guard_ms=g, m=4)
        best = max(best, int(np.count_nonzero(ser < 0.02)))
    return best


def main():
    for c in sys.stdin.read().split():
        ser, cm, c10, spread, mm = quality(c)
        print("%-14s P20 SER %.3f  c mean %.3f  c 10%% %.3f  2 s spread %.3f  "
              "margin %.3f  comb tones %d" % (c, ser, cm, c10, spread, mm,
                                              comb_tones(c)))
        sys.stdout.flush()


main()
