# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# probe.py <codec_probe> <codec spec>...: what a speech codec does to
# modem-like signals, through codec_probe (16 bit, 8000 Hz).  Each
# measurement generates a signal, runs it through the codec and analyses
# the output aligned to the input (the codec's delay found by
# correlation).  Prints one block per codec; "none" is the codec_probe
# bypass (the signal as it is), for the measurement's own floor.
import math
import os
import subprocess
import sys

import numpy as np

FS = int(os.environ.get("PROBE_FS", "8000"))
TOP = {8000: 3400, 16000: 7000, 32000: 14000}[FS]       # the band's top
PROBE = sys.argv[1]
EVS = os.environ.get("EVS_DIR", "")   # the 3GPP EVS reference code (EVS_cod, EVS_dec)
LEVEL_DBFS = -12.0


def through(codec, x):
    """x (float, 16 bit scale) through the codec; aligned output and the
    delay found (samples)"""
    xi = np.clip(np.round(x), -32768, 32767).astype("<i2")
    if codec == "none":
        y = xi.astype(float)
        return y, 0
    data = xi.tobytes()
    for stage in codec.split("+"):
        spec, _, loss = stage.partition("@")
        if spec.startswith("evs"):
            data = evs_stage(spec, float(loss or 0), data)
            continue
        p = subprocess.run([PROBE, spec, loss or "0", "7", str(FS)], input=data,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        data = p.stdout
    y = np.frombuffer(data, dtype="<i2").astype(float)
    # the delay: the lag of the output's best correlation with the input
    # over a middle stretch, 0 to 400 samples
    seg = slice(len(x) // 4, len(x) // 4 + min(16000, len(x) // 2))
    xs = x[seg]
    best, bd = -1e300, 0
    for d in range(0, 1600):
        if seg.stop + d > len(y):
            break
        c = np.dot(xs, y[seg.start + d:seg.stop + d])
        if c > best:
            best, bd = c, d
    ya = y[bd:bd + len(x)]
    if len(ya) < len(x):
        ya = np.concatenate([ya, np.zeros(len(x) - len(ya))])
    return ya, bd


def evs_stage(spec, loss, data):
    """data (16 bit samples at FS) through the EVS reference encoder and
    decoder: spec is evs:<bit/s>[:nb|wb|swb|fb][:dtx] (the primary
    modes) or evsio:<bit/s> (AMR-WB IO); loss erases that percentage of
    the G.192 frames (their sync word made the bad frame's)"""
    import tempfile
    parts = spec.split(":")
    rate = parts[1]
    opts = ["-q"]
    for o in parts[2:]:
        if o in ("nb", "wb", "swb", "fb"):
            opts += ["-max_band", o.upper()]
        elif o == "dtx":
            opts += ["-dtx"]
    with tempfile.TemporaryDirectory() as d:
        inp, bs, out = d + "/in.raw", d + "/bs.192", d + "/out.raw"
        open(inp, "wb").write(data)
        subprocess.run([EVS + "/EVS_cod"] + opts + [rate, str(FS // 1000), inp, bs],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if loss > 0:
            w = np.frombuffer(open(bs, "rb").read(), dtype="<u2").copy()
            rng = np.random.default_rng(7)
            i = 0
            while i < len(w):
                n = int(w[i + 1])
                if rng.random() < loss / 100:
                    w[i] = 0x6B20
                i += 2 + n
            open(bs, "wb").write(w.tobytes())
        subprocess.run([EVS + "/EVS_dec", "-q", str(FS // 1000), bs, out],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return open(out, "rb").read()


def amp():
    return 32767 * 10 ** (LEVEL_DBFS / 20)


def snr_db(ref, out):
    """SNR of out against ref, with the best gain"""
    g = np.dot(ref, out) / np.dot(ref, ref)
    e = out - g * ref
    return 10 * math.log10(np.sum((g * ref) ** 2) / max(np.sum(e ** 2), 1e-9)), g


def tone_sweep(codec):
    """a steady tone at each frequency: its SNR after the codec"""
    out = []
    for f in range(300, TOP + 100, 200 if FS == 8000 else 400 if FS == 16000 else 800):
        n = 2 * FS
        t = np.arange(n) / FS
        x = amp() * np.sin(2 * np.pi * f * t)
        y, d = through(codec, x)
        s, g = snr_db(x[FS // 2:-FS // 4], y[FS // 2:-FS // 4])
        out.append((f, s, 20 * math.log10(abs(g) + 1e-9)))
    return out


def tone_levels(codec):
    """a 1000 Hz tone at several levels: SNR against level"""
    out = []
    for lvl in (-3, -6, -12, -18, -24, -30):
        n = 2 * FS
        t = np.arange(n) / FS
        x = 32767 * 10 ** (lvl / 20) * np.sin(2 * np.pi * 1000 * t)
        y, d = through(codec, x)
        s, g = snr_db(x[FS // 2:-FS // 4], y[FS // 2:-FS // 4])
        out.append((lvl, s))
    return out


def dpsk_carrier(codec, baud, fc=1800.0, m=4, n_sym=600):
    """M-ary DPSK on one carrier at baud symbols/s (no pulse shaping
    beyond a rectangular symbol): the symbol error rate after the codec,
    differential detection with a matched correlator over the symbol"""
    rng = np.random.default_rng(1)
    sps = FS / baud
    syms = rng.integers(0, m, n_sym)
    ph = np.cumsum(syms) * 2 * np.pi / m
    n = int(round(n_sym * sps))
    t = np.arange(n) / FS
    idx = np.minimum((np.arange(n) / sps).astype(int), n_sym - 1)
    x = amp() * np.cos(2 * np.pi * fc * t + ph[idx])
    # lead-in and tail of carrier, so that the codec settles
    lead = amp() * np.cos(2 * np.pi * fc * np.arange(FS // 2) / FS)
    xx = np.concatenate([lead, x, lead])
    y, d = through(codec, xx)
    y = y[len(lead):len(lead) + n]
    # correlate each symbol against the carrier (complex), then the
    # phase difference between symbols
    lo = np.exp(-2j * np.pi * fc * t)
    z = y * lo
    r = np.array([z[int(round(k * sps)) + int(sps * 0.15):
                   int(round((k + 1) * sps)) - int(sps * 0.15)].sum()
                  for k in range(n_sym)])
    dphi = np.angle(r[1:] * np.conj(r[:-1]))
    dec = np.round(dphi / (2 * np.pi / m)).astype(int) % m
    err = np.count_nonzero(dec[20:] != syms[21:])
    # the EVM-like phase noise: deviation from the ideal phase step
    ideal = syms[21:] * 2 * np.pi / m
    dev = np.angle(np.exp(1j * (dphi[20:] - ideal)))
    return err / len(dec[20:]), math.degrees(np.sqrt(np.mean(dev ** 2)))


def fsk(codec, baud, f0, f1, n_bits=800):
    """binary FSK (continuous phase) at baud: bit error rate after the
    codec, with a non-coherent (two-filter, matched over the bit) detector"""
    rng = np.random.default_rng(2)
    bits = rng.integers(0, 2, n_bits)
    sps = FS / baud
    n = int(round(n_bits * sps))
    idx = np.minimum((np.arange(n) / sps).astype(int), n_bits - 1)
    f = np.where(bits[idx] == 1, f1, f0)
    ph = 2 * np.pi * np.cumsum(f) / FS
    x = amp() * np.cos(ph)
    lead = amp() * np.cos(2 * np.pi * f0 * np.arange(FS // 2) / FS)
    xx = np.concatenate([lead, x, lead])
    y, d = through(codec, xx)
    y = y[len(lead):len(lead) + n]
    t = np.arange(n) / FS
    e0 = y * np.exp(-2j * np.pi * f0 * t)
    e1 = y * np.exp(-2j * np.pi * f1 * t)
    dec = []
    for k in range(n_bits):
        a, b = int(round(k * sps)) + int(sps * 0.1), int(round((k + 1) * sps)) - int(sps * 0.1)
        dec.append(1 if abs(e1[a:b].sum()) > abs(e0[a:b].sum()) else 0)
    dec = np.array(dec)
    return np.count_nonzero(dec[10:] != bits[10:]) / (n_bits - 10)


def multitone(codec, f0=100.0, lo=300, hi=None, cycles=2, guard_ms=2.5,
              m=4, n_sym=150, offset_ms=0.0):
    """DQPSK on the tones k * f0 between lo and hi, as a multicarrier
    symbol: a useful period of cycles / f0 (the tones orthogonal over
    it) after a cyclic prefix of guard_ms; the phases change from one
    symbol to the next.  Per tone the symbol error rate and the rms
    phase error after the codec."""
    rng = np.random.default_rng(3)
    hi = TOP if hi is None else hi
    freqs = np.arange(math.ceil(lo / f0) * f0, hi + 1e-6, f0)
    K = len(freqs)
    nu = int(round(FS * cycles / f0))
    ng = int(round(FS * guard_ms / 1000))
    sps = nu + ng
    syms = rng.integers(0, m, (n_sym, K))
    ph = np.cumsum(syms, axis=0) * 2 * np.pi / m
    tu = np.arange(nu) / FS
    x = np.zeros(n_sym * sps)
    for s_ in range(n_sym):
        u = np.zeros(nu)
        for k, f in enumerate(freqs):
            u += np.cos(2 * np.pi * f * tu + ph[s_, k])
        x[s_ * sps:s_ * sps + ng] = u[nu - ng:]
        x[s_ * sps + ng:(s_ + 1) * sps] = u
    x *= amp() * math.sqrt(2) / math.sqrt(K)  # the same power as one tone
    lead = np.concatenate([np.zeros(int(round(FS * offset_ms / 1000))), x[:sps], x[:sps]])
    xx = np.concatenate([lead, x, np.zeros(FS // 4)])
    y, d = through(codec, xx)
    y = y[len(lead):len(lead) + len(x)]
    ser = []
    rms = []
    amp_db = []
    for k, f in enumerate(freqs):
        lo_ = np.exp(-2j * np.pi * f * tu)
        r = np.array([np.dot(y[s_ * sps + ng:(s_ + 1) * sps], lo_)
                      for s_ in range(n_sym)])
        dphi = np.angle(r[1:] * np.conj(r[:-1]))
        dec = np.round(dphi / (2 * np.pi / m)).astype(int) % m
        ser.append(np.count_nonzero(dec[5:] != syms[6:, k]) / (n_sym - 6))
        dev = np.angle(np.exp(1j * (dphi[5:] - syms[6:, k] * 2 * np.pi / m)))
        rms.append(math.degrees(np.sqrt(np.mean(dev ** 2))))
        a = np.abs(r[5:]) + 1e-9
        amp_db.append(np.sqrt(np.mean((20 * np.log10(a[1:] / a[:-1])) ** 2)))
    multitone.amp_db = np.array(amp_db)
    return freqs, np.array(ser), np.array(rms), 1000.0 * sps / FS


def pitch_symbols(codec, lags=(40, 48, 56, 64, 72, 80, 88, 96), sym_ms=20,
                  n_sym=200):
    """a periodic pulse train (an impulse every lag samples, low-pass
    shaped) whose period changes every symbol: the period the codec's
    output has, by autocorrelation, against the one sent"""
    rng = np.random.default_rng(4)
    sps = int(FS * sym_ms / 1000)
    choice = rng.integers(0, len(lags), n_sym)
    x = np.zeros(n_sym * sps)
    pos = 0
    for s in range(n_sym):
        L = lags[choice[s]]
        end = (s + 1) * sps
        while pos < end:
            x[pos] = 1.0
            pos += L
    # shape: a 300-3400 Hz band pulse (difference of sincs), so that the
    # pulses have the band of speech
    n = 33 * FS // 8000
    k = np.arange(n) - n // 2
    def sinc(fc):
        return np.where(k == 0, 2 * fc / FS, np.sin(2 * np.pi * fc / FS * k) / (np.pi * np.where(k == 0, 1, k)))
    h = (sinc(TOP) - sinc(300)) * np.hamming(n)
    x = np.convolve(x, h, mode="same")
    x *= amp() * 2 / np.sqrt(np.mean(x ** 2)) * 0.35
    y, d = through(codec, x)
    y = np.concatenate([y, np.zeros(2 * max(lags))])
    errs = 0
    for s in range(3, n_sym):
        seg = y[s * sps:(s + 1) * sps + max(lags)]
        best, bl = -1e300, 0
        for L in lags:
            c = np.dot(seg[:sps], seg[L:L + sps])
            if c > best:
                best, bl = c, L
        errs += bl != lags[choice[s]]
    return errs / (n_sym - 3)


def main():
    codecs = sys.argv[2:]
    for c in codecs:
        print("=== %s" % c)
        print("tone sweep (Hz: SNR dB, gain dB):")
        print("  " + "  ".join("%d:%.1f/%+.1f" % r for r in tone_sweep(c)))
        print("1000 Hz by level (dBFS: SNR dB): " +
              "  ".join("%d:%.1f" % r for r in tone_levels(c)))
        print("DQPSK 1800 Hz by baud (SER, rms phase err deg):")
        for baud in (50, 100, 200, 300, 600, 1200):
            ser, rms = dpsk_carrier(c, baud)
            print("  %5d baud: SER %.4f  rms %.1f deg" % (baud, ser, rms))
        print("8-DPSK 1800 Hz by baud:")
        for baud in (50, 100, 200, 600):
            ser, rms = dpsk_carrier(c, baud, m=8)
            print("  %5d baud: SER %.4f  rms %.1f deg" % (baud, ser, rms))
        print("FSK BER: V.21 ch2 300 bd 1650/1850: %.4f   V.23 1200 bd 1300/2100: %.4f"
              "   Bell103 300 bd 2025/2225: %.4f" % (
                  fsk(c, 300, 1850, 1650), fsk(c, 1200, 2100, 1300),
                  fsk(c, 300, 2225, 2025)))
        for f0, cyc, m in ((100, 2, 4), (100, 1, 4), (200, 2, 4), (200, 4, 4),
                           (50, 1, 4), (100, 2, 8), (200, 2, 8), (100, 4, 4)):
            freqs, ser, rms, Ts = multitone(c, f0=f0, cycles=cyc, m=m)
            good = np.count_nonzero(ser < 0.02)
            print("multitone f0 %d Hz, %d-DPSK, %.1f ms symbols: %d tones, mean SER %.3f, "
                  "tones with SER<2%%: %d, mean rms phase err %.1f deg, "
                  "raw %d bit/s (good tones)" % (
                      f0, m, Ts, len(freqs), ser.mean(), good, rms.mean(),
                      int(math.log2(m) * good * 1000 / Ts)))
            if f0 == 100 and cyc == 2 and m == 4:
                print("   per tone SER: " + " ".join("%d:%.2f" % (f, s_) for f, s_ in zip(freqs, ser)))
                print("   per tone rms deg: " + " ".join("%d:%.0f" % (f, r) for f, r in zip(freqs, rms)))
        print("pitch symbols (8 lags, 20 ms): symbol error rate %.3f" % pitch_symbols(c))
        sys.stdout.flush()


main()
