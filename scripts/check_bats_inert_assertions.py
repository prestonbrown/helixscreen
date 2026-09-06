#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: a bats assertion that bash 3.2 silently swallows.
#
# macOS ships bash 3.2, and bash 3.2 does not apply `set -e` to a failing
# `[[ ]]` or `(( ))`. bats runs every @test body under errexit, so only the
# LAST statement of a body decides the verdict there: an earlier
# `[[ "$output" == *"x"* ]]` can be false and the test still reports `ok`.
# Linux CI runs bash 5, where the same line fails the test.
#
# The two hosts therefore disagree about what a test pins, and the Mac is the
# one that reports green. A test can be written, run locally, and shipped while
# asserting nothing - which is worse than having no test, because the suite
# counts it.
#
# What is FINE, and must stay quiet:
#   [[ ... ]]                     as the last statement of its body (its status
#                                 becomes the body's status, honoured anywhere)
#   [[ ... ]] || fail "msg"       the list ends in a simple command
#   [[ ... ]] || { ...; return 1; }
#   [[ ... ]] && continue         control flow, not an assertion
#   if [[ ... ]]; then            a condition, not an assertion
#   contains / lacks / [ ... ]    already honoured everywhere
#
# What is FLAGGED: a bare `[[ ]]` / `(( ))` / `! [[ ]]` standing as its own
# statement somewhere other than last in a @test body.
#
# The fix is `contains` / `lacks` from tests/shell/helpers.bash for a substring,
# or keeping the condition and appending `|| fail "..."` for anything else.
#
# Two body parsers, and the answer is the union
# ---------------------------------------------
# Deciding "is this the last statement of its @test body" needs the body's
# extent, and a single rule gets it wrong in a way that hides sites:
#
#   brace depth      a `}` inside a heredoc or a multi-line quoted C++ fixture
#                    closes the body early
#   ends at lone `}` the same heredocs end it early
#
# Both run over a quote- and heredoc-aware view of the file, which removes most
# of the disagreement, and a line is reported only when NEITHER parser can call
# it final. A line no parser can place in a body is counted as unplaced and
# reported, never silently dropped - a parse that quietly stops seeing code is
# exactly how a gate comes back clean having examined nothing.

import argparse
import os
import re
import subprocess
import sys

HEREDOC = re.compile(r'<<-?\s*(["\']?)([A-Za-z_][A-Za-z0-9_]*)\1')
TEST_OPEN = re.compile(r'^\s*@test\s+.*\{\s*$')
LONE_CLOSE = re.compile(r'^\}\s*$')
CLOSER = re.compile(r'^\s*(\}|fi|done|esac|;;|else|elif\b.*|\S+\))\s*$')
# a statement whose final command is an errexit-exempt compound
BARE = re.compile(r'^\s*(!\s+)?(\[\[.*\]\]|\(\(.*\)\))\s*$')
COND = re.compile(r'^\s*(if|elif|while|until)\b')
GUARD = re.compile(r'\|\|\s*(\{|fail\b|return\b|exit\b|false\b|continue\b|break\b|skip\b)')
CTRL = re.compile(r'&&\s*(continue|break|skip|return)\b|\|\|\s*(continue|break)\b')


def code_view(lines):
    """Blank out quoted spans, comments and heredoc bodies.

    Returns (view, is_code) per line. `view` keeps the line's length and its
    structural characters ({ } etc.) while everything inside a string, comment
    or heredoc becomes a space, so a `}` in a C++ fixture cannot close a body.
    """
    view, is_code = [], []
    state = None          # None | "'" | '"'
    heredoc_term = None
    for raw in lines:
        if heredoc_term is not None:
            view.append(' ' * len(raw))
            is_code.append(False)
            if raw.strip() == heredoc_term:
                heredoc_term = None
            continue
        out = []
        i = 0
        pending_heredoc = None
        started_in_string = state is not None
        while i < len(raw):
            ch = raw[i]
            if state is None:
                if ch == '\\' and i + 1 < len(raw):
                    out.append('  ')
                    i += 2
                    continue
                if ch == '#' and (not out or raw[i - 1].isspace()):
                    out.append(' ' * (len(raw) - i))
                    break
                if ch == '<' and raw.startswith('<<', i):
                    # Capture the delimiter here, from the RAW text: quoting it
                    # (<<'EOF') is the common spelling, and blanking the quotes
                    # first would erase the name we need to find the end.
                    m = HEREDOC.match(raw, i)
                    if m:
                        pending_heredoc = m.group(2)
                        out.append(' ' * (m.end() - i))
                        i = m.end()
                        continue
                if ch in '"\'':
                    state = ch
                    out.append(' ')
                    i += 1
                    continue
                out.append(ch)
                i += 1
            else:
                if state == '"' and ch == '\\' and i + 1 < len(raw):
                    out.append('  ')
                    i += 2
                    continue
                if ch == state:
                    state = None
                out.append(' ')
                i += 1
        line_view = ''.join(out)
        view.append(line_view)
        is_code.append(not started_in_string)
        if state is None and pending_heredoc is not None:
            heredoc_term = pending_heredoc
    return view, is_code


def bodies_depth(lines, view):
    out, i, n = [], 0, len(lines)
    while i < n:
        if TEST_OPEN.match(lines[i]) and view[i].rstrip().endswith('{'):
            start, depth, i = i, 1, i + 1
            while i < n:
                depth += view[i].count('{') - view[i].count('}')
                if depth <= 0:
                    break
                i += 1
            out.append((start, i))
        i += 1
    return out


def bodies_lone_brace(lines, view):
    out, i, n = [], 0, len(lines)
    while i < n:
        if TEST_OPEN.match(lines[i]):
            start, i = i, i + 1
            while i < n and not LONE_CLOSE.match(view[i]):
                i += 1
            out.append((start, i))
        i += 1
    return out


