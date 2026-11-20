#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# hcf_probe_test.py <dmodem-hcf-probe> <fake libusb>: dmodem-hcf-probe on
# a fake Conexant HCF USB modem (fake_hcf.cpp, a libusb in front of the
# real one through LD_PRELOAD):
#
# - its eight pipes listed, the DSP reset and its ID block read (the
#   signature, the chip: L8371, and L8373 with FAKE_HCF_CHIP);
# - a DSP that gives no signature: said, exit 1;
# - the DSP code from two stand-in images (in the "DRAM" form of
#   Conexant's, not theirs: patterns, each module's checksum the fake's,
#   the sum of its octets): the modules the driver loads at the start,
#   each at its address, every octet, each checksum checked;
# - a module missing from the images, and a checksum that differs: said,
#   exit 1;
# - a "driver" (a PE with COFF symbols, as WINACUSB.sys keeps them) that
#   holds the images: found by name, loaded the same.
#
# The logs go to DMODEM_TEST_DIR, without it to the temporary directory,
# kept with KEEP=1 (and printed).
import os, shutil, struct, subprocess, sys, tempfile

probe, fake = sys.argv[1:3]
fake = os.path.abspath(fake)
tmp = tempfile.mkdtemp(prefix="dmodem-hcf-")
logs = os.environ.get("DMODEM_TEST_DIR") or tmp


def image(unit, mods, bad=()):
    """a DRAM image: mods (name, id, address, units), unit octets a unit;
    the ids in bad with a wrong checksum"""
    hdr = b"DRAM" + struct.pack("<HH", 0x36, len(mods))
    off = len(hdr) + sum(0x1c + len(m[0]) + 1 for m in mods)
    recs, code = b"", b""
    for name, mid, addr, units in mods:
        data = bytes(((i * 7 + mid) & 0xff) for i in range(units * unit))
        ck = (sum(data) + (1 if mid in bad else 0)) & 0xff
        recs += struct.pack("<HHHHIIIII", mid, 0xffff, 0x8000 if unit == 4 else 0,
                            ck, addr, addr + units, units, off, 0)
        recs += name.encode() + b"\0"
        off += len(data)
        code += data
    return hdr + recs + code

# L8371's start: CTL ec eb ed ee ea, SPX ee f0 f6 f7; then CTL 49 41
CTL = [("PORInit", 0xec, 0x3f00, 149), ("General", 0xeb, 0x3a80, 1092),
       ("Voice", 0xed, 0x2f30, 1839), ("DownDefVec", 0xee, 0x3fc0, 58),
       ("Vectors", 0xea, 0x1000, 1094), ("V34", 0x49, 0x1450, 4983),
       ("V8", 0x41, 0x27c7, 1893)]
SPX = [("GENERAL", 0xee, 0x7af6, 265), ("VECTORS", 0xf0, 0x7001, 184),
       ("APCM", 0xf6, 0x71a5, 2375), ("AVPCM", 0xf7, 0x70b9, 236)]


def write(name, data):
    p = os.path.join(tmp, name)
    open(p, "wb").write(data)
    return p


def pe(blobs):
    """a PE with one section holding blobs (name: octets), and COFF
    symbols _name and _name_SIZE (their values the RVAs, as
    WINACUSB.sys's)"""
    sec_rva, sec_raw = 0x1000, 0x200
    body, syms = b"", []
    for name, data in blobs:
        syms.append(("_" + name, sec_rva + len(body)))
        body += data + b"\0" * (-len(data) % 4)
        syms.append(("_" + name + "_SIZE", sec_rva + len(body)))
        body += struct.pack("<I", len(data))
    symtab = sec_raw + len(body)
    strtab = b""
    st = b""
    for name, value in syms:
        n = name.encode()
        if len(n) <= 8:
            field = n + b"\0" * (8 - len(n))
        else:
            field = struct.pack("<II", 0, 4 + len(strtab))
            strtab += n + b"\0"
        st += field + struct.pack("<IhHBB", value, 1, 0, 2, 0)
    h = 0x40
    mz = b"MZ" + b"\0" * 0x3a + struct.pack("<I", h)
    fh = b"PE\0\0" + struct.pack("<HHIIIHH", 0x14c, 1, 0, symtab, len(syms), 0, 0)
    sh = b".data\0\0\0" + struct.pack("<IIIIIIHHI", len(body), sec_rva, len(body),
                                      sec_raw, 0, 0, 0, 0, 0)
    head = mz + fh + sh
    return head + b"\0" * (sec_raw - len(head)) + body + st + \
        struct.pack("<I", 4 + len(strtab)) + strtab

