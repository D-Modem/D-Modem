# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# tables.py <matrix.txt> <probe.txt> <probe3.txt> <probe3_wb.txt>
#           <probe_evs8.txt> <probe_evs16.txt> <probe_evs32.txt> <cross.tex>
#           <pitch.txt> <pitch_evs.txt> <stack.txt> <dapsk_nb.txt>
#           <dapsk_wb.txt> <dapsk_swb.txt> <pquality.txt> > measured.tex
#
# The tables of Appendices I and II of v_cvm.tex from the measurement logs
# (measure/logs/, made by the scripts of measure/): matrix.txt from
# matrix.sh (D-Modem's modulations through each codec, interop_call),
# probe.txt and probe3.txt from probe.py and probe3.py at the 8000 Hz
# interface, probe3_wb.txt from probe3.py at 16000 Hz, probe_evs*.txt
# from probe_evs.py at 8000, 16000 and 32000 Hz; cross.tex has the rows
# of the cross-check table; pitch.txt and pitch_evs.txt from
# probe_pitch.py (the parametric mode) and stack.txt from probe_stack.py
# (the two layers in one waveform), all at 8000 Hz; dapsk_*.txt from
# probe_dapsk.py (amplitude loading) at 8000, 16000 and 32000 Hz and
# pquality.txt from probe_pquality.py (the parametric mode's
# autocorrelation against the codec's mode).  Prints LaTeX.
import re
import sys


def esc(s):
    return s.replace("%", "\\%").replace("_", "\\_")


def codec_name(c):
    c = c.strip()
    loss = ""
    m = re.match(r"(\S+) LOSS=(\d+)", c)
    if m:
        c, loss = m.group(1), m.group(2)
    stages = []
    for st in c.split("+"):
        spec, _, l = st.partition("@")
        if l:
            loss = l
        if spec == "none":
            stages.append("none (G.711)")
            continue
        name, _, rate = spec.partition(":")
        rate = rate.split(":")[0] if name != "evs" else rate
        if name == "evs":
            r, *rest = rate.split(":") if ":" in rate else (rate,)
            stages.append("EVS %g" % (int(r) / 1000.0))
        elif name == "evsio":
            stages.append("EVS AMR-WB IO %g" % (int(rate) / 1000.0))
        else:
            stages.append(("AMR-WB " if name == "amrwb" else "AMR ") + rate)
    s = " + ".join(stages)
    if loss and loss != "0":
        s += ", %s%% of the frames erased" % loss
    return s


def matrix(path):
    """Table: modulation x codec: the CONNECT and whether the data came
    through"""
    rows = {}
    order = []
    mods = []
    for line in open(path, errors="replace"):
        parts = line.rstrip("\n").split()
        if len(parts) < 3 or not re.match(r"(none|amr|amrwb|evs)", parts[0]):
            continue
        # codec may be two tokens ("amr:12.2 LOSS=1")
        if parts[1].startswith("LOSS="):
            codec, ms, rest = parts[0] + " " + parts[1], parts[2], parts[3:]
        else:
            codec, ms, rest = parts[0], parts[1], parts[2:]
        if not ms.isdigit():
            continue
        text = " ".join(rest)
        if codec not in rows:
            rows[codec] = {}
            order.append(codec)
        if ms not in mods:
            mods.append(ms)
        m = re.search(r"CONNECT (\d+)", text)
        rate = m.group(1) if m else None
        passed = text.rstrip().endswith("PASS")
        both_true = text.count(":True") == 2
        if rate is None:
            cell = "--"
        elif passed or both_true:
            cell = "\\textbf{%s}" % rate
        else:
            cell = "%s\\textsuperscript{e}" % rate
        rows[codec][ms] = cell
    names = {"21": "V.21", "103": "Bell 103", "23": "V.23", "22": "V.22",
             "212": "Bell 212A", "122": "V.22bis", "32": "V.32",
             "132": "V.32bis", "34": "V.34"}
    out = []
    out.append("\\begin{tabular}{l%s}" % ("c" * len(mods)))
    out.append("\\toprule")
    out.append("\\rowcolor{ituhead}\\thd{Codec} & " +
               " & ".join("\\thd{%s}" % names.get(m, m) for m in mods) + " \\\\")
    out.append("\\midrule")
    for codec in order:
        out.append(esc(codec_name(codec)) + " & " +
                   " & ".join(rows[codec].get(m, "") for m in mods) + " \\\\")
    out.append("\\bottomrule")
    out.append("\\end{tabular}")
    return "\n".join(out)


