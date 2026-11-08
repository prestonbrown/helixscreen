#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: a `queue_update()` lambda must not capture a raw `this`.
#
# `helix::ui::queue_update([this, …]{ … })` is safe only while the capturing
# object outlives the drain. UpdateQueue holds the lambda until the next
# process_pending() tick, which is an unbounded amount of time later and on a
# different thread's schedule. If the owner is destroyed — or merely runs
# deinit_subjects() — before that tick, the callback executes against freed
# memory, and if it touches a member lv_subject_t then lv_subject_notify()
# walks a freed observer list.
#
# That is #1146: MoonrakerAPI::notify_build_volume_changed() queued such a
# lambda, ~MoonrakerAPI deinited the subject, and a *later, unrelated* test
# drained the corpse. The SIGSEGV lands on whichever test happened to drain
# next, which is why it cost days rather than minutes to find.
#
# THE CORRECT FORMS (docs/devel/THREADING.md §2)
#
#   lifetime_.bg_cb("Class::method", [this](const Resp& r) { member_ = r; })
#   tok.defer("Class::method", [this, r]() { member_ = r; })
#
# Both route through an AsyncLifetimeGuard token, whose shared generation
# counter stays valid after the owner is freed — so the callback is dropped
# instead of run. Neither goes through queue_update() textually, so neither is
# matched here.
#
# WHAT COUNTS AS A VIOLATION
#   A lambda passed DIRECTLY as an argument to queue_update(...) whose capture
#   list reaches `this`:
#     [this, …]     explicit
#     [*this, …]    explicit copy — still a full member snapshot
#     [&]  [&, x]   default capture by reference
#     [=]  [=, &x]  default capture by copy (captures the `this` POINTER)
#
#   Every namespace spelling is matched — `helix::ui::queue_update`,
#   `ui::queue_update`, bare `queue_update` — as are the tagged
#   (`queue_update("tag", fn)`) and templated (`queue_update<T>(data, fn)`)
#   overloads, where the lambda is not the first argument.
#
# NOT FLAGGED
#   - A capture list with no path to `this`: [], [x], [&x], [x = std::move(x)].
#     Those are the fix target, not the bug.
#   - Lambdas nested inside the queued lambda's BODY. They cannot see `this`
#     unless the outer one captured it, and if it did the outer is the finding.
#   - `lifetime_.bg_cb(...)`, `tok.defer(...)`, `guard_.defer(...)` — no
#     queue_update token, so they never enter the scan.
#   - Anything inside a comment or a string literal.
#
# This is a RATCHET, same shape as check_imperative_ui.py. ~35 sites predate
# the gate; they survive today mostly because their owners happen to be
# process-lifetime singletons, which is a property of the current call graph
# and not a guarantee. --max-allowed freezes the count so it can only fall.
#
# Per-lambda opt-out — on the line, or on a `//` comment line directly above:
#   helix::ui::queue_update([this]() { … });  // QUEUE_RAW_THIS_OK: <reason>
#
# Usage:
#   ./scripts/check_raw_this_queue_update.py                 # fail on any
#   ./scripts/check_raw_this_queue_update.py --max-allowed 35 # ratchet
#   ./scripts/check_raw_this_queue_update.py --summary        # counts only
#   ./scripts/check_raw_this_queue_update.py --list           # every site
#   ./scripts/check_raw_this_queue_update.py --staged-only
#   ./scripts/check_raw_this_queue_update.py src/printer/printer_state.cpp

from __future__ import annotations

import argparse
import bisect
import os
import re
import subprocess
import sys
from pathlib import Path

from staged_content import read_index_blobs, staged_paths

from source_strip import blank_cxx

SCAN_DIRS = ('src',)
SCAN_EXTS = ('.cpp', '.cc', '.h', '.hpp')

OPT_OUT = 'QUEUE_RAW_THIS_OK'

# `helix::ui::queue_update(`, `ui::queue_update(`, `queue_update(`, plus the
# templated overload `queue_update<T>(`. The leading \b is what keeps
# `ui_queue_update` out — `_` is a word character, so there is no boundary
# before `queue` in that spelling. (It appears only in prose comments anyway.)
CALL_RE = re.compile(r'\bqueue_update\s*(?:<[^<>(){};]*>)?\s*\(')

# How far past the opening paren to look for the closing one. A queue_update
# call body longer than this is pathological; the cap just stops an unbalanced
# paren (inside a macro, say) from scanning to EOF.
MAX_CALL_SPAN = 8000

# Capture forms that reach `this`, in the order they are reported.
KINDS = ('this', '*this', '[&]', '[=]')

