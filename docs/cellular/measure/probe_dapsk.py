# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# probe_dapsk.py: amplitude loading on the comb (clause sec:qam of the
# Recommendation).  Each tone carries, per symbol, a differential phase
# (DQPSK or 8-DPSK, as the combs do) and a differential amplitude: the
# tone's ring index steps by d (0 .. R-1, modulo R) from the last symbol,
# the rings `ratio_db` apart, so that the receiver decides d from the
# ratio of the tone's magnitudes in two consecutive symbols, as it
# decides the phase from their phase difference (no reference, no
# equalization: DAPSK).  The comb's power is that of the phase-only comb.
#
# Per codec and configuration: the tones whose phase and ring symbol
# error ratios are both below 2 per cent, the mean ring SER, the rms of
# the magnitude ratio's error (dB), and the raw rate of the best loading
# per tone (of DQPSK, 8-DPSK and the configuration's amplitude orders)
# against phase alone.
#
# Codec specs on stdin as probe_evs.py takes them; PROBE_FS the interface
# rate (8000, 16000, 32000 for the N, W and S combs), EVS_DIR, CODEC_PROBE;
# DAPSK_TONES=1 the per-tone lines that set the loading's thresholds.
import math
import os
import sys

import numpy as np

sys.argv = [sys.argv[0], os.environ.get("CODEC_PROBE", os.path.dirname(
    os.path.abspath(__file__)) + "/../../../build/dsp/test/codec_probe")]
src = open(os.path.dirname(os.path.abspath(__file__)) + "/probe.py").read()
src = src.replace("\nmain()\n", "\n")
exec(compile(src, "probe.py", "exec"))

GOOD = 0.02


