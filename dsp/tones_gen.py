#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
#    tones_gen.py - tones.cpp from the ITU's list of the tones
#                        used in national networks (Annex to ITU
#                        Operational Bulletin No. 955, position on 1 May
#                        2010, after Recommendation E.180), its Word file:
#
#      https://www.itu.int/dms_pub/itu-t/opb/sp/T-SP-E.180-2010-MSW-E.doc
#
#    python3 -I tones_gen.py T-SP-E.180-2010-MSW-E.doc > tones.cpp
#
#    The file is only read: its text is the longest run of 8 bit
#    characters (Windows-1252) in it, paragraphs split at its marks.
#    Each country's "Ringing tone", "Dial tone", "Busy tone" and
#    "Congestion tone" (or "Re-order tone") (the first of several, I, II, ..., a PABX's, a call
#    waiting's or a message waiting's not; the ringing tone one with a
#    cadence) give its row:
#
#    - the frequency: of alternatives (//, or / but for a/b of two voice
#      band frequencies, a pair) the first plain tone or pair (a+b), else the first modulated one (a x m, or a+m with m
#      under 100 Hz); none given, the country's busy tone's;
#    - the cadence: its "on" and "off" seconds in turn ("2x(...)"
#      repeated), up to eight; a dial tone "continuous" (none), a
#      continuous ringing tone no row.  A country with no dial tone in
#      the list has a continuous one of its ringing tone's frequency; with
#      no congestion tone, its busy tone; with no busy tone, its
#      congestion tone, or with neither 0.5 s on and off of its ringing
#      tone's frequency.
#
#    The list has no ring signal frequency: 20 Hz with the North
#    American tone (440 + 480 Hz), 16 Hz in Japan, 50 Hz in France and
#    its departments and territories (440 Hz, 1.5 s on), 25 Hz elsewhere.
#
import re
import sys
import unicodedata

NOTES = "names: the list's, the part in parentheses left out"


def paragraphs(path):
    data = open(path, "rb").read()
    runs = re.findall(rb"[\x20-\x7e\x80-\xff\t\r\x0b\x07]{200,}", data)
    text = max(runs, key=len).decode("cp1252", "replace")
    ps = [p.strip() for p in re.split(r"[\r\x0b\x07]", text)]
    return [p for p in ps if p]


TONE = re.compile(r"^[A-Z][A-Za-z '\-]*\b[Tt]one\b")
VALUE = re.compile(r"^[0-9(\[]|^continuous|\bon\b.*\boff\b")


def countries(ps):
    """{name: [(tone heading, [value paragraphs])]} in the list's order"""
    start = next(i for i, p in enumerate(ps) if p.startswith("Albania"))
    out = {}
    country = None
    for i in range(start, len(ps)):
        p = ps[i]
        nxt = ps[i + 1] if i + 1 < len(ps) else ""
        if p.startswith("Explanatory") or p.startswith("Cadence (seconds)"):
            break
        if (TONE.match(nxt) and not TONE.match(p) and not VALUE.search(p)
                and p[0].isupper() and "tone" not in p.lower()):
            country = p
            out.setdefault(country, [])
        elif country and TONE.match(p):
            out[country].append((p, []))
        elif country and out[country]:
            out[country][-1][1].append(p)
    return out


def cadence(text):
    # the list's typos: "O.125", "2..0", "037" (0.37)
    text = re.sub(r"\bO\.", "0.", text.replace("..", "."))
    text = re.sub(r"(?<![\d.])0(\d+)\b(?!\.)", r"0.\1", text)
    # 2x(a on b off) repeated
    text = re.sub(r"(\d)x\(([^)]*)\)", lambda m: " ".join([m.group(2)] * int(m.group(1))), text)
    words = re.findall(r"(\d+(?:\.\d+)?)\s*(on|off)", text)
    cad = []
    for v, w in words:
        want = "on" if len(cad) % 2 == 0 else "off"
        if w != want:
            return None
        cad.append(float(v))
    if len(cad) < 2 or len(cad) % 2 or len(cad) > 8:
        return None
    return cad


