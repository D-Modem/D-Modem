# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# tables.py <matrix.txt> <probe.txt> <probe3.txt> <probe3_wb.txt>
#           <probe_evs8.txt> <probe_evs16.txt> <probe_evs32.txt> <cross.tex>
#           > measured.tex
#
# The tables of Appendices I and II of v_cvm.tex from the measurement logs
# (measure/logs/, made by the scripts of measure/): matrix.txt from
# matrix.sh (D-Modem's modulations through each codec, interop_call),
# probe.txt and probe3.txt from probe.py and probe3.py at the 8000 Hz
# interface, probe3_wb.txt from probe3.py at 16000 Hz, probe_evs*.txt
# from probe_evs.py at 8000, 16000 and 32000 Hz; cross.tex has the rows
# of the cross-check table.  Prints LaTeX.
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


def main():
    mat, probe, probe3, probe3_wb, evs8, evs16, evs32, cross = sys.argv[1:9]
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


main()
