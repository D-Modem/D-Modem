#!/usr/bin/env python3
#
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
#    doc_check.py - every function defined in the project's C and C++
#                   sources has a comment on the line just above it (or
#                   above its template <...> line)
#
#    usage: doc_check.py SOURCE_DIR [FILE...]
#
#    Without FILEs, the sources are git's tracked *.c, *.cpp, *.h and
#    *.hpp (or, outside a git checkout, those under the source
#    directories).  Prints each undocumented definition as file:line and
#    exits 1 if there are any.
#
#    A definition is a '{' at file, namespace or class scope that follows
#    a head with a name and a '(' in it (not an initializer, enum, class,
#    or namespace).  Comments, strings and preprocessor lines are blanked
#    first, so a definition inside a #define is not seen.
#

import os
import re
import subprocess
import sys

DIRS = ('ari', 'asterisk', 'compat', 'core', 'dmodemd', 'dsp', 'freeswitch',
        'link', 'pbx')
EXTS = ('.c', '.cpp', '.h', '.hpp')
KEYWORDS = re.compile(r'^(if|for|while|switch|catch|return|sizeof|decltype|'
                      r'alignof|alignas|static_assert|__attribute__|'
                      r'__declspec|defined)$')


def blank(text):
    """text with comments, string and character literals, and preprocessor
    lines turned into spaces (newlines kept, so offsets and lines hold)"""
    out = []
    i, n = 0, len(text)
    bol = True
    while i < n:
        c = text[i]
        if bol and c in ' \t':
            out.append(c)
            i += 1
            continue
        if bol and c == '#':
            j = i
            while j < n:
                if text.startswith('/*', j):
                    j = text.find('*/', j + 2)
                    j = n if j < 0 else j + 2
                    continue
                if text.startswith('//', j):
                    j = text.find('\n', j)
                    j = n if j < 0 else j
                    break
                if text[j] == '\n' and text[j - 1] != '\\':
                    break
                j += 1
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
            continue
        bol = c == '\n'
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
            continue
        m = re.match(r'R"([^(]*)\(', text[i:i + 20])
        if m:
            end = ')' + m.group(1) + '"'
            j = text.find(end, i) + len(end)
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
            continue
        if c in '"\'':
            # (a digit separator, 1'000, is no character literal)
            if c == "'" and i > 0 and re.match(r'[0-9a-fA-F]', text[i - 1]) \
                    and re.match(r'[0-9a-fA-F]', text[i + 1:i + 2]):
                out.append(' ')
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + re.sub(r'[^\n]', ' ', text[i + 1:j]) + c)
            i = j + 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def unparen(h):
    """h with what is inside each pair of parentheses removed"""
    while True:
        g = re.sub(r'\([^()]*\)', '()', h)
        if g == h:
            return g
        h = g


def after_templates(s, k, end):
    """the offset in s past the template <...> heads from k"""
    while True:
        while k < end and s[k].isspace():
            k += 1
        if not (s.startswith('template', k) and
                not re.match(r'\w', s[k + 8:k + 9])):
            return k
        j = s.find('<', k)
        depth = 0
        while j < end:
            if s[j] == '<':
                depth += 1
            elif s[j] == '>':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        k = j + 1


def scope_of(h, outer):
    """what a '{' after head h opens in scope outer: 'ns' (namespace or
    extern "C"), 'class', 'fn' (a function's body) or 'other'"""
    if outer not in ('ns', 'class'):
        return 'other'
    if re.search(r'\bnamespace\b', h) and '(' not in h:
        return 'ns'
    if re.match(r'^extern\s*"\s*"$', h):
        return 'ns'
    bare = re.sub(r'(alignas|__attribute__|__declspec)\s*\(\(?[^)]*\)?\)',
                  '', h)
    # (class, struct or union before any '(' and no function's ')' at the
    # end: struct modem *f(void) and f(struct modem *m) are functions;
    # struct x a[] = { is a table)
    if '=' not in unparen(h) and re.search(r'\b(class|struct|union)\b', bare.split('(')[0]) and \
            '(' not in bare.split(':')[0] and \
            not re.search(r'\)\s*(const\s*)?(noexcept\s*)?'
                          r'(override|final)?\s*$', bare):
        return 'class'
    if re.search(r'\benum\b', h) and '(' not in h:
        return 'other'
    q = unparen(h)
    if (re.search(r'=\s*$|=\s*\{|\[\]\s*=', q) or
            re.search(r'=\s*[\w:<>, ]*$', q)) and \
            not re.search(r'operator\s*\S*=', h):
        return 'other'
    names = [m for m in re.finditer(
        r'(~?[A-Za-z_]\w*|operator\s*(?:\(\)|[^\s(\w]+|\w+))\s*\(', h)
        if not KEYWORDS.match(m.group(1).strip())]
    if names and not h.startswith(':') and \
            not re.search(r'=\s*(default|delete)\s*$', h):
        return 'fn'
    return 'other'


def definitions(text):
    """the line of each function definition in text (its head's first
    line, past any template <...>)"""
    s = blank(text)
    lines = []
    scopes = []
    start = 0
    paren = 0
    for i, c in enumerate(s):
        if c == '(':
            paren += 1
        elif c == ')':
            paren -= 1
        elif paren == 0 and c in ';{}':
            if c == '{':
                k = after_templates(s, start, i)
                h = ' '.join(s[k:i].split())
                kind = scope_of(h, scopes[-1] if scopes else 'ns')
                if kind == 'fn':
                    lines.append(text.count('\n', 0, k) + 1)
                scopes.append(kind)
            elif c == '}' and scopes:
                scopes.pop()
            start = i + 1
        elif paren == 0 and c == ':' and scopes and scopes[-1] == 'class' \
                and re.match(r'\s*(public|private|protected)\s*$',
                             s[start:i]):
            start = i + 1
    return lines


def documented(lines, line):
    """whether the definition at line (1-based) has a comment above it"""
    i = line - 2
    while i >= 0 and re.match(r'\s*template\s*<', lines[i]):
        i -= 1
    prev = lines[i].strip() if i >= 0 else ''
    return prev.endswith('*/') or prev.startswith('//')


def sources(root):
    """the project's C and C++ sources, relative to root"""
    try:
        out = subprocess.run(['git', 'ls-files', '--'] +
                             ['*' + e for e in EXTS], cwd=root,
                             capture_output=True, text=True, check=True)
        files = out.stdout.split()
        if files:
            return files
    except (OSError, subprocess.CalledProcessError):
        pass
    files = []
    for d in DIRS:
        for top, _, names in os.walk(os.path.join(root, d)):
            files += [os.path.relpath(os.path.join(top, f), root)
                      for f in names if f.endswith(EXTS)]
    return sorted(files)


def main():
    if len(sys.argv) < 2:
        print(__doc__ or 'usage: doc_check.py SOURCE_DIR [FILE...]')
        return 2
    root = sys.argv[1]
    files = sys.argv[2:] or sources(root)
    bad = 0
    for f in files:
        with open(os.path.join(root, f), errors='replace') as fp:
            text = fp.read()
        lines = text.split('\n')
        for line in definitions(text):
            if not documented(lines, line):
                print(f'{f}:{line}: undocumented: {lines[line - 1].strip()}')
                bad += 1
    print(f'{len(files)} files, {bad} undocumented function definitions')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
