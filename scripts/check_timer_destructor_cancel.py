#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: a class that owns a raw `lv_timer_t*` and cancels it somewhere must
# also cancel it from its destructor.
#
# StaticPanelRegistry::destroy_all() runs BEFORE lv_deinit() (application.cpp),
# so at destructor time an armed timer is still in LVGL's timer list holding a
# freed `this`. Cancelling only in cleanup()/stop_*()/detach() is latent-until-
# it-is-not: it needs one teardown path that destroys the object without going
# through the explicit stop. #1173 (wizard auto-probe timer) and the ETA timer in
# PIDCalibrationPanel were both exactly this, and auto_probe_timer_cb carries an
# ASAN-confirmed heap-use-after-free from the same shape.
#
# The check is transitive: a destructor that calls cleanup(), detach(), or
# deinit_subjects() counts if that method cancels the timer. FilamentPanel is the
# reason — ~FilamentPanel() never names op_revert_timer_, but deinit_subjects()
# on its first line cancels it, so it is correct and must not be flagged.
#
# Approved:
#
#   Foo::~Foo() { cancel_poll_timer(); }          // direct, or via any helper
#   void Foo::cancel_poll_timer() {
#       if (poll_timer_ && lv_is_initialized())
#           helix::ui::lv_timer_cancel_safe(poll_timer_);
#       poll_timer_ = nullptr;
#   }
#
#   Foo::~Foo() { detach(); }                     // transitive through detach()
#
# Flagged:
#
#   void Foo::cleanup()  { lv_timer_delete(poll_timer_); poll_timer_ = nullptr; }
#   Foo::~Foo() { /* no path that reaches the cancel */ }
#
# Per-member opt-out — put it on the member declaration or in the destructor:
#
#   lv_timer_t* poll_timer_ = nullptr; // TIMER_DTOR_OK: owned by the caller, see ...
#
# Prefer lv_timer_cancel_safe() over lv_timer_delete() in the shared helper: it
# self-guards on lv_is_initialized() and neuters instead of unlinking, so it is
# safe both from a destructor and from inside lv_timer_handler (#750, #751).
#
# Usage:
#   ./scripts/check_timer_destructor_cancel.py [files...]
#   ./scripts/check_timer_destructor_cancel.py --list
#   ./scripts/check_timer_destructor_cancel.py --max-allowed N   # ratcheting baseline

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from source_strip import blank_cxx

OPT_OUT = "TIMER_DTOR_OK"

# `some_timer_ = lv_timer_create(...)` — a member, not a local. Both member
# spellings in this tree count: the `trailing_underscore_` convention and the
# older `m_prefix` one. Matching only the former silently exempted
# FlyingToasterScreensaver::m_tick_timer, which had the identical
# cancel-only-in-stop() shape as the two screensavers this gate did flag.
# `lv_timer_t* t = lv_timer_create(...)` is still excluded by the type check.
ARM_RE = re.compile(
    r'(?:^|[^\w>.])(?:self->|this->)?(\w+_|m_\w+)\s*=\s*lv_timer_create\s*\('
)

# Cancelling a specific member. Both the raw delete and the safe neuter count —
# the gate is about WHERE the cancel happens, not which primitive it uses.
CANCEL_TMPL = r'\blv_timer_(?:delete|cancel_safe)\s*\(\s*(?:self->|this->)?{name}\s*\)'

# `Class::method(` / `Class::~Class(` — anywhere, since the return type usually
# precedes it on the same line (`void Foo::bar() {`). Whether it is a definition
# or a mere declaration is decided by what follows the closing paren.
METHOD_RE = re.compile(r'\b(\w+)::(~?\w+)\s*\(')

MAX_CALL_DEPTH = 5  # destructor -> cleanup() -> cancel_x() is 2; 5 is slack