def dapsk_comb(codec, f0, cycles, guard_ms, m, rings, ratio_db, n_sym=150):
    """the comb of tones k f0 (300 Hz .. the band's top) with m-DPSK and
    `rings` amplitude rings ratio_db apart; per tone the phase SER, the
    ring SER and the rms error of the magnitude ratio (dB)"""
    rng = np.random.default_rng(5)
    freqs = np.arange(math.ceil(300 / f0) * f0, TOP + 1e-6, f0)
    K = len(freqs)
    nu = int(round(FS * cycles / f0))
    ng = int(round(FS * guard_ms / 1000))
    sps = nu + ng
    dph = rng.integers(0, m, (n_sym, K))
    dr = rng.integers(0, rings, (n_sym, K)) if rings > 1 else np.zeros((n_sym, K), dtype=int)
    ph = np.cumsum(dph, axis=0) * 2 * np.pi / m
    ring = np.cumsum(dr, axis=0) % rings
    step = 10 ** (ratio_db / 20)
    a = step ** ring
    a = a / math.sqrt(np.mean(step ** (2 * np.arange(rings))))   # unit mean power
    tu = np.arange(nu) / FS
    x = np.zeros(n_sym * sps)
    for s_ in range(n_sym):
        u = np.zeros(nu)
        for k, f in enumerate(freqs):
            u += a[s_, k] * np.cos(2 * np.pi * f * tu + ph[s_, k])
        x[s_ * sps:s_ * sps + ng] = u[nu - ng:]
        x[s_ * sps + ng:(s_ + 1) * sps] = u
    x *= amp() * math.sqrt(2) / math.sqrt(K)
    lead = np.concatenate([x[:sps], x[:sps]])
    xx = np.concatenate([lead, x, np.zeros(FS // 4)])
    y, _ = through(codec, xx)
    y = y[len(lead):len(lead) + len(x)]
    ser_p, ser_a, err_db = [], [], []
    for k, f in enumerate(freqs):
        lo_ = np.exp(-2j * np.pi * f * tu)
        r = np.array([np.dot(y[s_ * sps + ng:(s_ + 1) * sps], lo_)
                      for s_ in range(n_sym)])
        dphi = np.angle(r[1:] * np.conj(r[:-1]))
        dec = np.round(dphi / (2 * np.pi / m)).astype(int) % m
        ser_p.append(np.count_nonzero(dec[5:] != dph[6:, k]) / (n_sym - 6))
        q = 20 * np.log10((np.abs(r[1:]) + 1e-9) / (np.abs(r[:-1]) + 1e-9))
        if rings > 1:
            # the ratio's steps, modulo the rings: a step up of d rings
            # is d * ratio_db, a wrap down (rings - d) of them
            want = dr[1:, k]
            exp_db = (ring[1:, k] - ring[:-1, k]) * ratio_db
            dd = np.round(q / ratio_db).astype(int) % rings
            ser_a.append(np.count_nonzero(dd[5:] != want[5:]) / (n_sym - 6))
            err_db.append(math.sqrt(np.mean((q[5:] - exp_db[5:]) ** 2)))
        else:
            ser_a.append(0.0)
            err_db.append(math.sqrt(np.mean(q[5:] ** 2)))
    return freqs, np.array(ser_p), np.array(ser_a), np.array(err_db), 1000.0 * sps / FS


def per_tone(c):
    """DAPSK_TONES=1: per tone, the magnitude ratio's rms change with
    phase alone (what the probing can measure) and whether 2 rings 6 dB
    apart and 4 rings 3 dB apart held (both SERs under GOOD)"""
    for f0, cyc, g in ((200, 3, 5.0), (300, 3, 10.0), (400, 6, 5.0)):
        _, _, _, e1, _ = dapsk_comb(c, f0, cyc, g, 8, 1, 0.0)
        _, sp2, sa2, _, _ = dapsk_comb(c, f0, cyc, g, 8, 2, 6.0)
        _, sp4, sa4, _, _ = dapsk_comb(c, f0, cyc, g, 8, 4, 3.0)
        for e, a, b in zip(e1, (sp2 < GOOD) & (sa2 < GOOD), (sp4 < GOOD) & (sa4 < GOOD)):
            print("tone %s %d %.3f %d %d" % (c, f0, e, a, b))
        sys.stdout.flush()


def main():
    if os.environ.get("DAPSK_TONES"):
        for c in sys.stdin.read().split():
            per_tone(c)
        return
    # the comb (spacing, cycles, prefix), and the orders to compare: phase
    # alone (DQPSK, 8-DPSK) and with 2 rings 6 dB apart, 2 rings 3 dB
    # apart, 4 rings 3 dB apart
    combs = [(200, 3, 5.0), (300, 3, 10.0), (400, 6, 5.0)]
    orders = [(4, 1, 0.0), (8, 1, 0.0), (4, 2, 6.0), (8, 2, 6.0), (8, 2, 3.0),
              (8, 4, 3.0)]
    for c in sys.stdin.read().split():
        print("=== %s (%d Hz)" % (c, FS))
        for f0, cyc, g in combs:
            best = None
            res = []
            for m, rings, rdb in orders:
                freqs, sp, sa, edb, Ts = dapsk_comb(c, f0, cyc, g, m, rings, rdb)
                ok = (sp < GOOD) & (sa < GOOD)
                bits = math.log2(m) + math.log2(rings)
                res.append((bits, ok))
                print("comb %3d Hz %d-DPSK %d ring%s%s: %2d tones, good %2d, "
                      "phase SER %.3f, ring SER %.3f, ratio error %.2f dB rms, "
                      "raw %5d bit/s" % (
                          f0, m, rings, "s" if rings > 1 else " ",
                          (" %.0f dB" % rdb) if rings > 1 else "     ",
                          len(freqs), np.count_nonzero(ok), sp.mean(), sa.mean(),
                          edb.mean(), int(bits * np.count_nonzero(ok) * 1000 / Ts)))
                sys.stdout.flush()
            # the best loading per tone: phase alone, and with amplitude
            ph_only = np.zeros(len(res[0][1]))
            with_a = np.zeros(len(res[0][1]))
            for bits, ok in res:
                with_a = np.maximum(with_a, np.where(ok, bits, 0))
            for bits, ok in res[:2]:
                ph_only = np.maximum(ph_only, np.where(ok, bits, 0))
            print("comb %3d Hz loaded per tone: phase alone %5d bit/s, with "
                  "amplitude %5d bit/s (%+.0f %%)" % (
                      f0, int(ph_only.sum() * 1000 / Ts), int(with_a.sum() * 1000 / Ts),
                      100 * (with_a.sum() / max(ph_only.sum(), 1) - 1)))
            sys.stdout.flush()


main()