WHY = {
    'this': 'raw `this` capture',
    '*this': '`*this` capture (whole-object copy)',
    '[&]': 'default capture by reference — reaches `this`',
    '[=]': 'default capture by copy — captures the `this` POINTER',
}

FIX = ('lifetime_.bg_cb("Class::method", fn) or tok.defer("Class::method", fn) '
       '— see docs/devel/THREADING.md §2')


def _match_bracket(s: str, start: int) -> int:
    """Index of the `]` closing the `[` at `start`, or -1."""
    depth = 0
    for i in range(start, len(s)):
        if s[i] == '[':
            depth += 1
        elif s[i] == ']':
            depth -= 1
            if depth == 0:
                return i
    return -1


def _split_top_level(capture: str) -> list[str]:
    """Split a capture list on its top-level commas.

    Only ()/[]/{} are tracked. A `<>` template comma can split an init-capture
    in two, which is harmless: neither fragment can equal `this`, `*this`, `&`
    or `=`, and those four are the only tokens that matter."""
    parts: list[str] = []
    depth = 0
    cur = []
    for ch in capture:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(''.join(cur))
            cur = []
            continue
        cur.append(ch)
    parts.append(''.join(cur))
    return parts


def _capture_kind(capture: str) -> str | None:
    """Which of KINDS this capture list uses to reach `this`, or None.

    A bare `&` or `=` element is the DEFAULT capture. `&x` / `x = expr` are an
    ordinary by-reference capture and an init-capture — neither reaches `this`,
    and conflating them would flag the fixed form and get the gate switched
    off."""
    for part in _split_top_level(capture):
        s = part.strip()
        if s == 'this':
            return 'this'
        if s.replace(' ', '') == '*this':
            return '*this'
        if s == '&':
            return '[&]'
        if s == '=':
            return '[=]'
    return None