def extract_bodies(text: str) -> dict[tuple[str, str], tuple[str, int]]:
    """Map (class, method) -> (body, 1-indexed line of the signature)."""
    bodies: dict[tuple[str, str], tuple[str, int]] = {}
    for m in METHOD_RE.finditer(text):
        cls, method = m.group(1), m.group(2)

        # Walk to the closing paren of the parameter list.
        depth, close = 0, None
        for i in range(m.end() - 1, len(text)):
            if text[i] == '(':
                depth += 1
            elif text[i] == ')':
                depth -= 1
                if depth == 0:
                    close = i
                    break
        if close is None:
            continue

        # A definition opens a brace before the next `;`. Anything else — a
        # declaration, a call — hits the semicolon first. Ctor initializer lists
        # sit between the two and contain no `;`, so they ride along fine.
        brace = text.find('{', close)
        semi = text.find(';', close)
        if brace == -1 or (semi != -1 and semi < brace):
            continue

        depth, end = 0, None
        for i in range(brace, len(text)):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    end = i + 1
                    break
        if end is None:
            continue
        bodies[(cls, method)] = (text[brace:end], text.count('\n', 0, m.start()) + 1)
    return bodies


def cancels(body: str, member: str) -> bool:
    return re.search(CANCEL_TMPL.format(name=re.escape(member)), body) is not None


def reaches_cancel(
    bodies: dict[tuple[str, str], tuple[str, int]], cls: str, entry: str, member: str
) -> bool:
    """BFS from `entry` over same-class method calls, looking for a cancel site."""
    seen, queue = set(), [(entry, 0)]
    while queue:
        method, depth = queue.pop(0)
        if (method, depth) in seen or depth > MAX_CALL_DEPTH:
            continue
        seen.add((method, depth))
        entry_body = bodies.get((cls, method))
        if entry_body is None:
            continue
        body = entry_body[0]
        if cancels(body, member):
            return True
        # Follow calls to other methods of the same class defined in this TU.
        for callee in re.findall(r'\b(\w+)\s*\(', body):
            if (cls, callee) in bodies and callee != method:
                queue.append((callee, depth + 1))
    return False


DECL_COMMENT_LOOKBACK = 8


def class_body(text: str, cls: str) -> str | None:
    """The brace-matched body of `class cls`, or None if not declared here."""
    pat = re.compile(r'\bclass\s+' + re.escape(cls) + r'\b')
    m = pat.search(text)
    while m:
        brace = text.find('{', m.end())
        semi = text.find(';', m.end())
        if brace != -1 and (semi == -1 or brace < semi):
            depth = 0
            for i in range(brace, len(text)):
                if text[i] == '{':
                    depth += 1
                elif text[i] == '}':
                    depth -= 1
                    if depth == 0:
                        return text[brace:i + 1]
            return text[brace:]
        m = pat.search(text, m.end())
    return None


_HEADER_TEXT: dict[Path, str] = {}


def header_text(path: Path) -> str:
    """A header's text, read once per run: every armed member asks of every header."""
    if path not in _HEADER_TEXT:
        _HEADER_TEXT[path] = path.read_text(encoding='utf-8', errors='replace')
    return _HEADER_TEXT[path]


def member_decl_lines(headers: list[Path], cls: str, member: str) -> list[str]:
    """Declaration lines for `cls::member`, each with the comment block above it.

    Scoped to the class body: member names repeat across classes (three separate
    classes own a plain `timer_`), and an unscoped search let one class's opt-out
    silence all of them.

    The opt-out reason is usually several lines long, so it sits in a comment
    block ABOVE the declaration rather than trailing it. Both must count.
    """
    out = []
    for h in headers:
        text = header_text(h)
        # A plain substring test first: the regex search over every header is the cost.
        if cls not in text:
            continue
        body = class_body(text, cls)
        if body is None:
            continue
        lines = body.splitlines()
        for i, line in enumerate(lines):
            if re.search(r'\b' + re.escape(member) + r'\b', line) and 'lv_timer_t' in line:
                start = i
                while start > 0 and start > i - DECL_COMMENT_LOOKBACK:
                    prev = lines[start - 1].strip()
                    if prev.startswith('//') or prev.startswith('*') or prev.startswith('/*'):
                        start -= 1
                    else:
                        break
                out.append('\n'.join(lines[start:i + 1]))
    return out