def frequency(text):
    """(f1, f2, mod) or None"""
    # a/b of two voice band frequencies is a pair (350/440); else '/'
    # parts alternatives, as '//' do
    text = re.sub(r"\b([3-9]\d\d)/([3-9]\d\d)\b", r"\1+\2", text)
    alts = [a.strip() for a in re.split(r"//|/", text) if a.strip()]
    for a in alts:
        m = re.fullmatch(r"(\d+)(?:\s*\+\s*(\d+))?", a)
        if m and int(m.group(1)) >= 300 and (not m.group(2) or int(m.group(2)) >= 100):
            a, b = int(m.group(1)), int(m.group(2) or 0)
            return (min(a, b), max(a, b), 0.0) if b else (a, 0, 0.0)
    for a in alts:
        m = re.fullmatch(r"(\d+)\s*[x+]\s*\(?(\d+)(?:~\d+\))?(?:\s+(\d)/(\d))?", a)
        if m and int(m.group(1)) >= 300 and int(m.group(2)) < 100:
            mod = float(m.group(2))
            if m.group(3):
                mod += float(m.group(3)) / float(m.group(4))
            return (int(m.group(1)), 0, round(mod, 2))
    return None


def busy_frequency(tones):
    for head, vals in tones:
        if head.lower().startswith("busy") and vals:
            f = frequency(vals[0])
            if f:
                return f
    return None


def row_name(country):
    n = re.sub(r"\s*\(.*", "", country)
    n = unicodedata.normalize("NFKD", n).encode("ascii", "ignore").decode()
    n = n.replace("'", "").replace(",", "").replace(":", "")
    n = re.sub(r"[^A-Za-z0-9]+", "_", n).strip("_").upper()
    return n


def ring_hz(name, f1, f2):
    if (f1, f2) == (440, 480) or (f1, f2) == (480, 440):
        return 20
    if name == "JAPAN":
        return 16
    if f1 == 440 and name in FRENCH:
        return 50
    return 25


FRENCH = {"FRANCE", "FRENCH_DEPARTMENTS_AND_TERRITORIES_IN_THE_INDIAN_OCEAN",
          "FRENCH_GUIANA", "FRENCH_POLYNESIA", "GUADELOUPE", "MARTINIQUE",
          "MAYOTTE", "NEW_CALEDONIA", "REUNION", "SAINT_PIERRE_AND_MIQUELON",
          "WALLIS_AND_FUTUNA"}

# the core's country names (core/homolog_data.cpp) that are not the
# list's, and other short ones
ALIASES = {
    "USA": "UNITED_STATES_OF_AMERICA", "US": "UNITED_STATES_OF_AMERICA",
    "UNITED_STATES": "UNITED_STATES_OF_AMERICA",
    "UK": "UNITED_KINGDOM_OF_GREAT_BRITAIN_AND_NORTHERN_IRELAND",
    "GB": "UNITED_KINGDOM_OF_GREAT_BRITAIN_AND_NORTHERN_IRELAND",
    "UNITED_KINGDOM": "UNITED_KINGDOM_OF_GREAT_BRITAIN_AND_NORTHERN_IRELAND",
    "KOREA": "KOREA", "SOUTHKOREA": "KOREA", "SOUTH_KOREA": "KOREA",
    "HONG_KONG": "HONG_KONG_CHINA", "HONGKONG": "HONG_KONG_CHINA",
    "MACAU": "MACAO_CHINA", "MACAO": "MACAO_CHINA",
    "SAUDIARABIA": "SAUDI_ARABIA", "SOUTHAFRICA": "SOUTH_AFRICA",
    "SRILANKA": "SRI_LANKA", "UAE": "UNITED_ARAB_EMIRATES",
    "BRUNEI": "BRUNEI_DARUSSALAM", "RUSSIA": "RUSSIAN_FEDERATION",
    "SLOVAKIA": "SLOVAK_REPUBLIC", "ARGENTINA": "ARGENTINE_REPUBLIC",
    "GABON": "GABONESE_REPUBLIC", "KYRGYZSTAN": "KYRGYZ_REPUBLIC",
    "SYRIA": "SYRIAN_ARAB_REPUBLIC", "LAOS": "LAO_PEOPLES_DEMOCRATIC_REPUBLIC",
    "MACEDONIA": "THE_FORMER_YUGOSLAV_REPUBLIC_OF_MACEDONIA",
    "CZECHIA": "CZECH_REPUBLIC", "DR_CONGO": "DEMOCRATIC_REPUBLIC_OF_THE_CONGO",
}


def pick(tones, kind, continuous_ok):
    """((f1, f2, mod), cadence) of the country's first tone of kind
    ("ringing tone", "dial tone") that is not a PABX's, a call waiting's
    or a message waiting's: cadence [] if continuous (continuous_ok);
    None if none"""
    for head, vals in tones:
        h = head.lower()
        if not h.startswith(kind) or "pabx" in h or "waiting" in h:
            continue
        vals = [v for v in vals if not re.fullmatch(r"\(\d+\)", v)]
        cad = None
        f = None
        for v in vals[:2]:
            c = cadence(v)
            if c:
                cad = c
            elif continuous_ok and v.strip().lower() == "continuous":
                cad = []
            elif f is None and not TONE.match(v):
                f = frequency(v)
        if cad is None:
            continue
        f = f or busy_frequency(tones)
        if f:
            return (f, cad)
    return None


