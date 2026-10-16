# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# probe_stack.py: the waveform layer and the parametric layer as one
# signal.  A symbol is one period of L samples, repeated to fill the
# symbol: the signal is then exactly periodic at L, so the codec's
# long-term predictor carries L (the parametric layer), and its spectrum
# is the harmonics of f0 = Fs/L, whose phases carry the data (the
# waveform layer).  The two do not have to be added and do not fight:
# the comb's spacing is the pitch.
#
# The data is differential across frequency, so that a symbol needs no
# reference to the symbol before it and L may change every symbol.  A
# timing offset of a fraction of a sample is a phase ramp across the
# tones, that is a constant in the differentials, which the receiver
# estimates by the m-th power of them; the first differential is a known
# reference, which resolves that estimate's m-fold ambiguity.
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

# the lag alphabet: none a multiple of another, so that a period is not
# read as its own; LAGS=<first>,<last>,<step> in the environment
_l = [int(v) for v in os.environ.get("LAGS", "40,70,2").split(",")]
LAGS = list(range(_l[0], _l[1] + 1, _l[2]))


def tones_of(L):
    """the harmonics of Fs/L inside the band"""
    f0 = FS / L
    return [k for k in range(1, int(TOP / f0) + 1) if k * f0 >= 300]


def symbol(L, data, m, nsamp):
    """one symbol: the period of L samples carrying the differentials in
    data on the harmonics of Fs/L, tiled over nsamp samples"""
    ks = tones_of(L)
    phi = [0.0]
    for d in data:
        phi.append(phi[-1] + 2 * math.pi * d / m)
    n = np.arange(L)
    per = np.zeros(L)
    for k, p in zip(ks, phi):
        per += np.cos(2 * math.pi * k * n / L + p)
    per /= math.sqrt(len(ks))
    return np.resize(per, nsamp)


def demod(y, a, b, L, m, n_ref=1):
    """the differentials of the symbol in y[a:b) at lag L: the tone
    phases over whole periods, their difference across frequency, the
    constant of a timing offset removed, the reference resolving the
    ambiguity"""
    ks = tones_of(L)
    nper = (b - a) // L
    if nper < 1:
        return None
    skip = L if nper >= 3 else 0         # the codec's transient
    w = (nper - (1 if nper >= 3 else 0)) * L
    seg = y[a + skip:a + skip + w]
    n = np.arange(w)
    r = np.array([np.dot(seg, np.exp(-2j * math.pi * k * n / L)) for k in ks])
    dphi = np.angle(r[1:] * np.conj(r[:-1]))
    off = np.angle(np.mean(np.exp(1j * m * dphi))) / m
    dec = np.round((dphi - off) / (2 * math.pi / m)).astype(int) % m
    # the first n_ref differentials are zero: whichever rotation of the
    # m-fold ambiguity makes them so is the right one
    if n_ref:
        dec = (dec - int(np.round(np.mean(dec[:n_ref])))) % m
    return dec


def run(codec, lag=None, m=4, sym_ms=20.0, n_sym=200, pitch=False):
    """a run at a fixed lag, or with the lag chosen per symbol (pitch);
    the tone symbol error ratio, the tones carrying data, the bits a
    symbol and the raw rate, and the pitch symbol error ratio"""
    rng = np.random.default_rng(19)
    sps = int(round(FS * sym_ms / 1000))
    seq = (rng.integers(0, len(LAGS), n_sym) if pitch else
           np.zeros(n_sym, dtype=int))
    lags = LAGS if pitch else [lag]
    x = np.zeros(n_sym * sps)
    data = []
    for s in range(n_sym):
        L = lags[seq[s]] if pitch else lag
        nd = len(tones_of(L)) - 1
        d = rng.integers(0, m, nd)
        d[0] = 0                         # the reference differential
        data.append(d)
        x[s * sps:(s + 1) * sps] = symbol(L, d, m, sps)
    x *= amp() * math.sqrt(2) / (np.sqrt(np.mean(x ** 2)) + 1e-9) * 0.7
    lead = x[:2 * sps]
    y, _ = through(codec, np.concatenate([lead, x, np.zeros(FS // 4)]))
    y = y[len(lead):len(lead) + len(x)]
    bad = tot = pbad = 0
    per_tone_bad = None
    for s in range(2, n_sym):
        L = lags[seq[s]] if pitch else lag
        if pitch:
            i = detect(y, s * sps, (s + 1) * sps, LAGS)
            pbad += i != seq[s]
            L = LAGS[i]
            if i != seq[s]:
                tot += len(data[s]) - 1      # the whole symbol is lost
                bad += len(data[s]) - 1
                continue
        dec = demod(y, s * sps, (s + 1) * sps, L, m)
        d = data[s]
        if per_tone_bad is None:
            per_tone_bad = np.zeros(len(d))
        e = dec[1:] != d[1:]
        bad += int(np.count_nonzero(e))
        tot += len(e)
        if len(per_tone_bad) == len(d):
            per_tone_bad[1:] += e
    nd = (sum(len(tones_of(L)) - 2 for L in LAGS) / len(LAGS) if pitch
          else len(tones_of(lag)) - 2)
    bits = nd * math.log2(m) + (math.log2(len(LAGS)) if pitch else 0)
    return (bad / max(tot, 1), nd, bits, int(bits * 1000 / sym_ms),
            pbad / (n_sym - 2))


def baseline(codec, f0=200.0, cycles=3, guard_ms=5.0, m=4, n_sym=200):
    """the comb of v_cvm.tex for comparison: random phases, differential
    in time, a cyclic prefix"""
    freqs, ser, rms, Ts = multitone(codec, f0=f0, cycles=cycles,
                                    guard_ms=guard_ms, m=m, n_sym=n_sym)
    good = int(np.count_nonzero(ser < 0.02))
    return (float(ser.mean()), good, len(freqs),
            int(math.log2(m) * good * 1000 / Ts))


def main():
    for c in sys.stdin.read().split():
        print("=== %s" % c)
        ser, good, K, rate = baseline(c)
        print("  comb 200 Hz, random phase, differential in time (v_cvm): "
              "SER %.3f, %d/%d tones, %4d bit/s" % (ser, good, K, rate))
        for L in (27, 32, 36, 40, 44, 50, 57, 70):
            ser, nd, bits, rate, _ = run(c, lag=L)
            print("  harmonic L=%2d (%5.1f Hz), %2d data tones: SER %.3f, "
                  "%4.1f bit/symbol, %4d bit/s" %
                  (L, FS / L, nd, ser, bits, rate))
            sys.stdout.flush()
        ser, nd, bits, rate, pser = run(c, pitch=True)
        print("  harmonic, lag per symbol (%d lags %d-%d): tone SER %.3f, "
              "pitch SER %.3f, %4.1f bit/symbol, %4d bit/s"
              % (len(LAGS), LAGS[0], LAGS[-1], ser, pser, bits, rate))
        sys.stdout.flush()


main()
