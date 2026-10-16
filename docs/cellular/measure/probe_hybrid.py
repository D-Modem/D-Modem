# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# probe_hybrid.py: the two layers at once.  The transmitted signal is the
# comb of probe3.py (tones with differential phase modulation, the
# waveform layer) added to the pulse train of probe_pitch.py (the codec's
# pitch lag, the parametric layer), in one 20 ms symbol.  Prints, per
# power split, what each layer delivers: whether the parametric layer is
# free throughput on top of the waveform layer, or takes from it.
#
# Codec specs on stdin.  PROBE_FS, EVS_DIR, CODEC_PROBE as probe_pitch.py.
import math
import os
import sys

sys.argv = [sys.argv[0], os.environ.get("CODEC_PROBE", os.path.dirname(
    os.path.abspath(__file__)) + "/../../../build/dsp/test/codec_probe")]
_d = os.path.dirname(os.path.abspath(__file__))
exec(compile(open(_d + "/probe.py").read().replace("\nmain()\n", "\n"),
             "probe.py", "exec"))
_p = open(_d + "/probe_pitch.py").read()
exec(compile(_p[_p.index("def pulse_shape"):_p.index("def main()")],
             "probe_pitch.py", "exec"))


def comb_symbols(freqs, n_sym, nu, ng, m, rng):
    """the comb's samples and the symbols it carries"""
    syms = rng.integers(0, m, (n_sym, len(freqs)))
    ph = np.cumsum(syms, axis=0) * 2 * np.pi / m
    tu = np.arange(nu) / FS
    sps = nu + ng
    x = np.zeros(n_sym * sps)
    for s in range(n_sym):
        u = np.zeros(nu)
        for k, f in enumerate(freqs):
            u += np.cos(2 * np.pi * f * tu + ph[s, k])
        x[s * sps:s * sps + ng] = u[nu - ng:]
        x[s * sps + ng:(s + 1) * sps] = u
    return x / math.sqrt(len(freqs)), syms, tu


def hybrid(codec, f0=200.0, cycles=3, guard_ms=5.0, m=4, lags=None,
           frac=0.3, n_sym=200):
    """the comb and the pitch channel in one signal, frac of the power in
    the pulse train; the tone symbol error ratio, the raw rate of the
    good tones, the pitch symbol error ratio and its rate"""
    lags = lags or list(range(32, 64))
    rng = np.random.default_rng(13)
    freqs = np.arange(math.ceil(300 / f0) * f0, TOP + 1e-6, f0)
    nu, ng = int(round(FS * cycles / f0)), int(round(FS * guard_ms / 1000))
    sps = nu + ng
    comb, syms, tu = comb_symbols(freqs, n_sym, nu, ng, m, rng)
    pseq = rng.integers(0, len(lags), n_sym)
    pitch = pitch_signal(pseq, lags, sps, True)
    comb = comb / (np.sqrt(np.mean(comb ** 2)) + 1e-9)
    pitch = pitch / (np.sqrt(np.mean(pitch ** 2)) + 1e-9)
    x = (math.sqrt(1 - frac) * comb + math.sqrt(frac) * pitch)
    x *= amp() * math.sqrt(2) / (np.sqrt(np.mean(x ** 2)) + 1e-9) * 0.7
    lead = x[:2 * sps]
    y, _ = through(codec, np.concatenate([lead, x, np.zeros(FS // 4)]))
    y = y[len(lead):len(lead) + len(x)]
    # the waveform layer
    good, sers = 0, []
    for k, f in enumerate(freqs):
        lo = np.exp(-2j * np.pi * f * tu)
        r = np.array([np.dot(y[s * sps + ng:(s + 1) * sps], lo)
                      for s in range(n_sym)])
        dphi = np.angle(r[1:] * np.conj(r[:-1]))
        dec = np.round(dphi / (2 * np.pi / m)).astype(int) % m
        ser = np.count_nonzero(dec[5:] != syms[6:, k]) / (n_sym - 6)
        sers.append(ser)
        good += ser < 0.02
    # the parametric layer
    bad = sum(detect(y, s * sps, (s + 1) * sps, lags) != pseq[s]
              for s in range(2, n_sym))
    tone_rate = int(math.log2(m) * good * FS / sps)
    return (float(np.mean(sers)), good, len(freqs), tone_rate,
            bad / (n_sym - 2),
            int(math.log2(len(lags)) * FS / sps))


def main():
    for c in sys.stdin.read().split():
        print("=== %s" % c)
        for frac in (0.0, 0.15, 0.3, 0.5, 1.0):
            ser, good, K, trate, pser, prate = hybrid(c, frac=frac)
            tot = trate + (prate if pser < 0.02 else 0)
            print("pitch share %3d%%: tones mean SER %.3f, good %2d/%2d, "
                  "%4d bit/s | pitch SER %.3f, %3d bit/s | together %4d bit/s"
                  % (int(frac * 100), ser, good, K, trate, pser, prate, tot))
            sys.stdout.flush()


main()