def parse_probe(path):
    """{codec: [lines]}"""
    blocks = {}
    cur = None
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("=== "):
            cur = line[4:].strip()
            blocks[cur] = []
        elif cur is not None and line and line != "done":
            blocks[cur].append(line)
    return blocks


def fsk_table(blocks):
    """FSK bit error ratios by codec"""
    out = ["\\begin{tabular}{lccc}", "\\toprule",
           "\\rowcolor{ituhead}\\thd{Codec} & \\thd{V.21 ch.\\,1} & \\thd{V.21 ch.\\,2} & \\thd{Bell 103} \\\\",
           "\\midrule"]
    for c, lines in blocks.items():
        for l in lines:
            m = re.match(r"FSK BER: V.21 ch1 300 bd 980/1180: ([\d.]+)\s+V.21 ch2 300 bd 1650/1850: ([\d.]+)\s+Bell103 300 bd 2025/2225: ([\d.]+)", l)
            if m:
                vals = [float(v) for v in m.groups()]
                out.append(esc(codec_name(c)) + " & " +
                           " & ".join(("$<10^{-4}$" if v < 1e-4 else "%.4f" % v) for v in vals) + " \\\\")
    out += ["\\bottomrule", "\\end{tabular}"]
    return "\n".join(out)


def comb_table(blocks, want, with_amp=False):
    """the comb measurements of probe3: rows per codec, columns the symbol
    variants in want (list of (f0, m, Ts, offset))"""
    def key(l):
        m = re.match(r"comb\s+(\d+) Hz, (\d)-DPSK,\s+([\d.]+) ms symbol \(\s*([\d.]+) useful,\s*([\d.]+) prefix\), offset\s*(\d+) ms:\s*(\d+) tones, mean SER ([\d.]+), good\s*(\d+), rms ([\d.]+) deg, raw\s*(\d+) bit/s(?:, amp ([\d.]+) dB)?", l)
        if not m:
            return None
        f0, mm, Ts, tu, g, off, K, ser, good, rms, raw, amp = m.groups()
        return (int(f0), int(mm), float(Ts), int(off)), (int(K), float(ser), int(good), float(rms), int(raw), float(amp) if amp else None)
    heads = []
    for f0, mm, Ts, off in want:
        heads.append("\\thd{%d Hz, %d-DPSK, %g ms%s}" % (f0, mm, Ts, ", +10 ms" if off else ""))
    out = ["\\begin{tabular}{l%s}" % ("c" * len(want)), "\\toprule",
           "\\rowcolor{ituhead}\\thd{Codec} & " + " & ".join(heads) + " \\\\",
           "\\midrule"]
    for c, lines in blocks.items():
        found = {}
        for l in lines:
            k = key(l)
            if k:
                found[k[0]] = k[1]
        cells = []
        for w in want:
            v = found.get(w)
            if not v:
                cells.append("")
                continue
            K, ser, good, rms, raw, amp = v
            if ser < 0.0005:
                sers = "$<10^{-3}$"
            else:
                sers = "%.3f" % ser
            cells.append("%s / %.0f$^\\circ$ / %d" % (sers, rms, raw))
            if with_amp and amp is not None:
                cells[-1] += " / %.1f" % amp
        out.append(esc(codec_name(c)) + " & " + " & ".join(cells) + " \\\\")
    out += ["\\bottomrule", "\\end{tabular}"]
    return "\n".join(out)


