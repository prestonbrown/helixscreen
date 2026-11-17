#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lint: no `return` inside a Catch2 SECTION that has a later sibling.

Catch2 finds sections lazily: each run of a test case enters one section it
has not finished, and the SECTION macros the run passes on its way are what
tell Catch the others exist. A `return` inside a section ends the run before
the later sibling SECTIONs are reached, so Catch never learns of them and the
case passes having run none of them. A return in a nested section hides the
later siblings of every section around it the same way.

A return in the last section of its scope, outside every section, or inside a
lambda or local function body is fine. Siblings are sections with the same
enclosing section (or, at the top, the same test case body), whatever plain
blocks sit between. GIVEN/WHEN/THEN/AND_* and DYNAMIC_SECTION are sections too.
Opt-out on the return's line or the line above: // SECTION_RETURN_OK: <reason>

Usage: check_section_returns.py FILE...   (prints file:line: text per offender)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from source_strip import blank_cxx  # noqa: E402

SECTION = re.compile(
    r'\b(?:DYNAMIC_SECTION|SECTION|GIVEN|WHEN|THEN|AND_GIVEN|AND_WHEN|AND_THEN)\s*\(')
RETURN = re.compile(r'\breturn\b')
IDENT_BEFORE = re.compile(r'([A-Za-z_]\w*)\s*$')
CONTROL = {'if', 'while', 'for', 'switch', 'catch'}
# What may sit between a lambda's or function's parameter list and its body.
SPECIFIERS = re.compile(
    r'(?:\b(?:mutable|noexcept|const|override|final|constexpr)\b\s*|->\s*[\w:<>,\s*&]+?)*$')
HATCH = 'SECTION_RETURN_OK'


def bracket_pairs(text: str, open_ch: str, close_ch: str) -> dict[int, int]:
    """Map each bracket, open and close, to its partner."""
    pairs, stack = {}, []
    for m in re.finditer(re.escape(open_ch) + '|' + re.escape(close_ch), text):
        i = m.start()
        if text[i] == open_ch:
            stack.append(i)
        elif stack:
            j = stack.pop()
            pairs[i], pairs[j] = j, i
    return pairs


def is_function_body(text: str, brace: int, parens: dict[int, int]) -> bool:
    """A `{` opening a lambda or local function body: `return` leaves only it."""
    window = max(0, brace - 160)
    head = SPECIFIERS.search(text[window:brace])
    k = (window + head.start() if head else brace) - 1
    while k >= 0 and text[k].isspace():
        k -= 1
    if k < 0:
        return False
    if text[k] == ']':
        return True  # [captures] { ... }
    if text[k] != ')' or k not in parens:
        return False
    p = parens[k]
    q = p - 1
    while q >= 0 and text[q].isspace():
        q -= 1
    if q >= 0 and text[q] == ']':
        return True  # [captures](args) { ... }
    m = IDENT_BEFORE.search(text[max(0, p - 80):p])
    return m is not None and m.group(1) not in CONTROL


def scan(path: str, raw: str) -> list[str]:
    text = blank_cxx(raw)
    braces = bracket_pairs(text, '{', '}')
    parens = bracket_pairs(text, '(', ')')

    # Each section as (body open, body close).
    sections = []
    for m in SECTION.finditer(text):
        close_paren = parens.get(m.end() - 1)
        if close_paren is None:
            continue
        k = close_paren + 1
        while k < len(text) and text[k].isspace():
            k += 1
        if k < len(text) and text[k] == '{' and k in braces:
            sections.append((k, braces[k]))  # else a call, not a section block
    if not sections:
        return []

    def enclosing_sections(pos: int) -> list[tuple[int, int]]:
        return [s for s in sections if s[0] < pos < s[1]]

    def outermost_block(pos: int) -> int:
        """Start of the outermost brace block around pos: the test case body."""
        start = pos
        for o, c in braces.items():
            if text[o] == '{' and o < pos < c and o < start:
                start = o
        return start

    parent = {}
    for s in sections:
        around = enclosing_sections(s[0])
        parent[s] = max(around)[0] if around else outermost_block(s[0])
    later_sibling = {
        s: any(o[0] > s[1] and parent[o] == parent[s] for o in sections) for s in sections}

    # Lambda and local function bodies: a return there leaves that function,
    # not the test case.
    functions = [(b, c) for b, c in braces.items()
                 if b < c and text[b] == '{' and enclosing_sections(b)
                 and (b, c) not in sections and is_function_body(text, b, parens)]

    raw_lines = raw.split('\n')
    out = []
    for m in RETURN.finditer(text):
        pos = m.start()
        around = enclosing_sections(pos)
        if not any(later_sibling[s] for s in around):
            continue
        inner = max(around)
        if any(inner[0] < b < pos < c for b, c in functions):
            continue
        line = text.count('\n', 0, pos) + 1
        if any(HATCH in raw_lines[n] for n in (line - 2, line - 1) if n >= 0):
            continue
        out.append(f'{path}:{line}: {raw_lines[line - 1].strip()}')
    return out


def main(argv: list[str]) -> int:
    for path in argv:
        try:
            raw = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        if 'return' not in raw or not SECTION.search(raw):
            continue
        for line in scan(path, raw):
            print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