def tone_c(t):
    """a Tone's initialiser"""
    (f1, f2, mod), cad = t
    num = lambda x: "%gf" % x if "." in "%g" % x else "%g.0f" % x
    return "{ %d, %d, %s, %d, { %s } }" % (
        f1, f2, num(mod), len(cad), ", ".join(num(c) for c in cad) or "0.0f")


def main():
    db = countries(paragraphs(sys.argv[1]))
    rows, skipped, no_dial, no_busy, no_congestion = [], [], [], [], []
    for country, tones in db.items():
        name = row_name(country)
        ringing = pick(tones, "ringing tone", False)
        if not ringing:
            if any(h.lower().startswith("ringing") for h, _ in tones):
                skipped.append(country)
            continue
        dial = pick(tones, "dial tone", True)
        if not dial:
            # none in the list: a continuous tone of the ringing tone's
            # frequency
            no_dial.append(name)
            dial = (ringing[0], [])
        busy = pick(tones, "busy tone", True)
        congestion = (pick(tones, "congestion tone", True) or
                      pick(tones, "re-order tone", True) or
                      pick(tones, "reorder tone", True))
        if not busy:
            # none: the congestion tone, or E.180's 0.5 s on and off
            no_busy.append(name)
            busy = congestion or (ringing[0], [0.5, 0.5])
        if not congestion:
            no_congestion.append(name)
            congestion = busy
        (f1, f2, _), _ = ringing
        rows.append((name, ringing, ring_hz(name, f1, f2), dial, busy,
                     congestion))
    rows.sort()
    names = {r[0] for r in rows}
    for a, n in ALIASES.items():
        assert n in names, n
    w = sys.stdout.write
    w(HEAD)
    for name, ringing, hz, dial, busy, congestion in rows:
        w('\t{ "%s",\n\t  %s, %d,\n\t  %s,\n\t  %s,\n\t  %s },\n' %
          (name, tone_c(ringing), hz, tone_c(dial), tone_c(busy),
           tone_c(congestion)))
    w("\t{ nullptr, {}, 0, {}, {}, {} },\n};\n\n")
    w("const Alias ALIASES[] = {\n")
    for a in sorted(ALIASES):
        if a != ALIASES[a]:
            w('\t{ "%s", "%s" },\n' % (a, ALIASES[a]))
    w("\t{ nullptr, nullptr },\n};\n\n")
    w(TAIL)
    sys.stderr.write("%d rows; no ringing tone with a cadence: %s; no dial "
                     "tone: %s; no busy tone: %s; no congestion tone: %s\n" %
                     (len(rows), "; ".join(skipped), " ".join(no_dial),
                      " ".join(no_busy), " ".join(no_congestion)))


HEAD = """/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    tones.cpp - the countries' tones (tones.h), from the
 *                     ITU's list of the tones used in national networks
 *                     (Annex to ITU Operational Bulletin No. 955, position
 *                     on 1 May 2010, after Recommendation E.180): each
 *                     country's ringing, dial, busy and congestion
 *                     tones
 *
 *    Generated by tones_gen.py, which says how a row is chosen;
 *    the countries whose ringing tone is continuous or has no cadence
 *    are left out.  The ring signal's frequency is not in the list: 20 Hz
 *    with the North American tone (440 + 480 Hz), 16 Hz in Japan, 50 Hz
 *    in France's networks, 25 Hz elsewhere.
 *
 */

#include <string.h>

#include "tones.h"

namespace tones {

const Country COUNTRIES[] = {
"""

TAIL = """/* whether name is n, case, spaces, '-' and '.' taken as '_' */
static bool same(const char *n, const char *name)
{
	for (; *n && *name; n++, name++) {
		char x = *name >= 'a' && *name <= 'z' ? (char)(*name - 32) : *name;
		if (x == ' ' || x == '-' || x == '.')
			x = '_';
		if (x != *n)
			return false;
	}
	return !*n && !*name;
}

/* see tones.h */
const Country *find(const char *name)
{
	for (const Alias *a = ALIASES; a->alias; a++)
		if (same(a->alias, name))
			name = a->name;
	for (const Country *c = COUNTRIES; c->name; c++)
		if (same(c->name, name))
			return c;
	return nullptr;
}

} // namespace tones
"""

if __name__ == "__main__":
    main()