def carrier_table(blocks):
    """single carrier DQPSK at 1700 Hz, 300 and 600 baud"""
    out = ["\\begin{tabular}{lcc}", "\\toprule",
           "\\rowcolor{ituhead}\\thd{Codec} & \\thd{300 baud (600 bit/s)} & \\thd{600 baud (1200 bit/s)} \\\\",
           "\\midrule"]
    for c, lines in blocks.items():
        vals = {}
        for l in lines:
            m = re.match(r"DQPSK fc 1700 Hz\s+(\d+) baud: SER ([\d.]+) rms ([\d.]+) deg", l)
            if m:
                vals[int(m.group(1))] = (float(m.group(2)), float(m.group(3)))
        if vals:
            cells = []
            for b in (300, 600):
                ser, rms = vals.get(b, (None, None))
                if ser is None:
                    cells.append("")
                else:
                    cells.append("%s / %.0f$^\\circ$" % ("$<10^{-3}$" if ser < 0.0005 else "%.3f" % ser, rms))
            out.append(esc(codec_name(c)) + " & " + " & ".join(cells) + " \\\\")
    out += ["\\bottomrule", "\\end{tabular}"]
    return "\n".join(out)


def tone_table(blocks):
    """the steady tone sweep: the SNR per frequency (first probe file)"""
    out = []
    for c, lines in blocks.items():
        for i, l in enumerate(lines):
            if l.startswith("tone sweep"):
                vals = re.findall(r"(\d+):([-\d.]+)/", lines[i + 1])
                out.append((c, [(int(f), float(s)) for f, s in vals]))
                break
    if not out:
        return ""
    freqs = [f for f, _ in out[0][1]]
    res = ["\\begin{tabular}{l%s}" % ("c" * len(freqs)), "\\toprule",
           "\\rowcolor{ituhead}\\thd{Codec} & " + " & ".join("\\thd{%d}" % f for f in freqs) + " \\\\",
           "\\midrule"]
    for c, vals in out:
        if c == "none":
            continue
        res.append(esc(codec_name(c)) + " & " + " & ".join("%.0f" % s for _, s in vals) + " \\\\")
    res += ["\\bottomrule", "\\end{tabular}"]
    return "\n".join(res)


# the order of the codecs in the parametric mode's tables
CODEC_ORDER = ["none", "amr:12.2", "amr:10.2", "amr:7.95", "amr:7.4",
               "amr:4.75", "amrwb:23.85", "amrwb:12.65", "amrwb:6.6",
               "evs:9600", "evs:13200", "evs:24400", "amr:12.2+amr:12.2",
               "amrwb:23.85+amr:12.2", "amr:12.2@1", "amr:12.2@3"]


def ordered(blocks):
    """the codecs of blocks in CODEC_ORDER, the rest after in their order"""
    return ([c for c in CODEC_ORDER if c in blocks] +
            [c for c in blocks if c not in CODEC_ORDER])


def ser_cell(ser):
    return "0" if ser < 0.0005 else "%.3f" % ser


def pitch_table(blocks, want):
    """the parametric mode: rows per codec, columns the configurations in
    want (list of (alphabet name, symbol ms, offset ms)): the lag symbol
    error ratio"""
    def key(l):
        m = re.match(r"pitch (.+?)\s+([\d.]+) ms symbol(, \+10 ms)?\s*: "
                     r"SER ([\d.]+), mean miss ([\d.]+) steps, raw\s*(\d+) bit/s", l)
        if not m:
            return None
        name, ms, off, ser, miss, raw = m.groups()
        return ((name.strip(), float(ms), 10 if off else 0),
                (float(ser), float(miss), int(raw)))
    found = {c: {} for c in blocks}
    for c, lines in blocks.items():
        for l in lines:
            k = key(l)
            if k:
                found[c][k[0]] = k[1]
    heads = []
    for name, ms, off, label in want:
        raws = [found[c][(name, ms, off)][2] for c in found
                if (name, ms, off) in found[c]]
        heads.append("\\thd{%s, %g ms%s (%d)}" %
                     (label, ms, ", +10 ms" if off else "", raws[0] if raws else 0))
    out = ["\\begin{tabular}{l%s}" % ("c" * len(want)), "\\toprule",
           "\\rowcolor{ituhead}\\thd{Codec} & " + " & ".join(heads) + " \\\\",
           "\\midrule"]
    for c in ordered(blocks):
        cells = []
        for name, ms, off, _ in want:
            v = found[c].get((name, ms, off))
            cells.append(ser_cell(v[0]) if v else "")
        if any(cells):
            out.append(esc(codec_name(c)) + " & " + " & ".join(cells) + " \\\\")
    out += ["\\bottomrule", "\\end{tabular}"]
    return "\n".join(out)


