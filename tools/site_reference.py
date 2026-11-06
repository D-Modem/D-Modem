#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
#    site_reference.py - the reference pages of D-Modem's site, from the
#                        code: dmodemd's command line (the OPTIONS table
#                        in frontend/dmodemd/options.cpp), the CMake
#                        options and cache settings, and the environment
#                        variables the programs read or set.
#
#    tools/site_reference.py           writes docs/reference/*.md
#    tools/site_reference.py --check   fails if they are out of date, or
#                                      if an environment variable is in
#                                      no page of docs/
"""Write (or check) docs/reference/ from the source tree."""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "reference")
HEAD = ("<!-- Made by tools/site_reference.py from the source: change the "
        "code (or the script), not this page. -->\n\n")

# variables read by tests and debugging aids alone: listed, but no page
# need describe them
INTERNAL = {"DMODEM_G729_VECTORS", "DMODEM_TEST_DIR"}


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def cell(s):
    s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    return " ".join(s.replace("|", "\\|").split())


def c_string(lits):
    """Adjacent C string literals as one Python string."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', lits)
    s = "".join(parts)
    return re.sub(r"\\(.)", lambda m: m.group(1), s)


# --- dmodemd's options ---------------------------------------------------

OPT_RE = re.compile(
    r"\{\s*(0|'(.)')\s*,\s*\"([a-z0-9-]+)\"\s*,\s*Takes::(\w+)\s*,\s*(true|false)\s*,"
    r"(.*?),\s*(nullptr|\"(?:[^\"\\]|\\.)*\")\s*,\s*false\s*\}", re.S)


def dmodemd_options():
    src = read("frontend/dmodemd/options.cpp")
    start = src.index("const Option OPTIONS[OPT_COUNT] = {")
    end = src.index("\n};", start)
    body = src[start:end]
    opts = []
    # each option, and the #if (Windows' or not) it is under
    for m in OPT_RE.finditer(body):
        before = body[:m.start()]
        state = []
        for t in (l.strip() for l in before.splitlines()):
            if t.startswith("#ifndef _WIN32"):
                state.append("not Windows")
            elif t.startswith("#ifdef _WIN32"):
                state.append("Windows")
            elif t.startswith("#else") and state:
                state[-1] = "Windows" if state[-1] == "not Windows" else "not Windows"
            elif t.startswith("#endif") and state:
                state.pop()
        help_text = c_string(m.group(6))
        # an entry whose help differs on Windows: #ifdef _WIN32, its
        # Windows help; #else, the others'
        if "#ifdef _WIN32" in m.group(6):
            other = re.match(r",?\s*#else\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+)", body[m.end():])
            if other:
                help_text = "%s (on Windows: %s)" % (c_string(other.group(1)), help_text)
        if help_text == "this text":
            help_text = "print the usage, and exit"
        value = None if m.group(7) == "nullptr" else c_string(m.group(7))
        opts.append({
            "letter": m.group(2),
            "name": m.group(3),
            "takes": m.group(4),
            "help": help_text,
            "default": value,
            "only": state[-1] if state else "",
        })
    return opts


def posix_only():
    src = read("frontend/dmodemd/options.cpp")
    m = re.search(r"bool posix_only\(s16 i\)\s*\{\s*#ifdef _WIN32\s*return(.*?);", src, re.S)
    names = re.findall(r"OPT_(\w+)", m.group(1)) if m else []
    return {n.lower().replace("_", "-") for n in names}


def page_dmodemd():
    opts = dmodemd_options()
    posix = posix_only()
    b = [HEAD, "# dmodemd's command line\n\n",
         "Every option `dmodemd` takes, from its option table "
         "(`frontend/dmodemd/options.cpp`); `dmodemd --help` prints the same. "
         "The SIP account and the rest of the line's settings are "
         "[environment variables](environment.md); [Usage](../usage.md) "
         "shows them in use.\n\n",
         "    dmodemd [options] [name]\n\n",
         "`name` is the modem's: its pty is `/dev/tty<name>` "
         "(`/dev/ttyDM0` for the default, `dmodem0`... as the usage says), "
         "and its logs are named after it.\n\n",
         "| option | argument | default | systems | what it does |\n",
         "|---|---|---|---|---|\n"]
    for o in opts:
        flag = "`--%s`" % o["name"]
        if o["letter"]:
            flag = "`-%s`, %s" % (o["letter"], flag)
        arg = {"Nothing": "", "Text": "text", "Number": "number"}[o["takes"]]
        default = "`%s`" % o["default"] if o["default"] is not None else ""
        only = o["only"]
        if not only and o["name"] in posix:
            only = "not Windows"
        help_text = o["help"]
        help_text = help_text[0].upper() + help_text[1:] if help_text else ""
        if help_text and not help_text.endswith((".", ")")):
            help_text += "."
        b.append("| %s | %s | %s | %s | %s |\n" % (flag, arg, default, only, cell(help_text)))
    return "".join(b)


# --- CMake ---------------------------------------------------------------

def cmake_files():
    out = []
    for d, dirs, files in os.walk(ROOT):
        rel = os.path.relpath(d, ROOT)
        if rel.startswith((".git", "build", "external", "docs")) or "/build" in rel:
            dirs[:] = []
            continue
        dirs.sort()
        for f in sorted(files):
            if f == "CMakeLists.txt" or f.endswith(".cmake"):
                out.append(os.path.relpath(os.path.join(d, f), ROOT))
    return out


def page_cmake():
    opts, cache = [], []
    for rel in cmake_files():
        src = read(rel)
        for m in re.finditer(r"\boption\(\s*(DMODEM_\w+)\s+\"((?:[^\"\\]|\\.)*)\"\s*([^)]*)\)", src):
            default = m.group(3).strip()
            if default.startswith("${"):
                default = "ON, but OFF on Windows" if "pbx" in default else default
            opts.append((m.group(1), default or "OFF", m.group(2), rel))
        for m in re.finditer(r"\bset\(\s*(DMODEM_\w+)\s+(\S+)\s+CACHE\s+(\w+)\s*\n?\s*\"((?:[^\"\\]|\\.)*)\"", src):
            default = m.group(2).strip('"')
            if default.startswith("${"):
                default = "(depends on the system)"
            cache.append((m.group(1), default, m.group(3), m.group(4), rel))
    b = [HEAD, "# CMake options\n\n",
         "What `cmake -B build -D<NAME>=<value>` takes, from the "
         "`option()`s and cache settings in the build files. "
         "[Building](../building.md) says what each system needs.\n\n",
         "## Switches\n\n",
         "| option | default | what it does | set in |\n|---|---|---|---|\n"]
    for name, default, text, rel in opts:
        b.append("| `%s` | %s | %s | `%s` |\n" % (name, default, cell(text), rel))
    b += ["\n## Settings\n\n",
          "| setting | default | type | what it is | set in |\n|---|---|---|---|---|\n"]
    for name, default, typ, text, rel in cache:
        b.append("| `%s` | `%s` | %s | %s | `%s` |\n" % (name, default, typ.lower(), cell(text), rel))
    return "".join(b)


# --- environment variables -------------------------------------------------

def sources():
    for d, dirs, files in os.walk(ROOT):
        rel = os.path.relpath(d, ROOT)
        if rel.startswith((".git", "build", "external", "docs")):
            dirs[:] = []
            continue
        dirs.sort()
        for f in sorted(files):
            if f.endswith((".c", ".cpp", ".h", ".hpp")):
                yield os.path.relpath(os.path.join(d, f), ROOT)


def env_vars():
    found = {}
    for rel in sources():
        src = read(rel)
        for m in re.finditer(r"getenv\(\s*\"((?:SIP|DMODEM)_[A-Z0-9_]+)\"", src):
            found.setdefault(m.group(1), {"read": set(), "set": set()})["read"].add(rel)
        for m in re.finditer(r"\"((?:SIP|DMODEM)_[A-Z0-9_]+)\"", src):
            v = found.setdefault(m.group(1), {"read": set(), "set": set()})
            if rel not in v["read"]:
                v["set"].add(rel)
    return found


def doc_pages():
    pages = {}
    docs = os.path.join(ROOT, "docs")
    for f in sorted(os.listdir(docs)):
        if f.endswith(".md"):
            pages[f] = read(os.path.join("docs", f))
    return pages


def title(text, fallback):
    m = re.search(r"^# (.+)$", text, re.M)
    return m.group(1).strip() if m else fallback


def page_environment(problems):
    found = env_vars()
    pages = doc_pages()
    b = [HEAD, "# Environment variables\n\n",
         "The variables D-Modem's programs read, and those its PBX modules "
         "set for the programs they run, as the source has them. Each "
         "links the pages that describe it.\n\n",
         "| variable | | described in | in the code |\n|---|---|---|---|\n"]
    for name in sorted(found):
        v = found[name]
        kind = "read" if v["read"] else "set for a program"
        where = sorted(v["read"] or v["set"])
        docs = [f for f, t in pages.items() if re.search(r"\b%s\b" % name, t)]
        if not docs and name not in INTERNAL:
            problems.append("%s is in no page of docs/ (%s)" % (name, ", ".join(where)))
        links = ", ".join("[%s](../%s)" % (title(pages[f], f), f) for f in docs)
        if not links:
            links = "tests only" if name in INTERNAL else ""
        code = ", ".join("`%s`" % w for w in where[:3]) + (" ..." if len(where) > 3 else "")
        b.append("| `%s` | %s | %s | %s |\n" % (name, kind, links, code))
    return "".join(b)


def main():
    check = "--check" in sys.argv[1:]
    problems = []
    pages = {
        "dmodemd.md": page_dmodemd(),
        "cmake.md": page_cmake(),
        "environment.md": page_environment(problems),
    }
    os.makedirs(OUT, exist_ok=True)
    for name, text in pages.items():
        path = os.path.join(OUT, name)
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old == text:
            continue
        if check:
            problems.append("docs/reference/%s is out of date: run tools/site_reference.py" % name)
        else:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
    for p in problems:
        print("site_reference: " + p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
