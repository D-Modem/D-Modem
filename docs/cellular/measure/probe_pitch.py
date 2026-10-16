# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# probe_pitch.py: a parametric channel on the speech codec's own pitch
# (long-term predictor) lag.  The transmitter sends a band-limited pulse
# train whose period is the symbol; an ACELP codec's adaptive codebook
# follows the period, so the decoded signal keeps it, and the receiver
# recovers the symbol by autocorrelation of the decoded samples.  Neither
# end touches the codec's bitstream: the channel rides what the codec
# must transmit to describe the signal at all.
#
# The shortest symbol here is 10 ms (two of the codec's 5 ms subframes).
# A symbol per subframe needs a detector whose window is longer than the
# symbol, since two periods of a lag in the codec's range do not fit in
# 40 samples; that is left open.
#
# Prints, per codec and configuration, the symbol error ratio, how far the
# wrong decisions fall from the symbol sent (a Gray code over the lag
# alphabet makes near misses one bit each), and the raw bit rate.
#
# Codec specs on stdin, as probe3.py takes them ("amr:12.2", "evs:13200",
# "amr:12.2@1" for erasures, "amr:12.2+amr:12.2" for a tandem).
# PROBE_FS: the signal interface rate.  EVS_DIR: the EVS reference code.
# CODEC_PROBE: the codec_probe program.
import math
import os
import sys

sys.argv = [sys.argv[0], os.environ.get("CODEC_PROBE", os.path.dirname(
    os.path.abspath(__file__)) + "/../../../build/dsp/test/codec_probe")]
src = open(os.path.dirname(os.path.abspath(__file__)) + "/probe.py").read()
src = src.replace("\nmain()\n", "\n")
exec(compile(src, "probe.py", "exec"))


def pulse_shape():
    """one band-limited pulse: the band the codec carries"""
    n = 33 * FS // 8000
    k = np.arange(n) - n // 2
    def sinc(fc):
        return np.where(k == 0, 2 * fc / FS,
                        np.sin(2 * np.pi * fc / FS * k) /
                        (np.pi * np.where(k == 0, 1, k)))
    return (sinc(TOP) - sinc(300)) * np.hamming(n)


def pitch_signal(seq, lags, sps, tilt):
    """a pulse train: in symbol s the pulses are lags[seq[s]] apart, the
    first at the symbol's start (both ends know the symbol clock).  tilt
    gives the pulses a glottal-like spectral slope, which makes the
    signal look voiced to the codec's classifier."""
    n = len(seq) * sps
    exc = np.zeros(n)
    for s, i in enumerate(seq):
        L = lags[i]
        for p in range(s * sps, (s + 1) * sps, L):
            exc[p] = 1.0
    if tilt:
        # a one-pole low pass: -6 dB/octave, as a glottal pulse has
        b = np.zeros(n)
        acc = 0.0
        for i in range(n):
            acc = exc[i] + 0.7 * acc
            b[i] = acc
        exc = b
    x = np.convolve(exc, pulse_shape(), mode="same")
    return x * amp() * math.sqrt(2) / (np.sqrt(np.mean(x ** 2)) + 1e-9) * 0.5


def detect(y, a, b, lags):
    """the lag of the strongest normalized self-similarity of y[a:b);
    the smallest lag within 2 per cent of it, so that a true period is
    not mistaken for its own multiple"""
    best, cs = -2.0, []
    for L in lags:
        if (b - a) - L < 16:
            cs.append(-2.0)
            continue
        x1, x2 = y[a:b - L], y[a + L:b]
        d = math.sqrt(float(np.dot(x1, x1)) * float(np.dot(x2, x2))) + 1e-9
        c = float(np.dot(x1, x2)) / d
        cs.append(c)
        best = max(best, c)
    for i, c in enumerate(cs):
        if c >= best - 0.02:
            return i
    return int(np.argmax(cs))


def pitch_channel(codec, lags, sym_ms, n_sym=200, offset_ms=0.0, tilt=True):
    """n_sym symbols of the pitch channel through the codec: the symbol
    error ratio, the mean distance of a wrong decision from the symbol
    sent (in alphabet steps), and the raw bit rate"""
    rng = np.random.default_rng(11)
    sps = int(round(FS * sym_ms / 1000))
    seq = rng.integers(0, len(lags), n_sym)
    x = pitch_signal(seq, lags, sps, tilt)
    lead = np.concatenate([np.zeros(int(round(FS * offset_ms / 1000))),
                           pitch_signal(np.zeros(2, dtype=int), lags, sps,
                                        tilt)])
    xx = np.concatenate([lead, x, np.zeros(FS // 4)])
    y, _ = through(codec, xx)
    y = y[len(lead):len(lead) + len(x)]
    bad, dist = 0, 0
    for s in range(2, n_sym):          # the first symbols prime the codec
        d = detect(y, s * sps, (s + 1) * sps, lags)
        if d != seq[s]:
            bad += 1
            dist += abs(int(d) - int(seq[s]))
    n = n_sym - 2
    bits = math.log2(len(lags))
    return (bad / n, dist / max(bad, 1), bits * 1000.0 / sym_ms)


def main():
    # the configurations: the lag alphabet (samples at 8000 Hz, inside
    # AMR's 20 to 143), the symbol period, and whether the symbols are
    # aligned to the codec's 20 ms frame
    cfgs = [
        ("8 lags 40-96 step 8", list(range(40, 97, 8)), 20.0, 0.0),
        ("16 lags 40-70 step 2", list(range(40, 71, 2)), 20.0, 0.0),
        ("32 lags 32-63", list(range(32, 64)), 20.0, 0.0),
        ("32 lags 32-63", list(range(32, 64)), 10.0, 0.0),
        ("16 lags 20-35", list(range(20, 36)), 10.0, 0.0),
        ("8 lags 40-96 step 8", list(range(40, 97, 8)), 20.0, 10.0),
    ]
    for c in sys.stdin.read().split():
        print("=== %s" % c)
        for name, lags, ms, off in cfgs:
            n_sym = max(120, int(4000 / ms))
            ser, dist, rate = pitch_channel(c, lags, ms, n_sym, off)
            print("pitch %-22s %4.1f ms symbol%s: SER %.3f, mean miss "
                  "%.1f steps, raw %4d bit/s" %
                  (name, ms, ", +10 ms" if off else "       ", ser, dist,
                   int(rate)))
            sys.stdout.flush()


main()
