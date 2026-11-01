#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# hda_modem_setup_test.py <hda-modem-setup>: the helper on a fake sysfs
# and kernel configuration (in a temporary directory):
#
# - card 0: an audio codec (address 0, Realtek, driven) and a modem
#   codec in snd-hda-codec-si3054's list (address 1, Agere 11c1:3026,
#   driven by it);
# - card 1: a modem codec outside the list with no driver (address 1,
#   14f1:2c06);
# - card 2: a modem codec outside the list that has a driver (address 2).
#
# list shows the three modem codecs and what each is; write gives the
# one on card 1 the Si3054's ID (its [codec] line, [vendor_id]
# 0x163c3055, and patch=,dmodem-hda-modem.fw), or with --codec the one
# named; it refuses without the patch loader in the kernel's
# configuration, and warns of another patch= option; remove takes the
# files away.
import os, shutil, subprocess, sys, tempfile

tool = sys.argv[1]
tmp = tempfile.mkdtemp(prefix="dmodem-hda-")
sysfs = os.path.join(tmp, "sys")
fw = os.path.join(tmp, "firmware")
mp = os.path.join(tmp, "modprobe.d")
kcfg = os.path.join(tmp, "config")
ok = True


def check(what, cond):
    global ok
    print("%s: %s" % (what, "yes" if cond else "NO"))
    ok = ok and bool(cond)


def codec(card, addr, vendor, subsystem, afg, mfg, driver=None, chip=""):
    d = os.path.join(sysfs, "bus/hdaudio/devices/hdaudioC%dD%d" % (card, addr))
    os.makedirs(d)
    for k, v in (("vendor_id", "0x%x" % vendor),
                 ("subsystem_id", "0x%x" % subsystem),
                 ("revision_id", "0x100000"), ("afg", "0x%x" % afg),
                 ("mfg", "0x%x" % mfg), ("chip_name", chip)):
        with open(os.path.join(d, k), "w") as f:
            f.write(v + "\n")
    if driver:
        drv = os.path.join(sysfs, "bus/hdaudio/drivers", driver)
        os.makedirs(drv, exist_ok=True)
        os.symlink(drv, os.path.join(d, "driver"))


def run(*a):
    p = subprocess.run([sys.executable, tool, *a, "--sysfs", sysfs,
                        "--firmware-dir", fw, "--modprobe-dir", mp,
                        "--kconfig", kcfg], capture_output=True, text=True)
    print("$ hda-modem-setup %s\n%s" % (" ".join(a), p.stdout + p.stderr))
    return p.returncode, p.stdout


def text(p):
    try:
        with open(p) as f:
            return f.read()
    except OSError:
        return None


codec(0, 0, 0x10ec0262, 0x10280209, 1, 0, "snd_hda_codec_realtek", "ALC262")
codec(0, 1, 0x11c13026, 0x11c11040, 0, 2, "snd_hda_codec_si3054", "Si3054")
codec(1, 1, 0x14f12c06, 0x14f10000, 0, 2)
codec(2, 2, 0x12345678, 0x00000001, 0, 2, "some_driver")
with open(kcfg, "w") as f:
    f.write("CONFIG_SND_HDA_PATCH_LOADER=y\nCONFIG_SND_HDA_CODEC_SI3054=m\n")

r, out = run("list")
check("list: three modem codecs", r == 0 and out.count("hdaudioC") == 3 and
      "hdaudioC0D0" not in out)
check("list: 11c1:3026 in the list",
      "11c1:3026" in out and "in snd-hda-codec-si3054's list (Agere)" in out)
check("list: 14f1:2c06 a candidate",
      "14f1:2c06" in out and "no driver: a candidate" in out)
check("list: 1234:5678 driven", "driven by some_driver" in out)

# without the patch loader: refused, nothing written
with open(kcfg, "w") as f:
    f.write("CONFIG_SND_HDA_CODEC_SI3054=m\n")
r, out = run("write")
check("write refused without the patch loader",
      r == 1 and "CONFIG_SND_HDA_PATCH_LOADER" in out and
      text(os.path.join(fw, "dmodem-hda-modem.fw")) is None)
with open(kcfg, "w") as f:
    f.write("CONFIG_SND_HDA_PATCH_LOADER=y\nCONFIG_SND_HDA_CODEC_SI3054=m\n")

# another patch= option: a warning
os.makedirs(mp)
with open(os.path.join(mp, "alsa-base.conf"), "w") as f:
    f.write("options snd_hda_intel patch=other.fw\n")
r, out = run("write")
check("write: the other patch= option warned of",
      r == 0 and "alsa-base.conf gives snd-hda-intel a patch already" in out)
patch = text(os.path.join(fw, "dmodem-hda-modem.fw"))
conf = text(os.path.join(mp, "dmodem-hda-modem.conf"))
check("patch: the card 1 codec's [codec] line and the Si3054's ID",
      patch and "[codec]\n0x14f12c06 0x14f10000 1\n" in patch and
      "[vendor_id]\n0x163c3055\n" in patch and patch.count("[codec]") == 1)
check("option: the patch for card 1 only",
      conf and "options snd-hda-intel patch=,dmodem-hda-modem.fw\n" in conf)

# --codec: the one named, even if driven
r, out = run("write", "--codec", "hdaudioC2D2")
patch = text(os.path.join(fw, "dmodem-hda-modem.fw"))
conf = text(os.path.join(mp, "dmodem-hda-modem.conf"))
check("--codec: card 2's codec",
      r == 0 and patch and "0x12345678 0x00000001 2" in patch and
      "0x14f12c06" not in patch and
      "patch=,,dmodem-hda-modem.fw\n" in conf)
r, out = run("write", "--codec", "hdaudioC5D1")
check("--codec: an unknown codec refused", r == 1 and "no modem codec" in out)

r, out = run("remove")
check("remove", r == 0 and text(os.path.join(fw, "dmodem-hda-modem.fw")) is None
      and text(os.path.join(mp, "dmodem-hda-modem.conf")) is None and
      text(os.path.join(mp, "alsa-base.conf")) is not None)
r, out = run("remove")
check("remove again: nothing", r == 0 and "nothing to remove" in out)

shutil.rmtree(tmp, ignore_errors=True)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
