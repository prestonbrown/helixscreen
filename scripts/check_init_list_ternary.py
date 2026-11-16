#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint: no initializer_list picked by a ternary is stored past its expression.
#
# An initializer_list is a view of a temporary array that dies with the full
# expression that makes it. One picked by a ternary and then stored (a variable,
# a return value) is a view of freed stack, and whether that crashes depends on
# what reuses the slot, so no unit test catches it. Passed straight to a call it
# lives for that call and is fine. Opt-out: // INIT_LIST_TERNARY_OK: <reason>.
#
# Usage: check_init_list_ternary.py FILE...   (prints file:line: text per offender)

import re
import sys

OPERAND = re.compile(r'(\?|(?<!:):(?!:))\s*std\s*::\s*initializer_list\s*<')

def scrub(text):
    """Blank comments and literal contents, keeping offsets and newlines."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + re.sub(r'[^\n]', ' ', text[i + 1:j]) + c); i = j + 1
        else:
            out.append(c); i += 1
    return ''.join(out)

NAME_BEFORE = re.compile(r'([A-Za-z_]\w*)\s*$')
CTOR_INIT = re.compile(r'\)\s*(?:noexcept\s*)?:(?!:)')
NOT_A_NAME = {'if', 'while', 'for', 'switch', 'return', 'else', 'do', 'try', 'sizeof'}

def named_opener(text, idx):
    """The identifier right before the bracket at idx, or None."""
    m = NAME_BEFORE.search(text, 0, idx)
    if not m or m.group(1) in NOT_A_NAME:
        return None
    return m

def brace_initializer(text, idx):
    """`name{` - a variable or member initialised by braces."""
    return named_opener(text, idx) is not None

def copied_into_owner(decl):
    """A declaration whose own type is not an initializer_list (std::vector<T> v)
    copies the elements while the temporary array still lives."""
    decl = decl.strip()
    if 'initializer_list' in decl or re.search(r'\bauto\b', decl):
        return False
    return re.search(r'[\w>]\s*[&*]?\s+[A-Za-z_]\w*\s*$', decl) is not None

def member_initializer(text, start, idx):
    """`name(` inside a constructor's member-initializer list."""
    m = named_opener(text, idx)
    if m is None:
        return False
    before = text[start:m.start()].rstrip()
    return before.endswith((':', ',')) and not before.endswith('::') and \
        CTOR_INIT.search(before) is not None

for path in sys.argv[1:]:
    try:
        raw = open(path, encoding='utf-8', errors='replace').read()
    except OSError:
        continue
    # scrub() keeps every code character in place, so OPERAND cannot match in
    # a file that never spells initializer_list; skip it before the scrub.
    if 'initializer_list' not in raw:
        continue
    text = scrub(raw)
    raw_lines = raw.split('\n')
    seen = set()
    for m in OPERAND.finditer(text):
        # The statement holding the ternary starts after the last ; or unmatched
        # {, stepping over the braces of the lists themselves.
        start, depth = m.start(), 0
        while start > 0:
            ch = text[start - 1]
            if ch == '}':
                depth += 1
            elif ch == '{':
                if depth == 0:
                    break
                depth -= 1
            elif ch == ';' and depth == 0:
                break
            start -= 1
        if start in seen:
            continue
        seen.add(start)
        stmt = text[start:m.start()]
        opens = []
        for i, ch in enumerate(stmt):
            if ch == '(':
                opens.append(start + i)
            elif ch == ')' and opens:
                opens.pop()
        if opens:
            # Inside parentheses: a call argument lives for that call, but a
            # constructor's member initializer `: keys_(c ? ...)` stores it.
            if not member_initializer(text, start, opens[-1]):
                continue
        elif start > 0 and text[start - 1] == '{' and brace_initializer(text, start - 1):
            if copied_into_owner(text[text.rfind(';', 0, start) + 1:start - 1]):
                continue
        else:
            store = re.search(r'(?<![=!<>])=(?!=)|\breturn\b', stmt)
            if not store:
                continue
            if store.group(0) == '=' and copied_into_owner(stmt[:store.start()]):
                continue
        line = text.count('\n', 0, m.start()) + 1
        span = raw_lines[text.count('\n', 0, start):line + 2]
        if any('INIT_LIST_TERNARY_OK' in s for s in span):
            continue
        print(f'{path}:{line}: {raw_lines[line - 1].strip()}')