def statement_starts(lines, view, is_code, lo, hi):
    """Indices in (lo, hi) that begin an executable statement, in order."""
    starts, continuing = [], False
    for i in range(lo + 1, min(hi, len(lines))):
        raw, v = lines[i], view[i]
        this_continues = v.rstrip().endswith('\\')
        if not continuing and is_code[i] and v.strip() and not CLOSER.match(raw):
            starts.append(i)
        continuing = this_continues
    return starts


def joined_statement(lines, view, i):
    """The whole logical line starting at i, for guard detection."""
    text, k = lines[i], i
    while view[k].rstrip().endswith('\\') and k + 1 < len(lines):
        k += 1
        text = text.rstrip()[:-1] + lines[k]
    return text


def scan(path, src):
    lines = src.split('\n')
    view, is_code = code_view(lines)
    parsers = [bodies_depth(lines, view), bodies_lone_brace(lines, view)]
    finals, spans = [], []
    for bl in parsers:
        f, s = {}, []
        for lo, hi in bl:
            st = statement_starts(lines, view, is_code, lo, hi)
            s.append((lo, hi))
            if st:
                f[st[-1]] = True
        finals.append(f)
        spans.append(s)

    hits, examined, unplaced = [], 0, 0
    for i, raw in enumerate(lines):
        # Match the code view, not the raw line: it has comments and string
        # bodies blanked, so a trailing `# note` cannot hide the assertion and
        # a bracket inside a quoted fixture cannot invent one.
        if not is_code[i] or not BARE.match(view[i]):
            continue
        if COND.match(view[i]):
            continue
        stmt = joined_statement(lines, view, i)
        if GUARD.search(stmt) or CTRL.search(stmt):
            continue
        placed = [any(lo < i < hi for lo, hi in s) for s in spans]
        if not any(placed):
            unplaced += 1
            continue
        examined += 1
        # honoured if EITHER parser can call it the body's last statement
        if any(f.get(i) for f in finals):
            continue
        hits.append((path, i + 1, raw.strip()))
    return hits, examined, unplaced


def collect(args):
    if args.paths:
        out = []
        for p in args.paths:
            if os.path.isdir(p):
                for root, _, files in os.walk(p):
                    out += [os.path.join(root, f) for f in files if f.endswith('.bats')]
            else:
                out.append(p)
        return sorted(out)
    if args.staged_only:
        try:
            r = subprocess.run(['git', 'diff', '--cached', '--name-only', '--diff-filter=ACM'],
                               capture_output=True, text=True, check=False)
            return sorted(f for f in r.stdout.split('\n')
                          if f.endswith('.bats') and os.path.exists(f))
        except OSError:
            return []
    base = 'tests/shell'
    if not os.path.isdir(base):
        return []
    return sorted(os.path.join(base, f) for f in os.listdir(base) if f.endswith('.bats'))


def main():
    ap = argparse.ArgumentParser(
        description='Find bats assertions that bash 3.2 swallows under errexit.')
    ap.add_argument('--list', action='store_true', help='Print every site')
    ap.add_argument('--summary', action='store_true', help='Per-file counts only')
    ap.add_argument('--max-allowed', type=int, default=None,
                    help='Ratchet baseline; fail only above it')
    ap.add_argument('--staged-only', action='store_true',
                    help='Only staged .bats files')
    ap.add_argument('paths', nargs='*', help='Files or directories (default tests/shell)')
    args = ap.parse_args()

    files = collect(args)
    hits, examined, unplaced, scanned = [], 0, 0, 0
    for path in files:
        try:
            with open(path, encoding='utf-8', errors='replace') as fh:
                src = fh.read()
        except OSError:
            continue
        scanned += 1
        h, e, u = scan(path, src)
        hits += h
        examined += e
        unplaced += u

    if args.list:
        for path, lineno, text in hits:
            print(f'{path}:{lineno}: {text}')
        if hits:
            print()

    if args.summary or args.list:
        per = {}
        for path, _, _ in hits:
            per[path] = per.get(path, 0) + 1
        for path in sorted(per, key=lambda p: (-per[p], p)):
            print(f'  {per[path]:>4}  {path}')
        print(f'  {"TOTAL":>4}  {len(hits)}')

    # Coverage, always. A gate that examined nothing must not read as clean.
    coverage = f'{examined} assertion(s) examined across {scanned} file(s)'
    if unplaced:
        coverage += f'; {unplaced} could not be placed in a @test body'

    if scanned == 0:
        print('❌ bats inert assertions: no .bats files examined - nothing was checked.')
        return 1

    total = len(hits)
    limit = args.max_allowed
    if limit is None:
        if total:
            print(f'❌ bats inert assertions: {total} assertion(s) bash 3.2 will swallow '
                  f'({coverage}).')
            print('   Only the LAST statement of a @test body fails the test on macOS.')
            print('   Use contains/lacks from tests/shell/helpers.bash, or append || fail "...".')
            print('   Run: python3 scripts/check_bats_inert_assertions.py --list')
            return 1
        print(f'✅ bats inert assertions: none ({coverage}).')
        return 0

    if total > limit:
        print(f'❌ bats inert assertions: {total} exceeds baseline ({limit}) ({coverage}).')
        print('   Use contains/lacks from tests/shell/helpers.bash, or append || fail "...".')
        return 1
    if total < limit:
        print(f'✅ bats inert assertions: {total} (baseline {limit} - ratchet it down) '
              f'({coverage}).')
        return 0
    print(f'✅ bats inert assertions: {total} == baseline ({limit}) ({coverage}).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