def _direct_lambdas(clean: str, open_paren: int) -> list[tuple[int, str]]:
    """[(offset of `[`, capture text)] for every lambda passed DIRECTLY as an
    argument of the call whose `(` is at `open_paren`.

    Direct means paren-depth 1 and brace-depth 0 relative to that paren: an
    argument slot, not somewhere inside another lambda's body. A lambda nested
    in the queued body sits at brace-depth > 0 and is skipped — it can only see
    `this` if the enclosing lambda captured it, in which case the enclosing one
    is already the finding."""
    found: list[tuple[int, str]] = []
    limit = min(len(clean), open_paren + MAX_CALL_SPAN)
    depth = 0
    brace = 0
    i = open_paren
    while i < limit:
        c = clean[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                break
        elif c == '{':
            brace += 1
        elif c == '}':
            brace -= 1
        elif c == '[' and depth == 1 and brace == 0:
            close = _match_bracket(clean, i)
            if close > 0:
                k = close + 1
                while k < len(clean) and clean[k].isspace():
                    k += 1
                # A lambda introducer is followed by its parameter list or,
                # when it takes none, straight by the body. Anything else
                # (`,`, `)`, an operator) is an array subscript.
                if k < len(clean) and clean[k] in '({':
                    found.append((i, clean[i + 1:close]))
                    i = close
        i += 1
    return found


def _opted_out(lines: list[str], ln: int) -> bool:
    """OPT_OUT on line `ln` (1-based), or on a `//` comment line directly above.

    Requiring the `//` on the line above is what stops a trailing comment from
    leaking down onto the next statement — same rule as the XML gate's
    `<!--` check."""
    if 0 < ln <= len(lines) and OPT_OUT in lines[ln - 1]:
        return True
    prev = lines[ln - 2] if ln >= 2 else ''
    return OPT_OUT in prev and prev.lstrip().startswith('//')


def scan_file(path: str) -> list[tuple[str, int, str, str]]:
    """[(path, line, kind, source line)] for each offending lambda."""
    try:
        src = Path(path).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    return scan_source(path, src)


def scan_source(path: str, src: str) -> list[tuple[str, int, str, str]]:
    """scan_file's body, operating on already-sourced text.

    Split out so a caller holding the STAGED blob (index content, not
    necessarily what sits on disk) can scan that directly. `path` is only
    carried into the report.
    """
    if 'queue_update' not in src:
        return []

    clean = blank_cxx(src)
    lines = src.splitlines()
    starts = [0]
    for i, ch in enumerate(src):
        if ch == '\n':
            starts.append(i + 1)

    def line_of(off: int) -> int:
        return bisect.bisect_right(starts, off)

    hits: list[tuple[str, int, str, str]] = []
    for m in CALL_RE.finditer(clean):
        call_ln = line_of(m.start())
        for intro, capture in _direct_lambdas(clean, m.end() - 1):
            kind = _capture_kind(capture)
            if not kind:
                continue
            intro_ln = line_of(intro)
            if _opted_out(lines, call_ln) or _opted_out(lines, intro_ln):
                continue
            text = lines[intro_ln - 1].strip() if intro_ln <= len(lines) else ''
            hits.append((path, intro_ln, kind, text))
    return hits


def staged_targets() -> list[str]:
    return [p for p in staged_paths(suffixes=SCAN_EXTS) if p.startswith(SCAN_DIRS)]


def walk_targets() -> list[str]:
    targets: list[str] = []
    for d in SCAN_DIRS:
        for root, _, files in os.walk(d):
            if 'build/' in root + '/':
                continue
            targets += [os.path.join(root, f) for f in files if f.endswith(SCAN_EXTS)]
    return targets


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--max-allowed', type=int, default=None,
                    help='Pass if total sites <= N (ratcheting baseline). '
                         'Default: fail on any. Only meaningful on a full scan.')
    ap.add_argument('--summary', action='store_true', help='Per-capture-form counts only')
    ap.add_argument('--list', action='store_true', help='Print every site, file:line')
    ap.add_argument('--staged-only', action='store_true',
                    help='Scan only staged src/ sources (pre-commit). Partial scan — '
                         'do not combine with --max-allowed.')
    ap.add_argument('paths', nargs='*', help=f'Files to scan (default: {"/".join(SCAN_DIRS)}/)')
    args = ap.parse_args()

    if args.staged_only:
        targets = staged_targets()
    elif args.paths:
        targets = [p for p in args.paths if p.endswith(SCAN_EXTS) and os.path.isfile(p)]
    else:
        repo_root = subprocess.run(
            ['git', 'rev-parse', '--show-toplevel'],
            capture_output=True, text=True, check=False,
        ).stdout.strip()
        if repo_root:
            os.chdir(repo_root)
        targets = walk_targets()

    hits: list[tuple[str, int, str, str]] = []
    if args.staged_only:
        staged_src = dict(read_index_blobs(targets))
        for path in sorted(targets):
            src = staged_src.get(path)
            if src is not None:
                hits += scan_source(path, src)
    else:
        for path in sorted(targets):
            hits += scan_file(path)
    hits.sort(key=lambda h: (h[0], h[1]))

    by_kind: dict[str, int] = {}
    for _, _, kind, _ in hits:
        by_kind[kind] = by_kind.get(kind, 0) + 1
    total = len(hits)

    if args.list:
        for path, ln, kind, text in hits:
            print(f'{path}:{ln}: [{kind}] {text}')
        print()

    if args.summary or args.list:
        for kind in KINDS:
            print(f'  {kind:<7} {by_kind.get(kind, 0):>5}   {WHY[kind]}')
        print(f'  {"TOTAL":<7} {total:>5}')

    limit = args.max_allowed
    if limit is None:
        if total:
            print(f'❌ Raw `this` in queue_update: {total} site(s). The lambda runs at the '
                  f'next drain whether or not the owner is still alive (#1146).')
            print('   NOT every hit is a live use-after-free. A body whose first statement '
                  'is `if (tok.expired()) return;` is already safe — the token holds a '
                  'shared_ptr to the generation counter, so it stays valid after the owner '
                  'dies, and `this` is never dereferenced before the check. Those count as '
                  'debt to migrate to the sanctioned form, not as bugs. This gate cannot '
                  'see the guard, nor tell a singleton owner from a short-lived one.')
            if not (args.list or args.summary):
                for path, ln, kind, _ in hits[:20]:
                    print(f'   {path}:{ln}: [{kind}]')
                if total > 20:
                    print(f'   … and {total - 20} more')
            print(f'   Fix: {FIX}')
            print(f'   Suppress per-lambda: // {OPT_OUT}: <reason>')
            return 1
        print('✅ Raw `this` in queue_update: none.')
        return 0

    if total > limit:
        print(f'❌ Raw `this` in queue_update: {total} sites exceeds baseline ({limit}).')
        print(f'   New code must guard the callback: {FIX}')
        print('   Run: python3 scripts/check_raw_this_queue_update.py --list')
        return 1
    if total < limit:
        print(f'✅ Raw `this` in queue_update: {total} (baseline {limit} — '
              f'ratchet the baseline down)')
        return 0
    print(f'✅ Raw `this` in queue_update: {total} == baseline ({limit})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