ok = True
def check(what, cond):
    """print what was checked and whether it held; any failure fails the test"""
    global ok
    print("%s: %s" % (what, "yes" if cond else "NO"))
    ok = ok and bool(cond)

n = 0
def run(args, **env):
    """the probe with args on the fake (env more of its settings): its
    exit status, its output, the fake's log"""
    global n
    n += 1
    e = dict(os.environ, LD_PRELOAD=fake, FAKE_HCF_LOG="1", **env)
    p = subprocess.run([probe, "--seconds", "0"] + args, env=e,
                       capture_output=True, text=True, timeout=30)
    with open(os.path.join(logs, "probe-%d.log" % n), "w") as f:
        f.write(p.stdout + "\n--- fake\n" + p.stderr)
    return p.returncode, p.stdout, p.stderr

try:
    ctl = write("ctl.img", image(1, CTL))
    spx = write("spx.img", image(4, SPX))

    r, out, err = run([])
    check("eight pipes listed", out.count("pipe ") == 8 and
          "pipe 5: endpoint 86 IN interrupt" in out)
    check("the DSP's signature and chip",
          "DSP signature 0451 49a5, chip 3731 (L8371)" in out and r == 0)
    check("the modem interface reset, then the DSP's",
          "fake HCF: gpio 00 00 09" in err and "reg 1a = 80" in err)
    r, out, err = run([], FAKE_HCF_CHIP="3733")
    check("FAKE_HCF_CHIP 3733: L8373", "chip 3733 (L8373)" in out)
    r, out, err = run([], FAKE_HCF_MUTE="1")
    check("no signature: said", r == 1 and "gave no signature" in out)

    r, out, err = run(["--images", ctl, spx])
    check("11 modules loaded", r == 0 and "DSP code: 11 of 11 modules loaded" in out)
    loads = [l for l in err.splitlines() if " module at " in l]
    want = ["CTL module at 3f00, 149 octets", "CTL module at 3a80, 1092 octets",
            "CTL module at 2f30, 1839 octets", "CTL module at 3fc0, 58 octets",
            "CTL module at 1000, 1094 octets", "SPX module at 7af6, 1060 octets",
            "SPX module at 7001, 736 octets", "SPX module at 71a5, 9500 octets",
            "SPX module at 70b9, 944 octets", "CTL module at 1450, 4983 octets",
            "CTL module at 27c7, 1893 octets"]
    check("each at its address, every octet, in the driver's order",
          len(loads) == len(want) and all(w in l for w, l in zip(want, loads)))

    bad = write("ctl-bad.img", image(1, CTL, bad=(0xeb,)))
    r, out, err = run(["--images", bad, spx])
    check("a wrong checksum: said", r == 1 and
          "CTL General: the DSP's checksum" in out and "10 of 11" in out)
    short = write("ctl-short.img", image(1, CTL[:-1]))
    r, out, err = run(["--images", short, spx])
    check("a missing module: said", r == 1 and "CTL module 41: not in the images" in out)

    drv = write("WINACUSB.sys", pe([("CTL_DATA_L8371DP", image(1, CTL)),
                                    ("SPX_DATA_L8371DP", image(4, SPX))]))
    r, out, err = run(["--driver", drv])
    check("from a driver's symbols: 11 modules", r == 0 and "11 of 11" in out)
    r, out, err = run(["--driver", drv], FAKE_HCF_CHIP="3733")
    check("a driver without the chip's images: said",
          r == 1 and "it has no CTL_DATA_L8373DP" in out)
finally:
    if not os.environ.get("KEEP"):
        shutil.rmtree(tmp, ignore_errors=True)
    if logs != tmp or os.environ.get("KEEP"):
        print("logs in", logs)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