def stack_table(blocks, lags=(40, 50, 70)):
    """the two layers in one waveform: the comb for comparison, the
    harmonic signal at fixed lags, and the lag chosen per symbol"""
    heads = ["\\thd{Comb N200 (clause 10): SER / tones / bit/s}"]
    heads += ["\\thd{Harmonic $L=%d$ (%.0f Hz): SER / bit/s}" % (L, 8000.0 / L)
              for L in lags]
    heads.append("\\thd{Lag per symbol: tone SER / lag SER / bit/s}")
    out = ["\\begin{tabular}{l%s}" % ("c" * len(heads)), "\\toprule",
           "\\rowcolor{ituhead}\\thd{Codec} & " + " & ".join(heads) + " \\\\",
           "\\midrule"]
    for c in ordered(blocks):
        comb = fixed = per = None
        fixed = {}
        for l in blocks[c]:
            m = re.match(r"\s*comb .*: SER ([\d.]+), (\d+)/(\d+) tones,\s*(\d+) bit/s", l)
            if m:
                comb = m.groups()
            m = re.match(r"\s*harmonic L=\s*(\d+) .*: SER ([\d.]+),.*,\s*(\d+) bit/s", l)
            if m:
                fixed[int(m.group(1))] = (float(m.group(2)), int(m.group(3)))
            m = re.match(r"\s*harmonic, lag per symbol \((\d+) lags (\d+)-(\d+)\): "
                         r"tone SER ([\d.]+), pitch SER ([\d.]+),.*,\s*(\d+) bit/s", l)
            if m:
                per = m.groups()
        if not comb:
            continue
        cells = ["%s / %s/%s / %s" % (ser_cell(float(comb[0])), comb[1], comb[2], comb[3])]
        for L in lags:
            v = fixed.get(L)
            cells.append("%s / %d" % (ser_cell(v[0]), v[1]) if v else "")
        cells.append("%s / %s / %s" % (ser_cell(float(per[3])), ser_cell(float(per[4])), per[5])
                     if per else "")
        out.append(esc(codec_name(c)) + " & " + " & ".join(cells) + " \\\\")
    out += ["\\bottomrule", "\\end{tabular}"]
    return "\n".join(out)


def dapsk_table(paths):
    """amplitude loading: per codec and interface, per comb, the raw rate
    of the best loading per tone with phase alone and with the amplitude
    rings (probe_dapsk.py's "loaded per tone" lines)"""
    rows = []
    for path in paths:
        cur = None
        for line in open(path):
            m = re.match(r"=== (\S+) \((\d+) Hz\)", line)
            if m:
                cur = (m.group(1), int(m.group(2)), {})
                rows.append(cur)
                continue
            m = re.match(r"comb (\d+) Hz loaded per tone: phase alone\s+(\d+) bit/s, "
                         r"with amplitude\s+(\d+) bit/s \(([+-]\d+) %\)", line)
            if m and cur:
                cur[2][int(m.group(1))] = (int(m.group(2)), int(m.group(3)), m.group(4))
    band = {8000: "N", 16000: "W", 32000: "S"}
    out = ["\\begin{tabular}{lllll}", "\\toprule",
           "\\rowcolor{ituhead}\\thd{Codec} & \\thd{Interface} & "
           "\\thd{200~Hz comb} & \\thd{300~Hz comb} & \\thd{400~Hz comb} \\\\",
           "\\midrule"]
    for c, fs, combs in rows:
        cells = []
        for f0 in (200, 300, 400):
            v = combs.get(f0)
            cells.append("%s%d: %d / %d (%s\\,\\%%)" % (band[fs], f0, v[0], v[1], v[2])
                         if v else "")
        out.append("%s & %d~Hz & %s \\\\" % (esc(codec_name(c)), fs, " & ".join(cells)))
    out += ["\\bottomrule", "\\end{tabular}"]
    return "\n".join(out)