def scan(path: Path, headers: list[Path]) -> list[str]:
    raw = path.read_text(encoding='utf-8', errors='replace')
    # Every finding starts at an ARM_RE match, which blanking can only remove.
    if 'lv_timer_create' not in raw:
        return []
    # Analyse code only; keep the raw bodies alongside so the `// TIMER_DTOR_OK`
    # opt-out — which lives in a comment — is still visible.
    bodies = extract_bodies(blank_cxx(raw, strings="body"))
    raw_bodies = extract_bodies(raw)
    if not bodies:
        return []

    # Which timer members does each class arm?
    armed: dict[str, set[str]] = {}
    for (cls, method), (body, _) in bodies.items():
        for m in ARM_RE.finditer(body):
            name = m.group(1)
            # Skip locals: `lv_timer_t* foo_ = lv_timer_create(...)`.
            line_start = body.rfind('\n', 0, m.start()) + 1
            if 'lv_timer_t' in body[line_start:m.start()]:
                continue
            armed.setdefault(cls, set()).add(name)

    findings = []
    for cls, members in sorted(armed.items()):
        dtor = bodies.get((cls, f'~{cls}'))
        for member in sorted(members):
            # Only flag timers the class actually cancels somewhere — a timer that
            # is never cancelled at all is a different (and louder) problem.
            has_cancel = any(cancels(b, member) for (c, _), (b, _l) in bodies.items() if c == cls)
            if not has_cancel:
                continue
            decls = member_decl_lines(headers, cls, member)
            if any(OPT_OUT in d for d in decls):
                continue
            if dtor is None:
                findings.append(
                    f'{path}: {cls}::{member} — cancelled in {path.name}, '
                    f'but {cls} defines no destructor in this TU'
                )
                continue
            raw_dtor = raw_bodies.get((cls, f'~{cls}'))
            if raw_dtor and OPT_OUT in raw_dtor[0]:
                continue
            if not reaches_cancel(bodies, cls, f'~{cls}', member):
                findings.append(
                    f'{path}:{dtor[1]}: {cls}::{member} — cancelled elsewhere in the class '
                    f'but not reachable from ~{cls}()'
                )
    return findings


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('files', nargs='*', help='files to scan (default: src/ recursively)')
    ap.add_argument('--list', action='store_true', help='print every finding')
    ap.add_argument('--max-allowed', type=int, default=0,
                    help='pass if findings <= N (ratcheting baseline)')
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    files = [Path(f) for f in args.files] if args.files else sorted((root / 'src').rglob('*.cpp'))
    headers = sorted((root / 'include').rglob('*.h')) + sorted((root / 'src').rglob('*.h'))

    findings: list[str] = []
    for f in files:
        if f.is_file():
            findings.extend(scan(f, headers))

    total = len(findings)
    if args.list or total > args.max_allowed:
        for f in findings:
            print(f'  {f}')

    if total > args.max_allowed:
        print(f'❌ Timer destructor cancel: {total} exceeds baseline ({args.max_allowed}).')
        print('   A raw lv_timer_t* cancelled only in cleanup()/stop_*() outlives the object')
        print('   on any teardown that skips that call — destroy_all() runs before lv_deinit().')
        print(f'   Cancel it from the destructor too, or annotate: // {OPT_OUT}: <reason>')
        return 1
    if total < args.max_allowed:
        print(f'✅ Timer destructor cancel: {total} (baseline {args.max_allowed} — ratchet down)')
        return 0
    print(f'✅ Timer destructor cancel: {total} == baseline ({args.max_allowed})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