def pquality_table(paths):
    """the parametric mode's autocorrelation by codec (probe_pquality.py)"""
    out = ["\\begin{tabular}{lcccccc}", "\\toprule",
           "\\rowcolor{ituhead}\\thd{Codec} & \\thd{P20 SER} & \\thd{$\\bar c$} & "
           "\\thd{$c$, 10th percentile} & \\thd{Spread of $\\bar c$ over 2~s} & "
           "\\thd{Margin} & \\thd{Comb tones} \\\\", "\\midrule"]
    for path in paths:
        for line in open(path):
            m = re.match(r"(\S+)\s+P20 SER ([\d.]+)\s+c mean ([\d.]+)\s+c 10% ([\d.]+)\s+"
                         r"2 s spread ([\d.]+)\s+margin ([\d.]+)\s+comb tones (\d+)", line)
            if m and m.group(1) != "none":
                c, ser, cm, c10, sp, mg, nt = m.groups()
                out.append("%s & %s & %s & %s & %s & %s & %s \\\\" % (
                    esc(codec_name(c)), ser_cell(float(ser)), cm, c10, sp, mg, nt))
    out += ["\\bottomrule", "\\end{tabular}"]
    return "\n".join(out)


def main():
    (mat, probe, probe3, probe3_wb, evs8, evs16, evs32, cross,
     pitch, pitch_evs, stack) = sys.argv[1:12]
    b1 = parse_probe(probe)
    b3 = parse_probe(probe3)
    b3w = parse_probe(probe3_wb)
    e8, e16, e32 = parse_probe(evs8), parse_probe(evs16), parse_probe(evs32)
    print("% generated by tables.py from the measurement logs; do not edit")
    print("\\newcommand{\\tabmodsurvive}{%\n" + matrix(mat) + "}")
    print("\\newcommand{\\tabfsk}{%\n" + fsk_table(b3) + "}")
    print("\\newcommand{\\tabcarrier}{%\n" + carrier_table(b3) + "}")
    want_nb = [(200, 4, 20.0, 0), (200, 4, 20.0, 10), (200, 8, 20.0, 0), (200, 4, 22.5, 0), (200, 4, 12.5, 0), (400, 4, 20.0, 0)]
    print("\\newcommand{\\tabcombnb}{%\n" + comb_table(b3, want_nb) + "}")
    want_wb = [(200, 4, 20.0, 0), (200, 8, 20.0, 0), (400, 4, 20.0, 0), (400, 8, 20.0, 0), (300, 4, 20.0, 0), (100, 4, 20.0, 0)]
    print("\\newcommand{\\tabcombwb}{%\n" + comb_table(b3w, want_wb) + "}")
    print("\\newcommand{\\tabtones}{%\n" + tone_table(b1) + "}")
    want_evs = [(200, 4, 20.0, 0), (200, 8, 20.0, 0), (300, 4, 20.0, 0), (300, 8, 20.0, 0), (400, 4, 20.0, 0), (400, 8, 20.0, 0)]
    print("\\newcommand{\\tabcombevsnb}{%\n" + comb_table(e8, want_evs, True) + "}")
    print("\\newcommand{\\tabcombevswb}{%\n" + comb_table(e16, want_evs, True) + "}")
    print("\\newcommand{\\tabcombevsswb}{%\n" + comb_table(e32, want_evs, True) + "}")
    print("\\newcommand{\\tabcrosscheck}{%\n" + open(cross).read().strip() + "}")
    pb = parse_probe(pitch)
    pb.update(parse_probe(pitch_evs))
    want_pitch = [("16 lags 40-70 step 2", 20.0, 0, "P20: 16 lags 40--70"),
                  ("16 lags 20-35", 10.0, 0, "P10: 16 lags 20--35"),
                  ("8 lags 40-96 step 8", 20.0, 0, "8 lags 40--96"),
                  ("8 lags 40-96 step 8", 20.0, 10, "8 lags 40--96"),
                  ("32 lags 32-63", 20.0, 0, "32 lags 32--63"),
                  ("32 lags 32-63", 10.0, 0, "32 lags 32--63")]
    print("\\newcommand{\\tabpitch}{%\n" + pitch_table(pb, want_pitch) + "}")
    print("\\newcommand{\\tabstack}{%\n" + stack_table(parse_probe(stack)) + "}")
    # dapsk_nb.txt dapsk_wb.txt dapsk_swb.txt pquality.txt
    if len(sys.argv) > 15:
        print("\\newcommand{\\tabdapsk}{%\n" + dapsk_table(sys.argv[12:15]) + "}")
        print("\\newcommand{\\tabpquality}{%\n" + pquality_table(sys.argv[15:16]) + "}")


main()
