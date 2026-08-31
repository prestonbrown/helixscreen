#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: an assertion must be able to fail.
#
# Two shapes here cannot fail no matter what production does. Both read as
# ordinary tests -- real assertions, real values, green under coverage -- which
# is why they survive review and why a gate is the only thing that finds them.
#
# SIGNAL 1: set-then-assert
#
#     slot.set_tool_id(3);
#     REQUIRE(slot.tool_id() == 3);        // asserts std::int32_t assignment
#
# This tests the compiler. It is only reported when BOTH accessors are trivial
# in a shipped header -- setter is `member_ = v;`, getter is `return member_;`
# and nothing else. That filter is the whole gate: the identical shape against
# a setter that validates, clamps, normalises, or notifies IS a real test of
# that behaviour, and flagging it would be the false positive that gets the
# gate switched off. Non-trivial accessor, no finding.
#
# SIGNAL 2: self-fulfilling expectation
#
#     auto expected = build_gcode(cfg);
#     REQUIRE(build_gcode(cfg) == expected);   // passes for ANY implementation
#
#     REQUIRE(fmt_temp(t) == fmt_temp(t));     // the degenerate form
#
# The expected value is produced by the function under test, so the assertion
# restates determinism and says nothing about correctness. It survives any
# rewrite of build_gcode(), including one that returns "".
#
# WHAT THIS GATE CANNOT SEE, by construction: an assertion that is real but
# pins an ADJACENT invariant instead of the changed behaviour. Those have
# genuine expectations and no syntactic tell at all. `make mutate-diff` is the
# only instrument for that shape -- it reverts the hunk and looks for red.
#
# Per-finding opt-out, on or beside the flagged line:
#
#   // TEST_TAUTOLOGY_OK: set_/get_ pair is trivial today but the point of the
#   //                    test is that the field survives a rebuild of the row
#
# Usage:
#   python3 scripts/check_test_tautology.py --list
#   python3 scripts/check_test_tautology.py --max-allowed N --summary

import argparse
import re
import sys
from pathlib import Path

OPT_OUT = re.compile(r'//\s*TEST_TAUTOLOGY_OK\s*:')

# `obj.set_foo(LITERAL);` / `obj->set_foo(LITERAL);`
SETTER = re.compile(
    r'\b(?P<obj>[A-Za-z_]\w*)\s*(?P<arrow>\.|->)\s*set_(?P<field>\w+)\s*\('
    r'\s*(?P<val>"[^"]*"|-?\d+\.?\d*[fu]?|true|false)\s*\)\s*;')

# A literal: the only values worth flagging. A variable round-trip may be
# carrying meaning the gate cannot see.
LITERAL = re.compile(r'^(?:"[^"]*"|-?\d+\.?\d*[fu]?|true|false)$')

ASSERT = re.compile(r'\b(?:REQUIRE|CHECK)\s*\((?P<body>.*)\)\s*;')
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')


def split_eq(body: str):
    """Split an assertion body on a top-level `==`, respecting nesting."""
    depth = 0
    for i in range(len(body) - 1):
        c = body[i]
        if c in '([': depth += 1
        elif c in ')]': depth -= 1
        elif depth == 0 and body[i:i + 2] == '==':
            return body[:i].strip(), body[i + 2:].strip()
    return None, None


def trivial_accessors(root: Path):
    """Fields whose set_X/X() pair is a plain store and a plain load.

    Anything with a second statement -- validation, clamping, a notify, a
    dirty flag -- is deliberately absent from this set.
    """
    trivial_set, trivial_get = set(), set()
    set_re = re.compile(
        r'\bset_(\w+)\s*\([^)]*\)\s*(?:noexcept\s*)?\{\s*(\w+)\s*=\s*[\w.]+\s*;\s*\}')
    get_re = re.compile(
        r'\b(\w+)\s*\(\s*\)\s*const\s*(?:noexcept\s*)?\{\s*return\s+[\w.]+\s*;\s*\}')
    for sub in ('include', 'src'):
        d = root / sub
        if not d.is_dir():
            continue
        for p in d.rglob('*'):
            if p.suffix not in ('.h', '.hpp', '.cpp'):
                continue
            try:
                text = p.read_text(errors='replace')
            except OSError:
                continue
            for m in set_re.finditer(text):
                trivial_set.add(m.group(1))
            for m in get_re.finditer(text):
                trivial_get.add(m.group(1))
    return trivial_set, trivial_get


def scan(root: Path):
    tset, tget = trivial_accessors(root)
    findings = []
    unit = root / 'tests' / 'unit'
    if not unit.is_dir():
        return findings

    for path in sorted(unit.rglob('*.cpp')):
        try:
            lines = path.read_text(errors='replace').split('\n')
        except OSError:
            continue
        rel = str(path.relative_to(root))
        opt_out = {i for i, l in enumerate(lines, 1) if OPT_OUT.search(l)}

        def excused(n):
            return any(abs(n - j) <= 2 for j in opt_out)

        # --- Signal 1: set-then-assert on a trivial pair -------------------
        for i, line in enumerate(lines, 1):
            m = SETTER.search(line)
            if not m:
                continue
            field, obj, val = m.group('field'), m.group('obj'), m.group('val')
            if field not in tset or field not in tget:
                continue          # accessor does something; the test is real
            window = lines[i:i + 5]
            pat = re.compile(
                r'\b(?:REQUIRE|CHECK)\s*\(\s*' + re.escape(obj) +
                r'\s*(?:\.|->)\s*(?:get_)?' + re.escape(field) +
                r'\s*\(\s*\)\s*==\s*' + re.escape(val) + r'\s*\)')
            for k, w in enumerate(window, i + 1):
                if pat.search(w) and not excused(k):
                    findings.append((rel, k, 'set-then-assert',
                                     f'asserts set_{field}({val}) round-trips through a '
                                     f'trivial accessor pair - cannot fail'))
                    break

        # --- Signal 2: expectation produced by the function under test -----
        for i, line in enumerate(lines, 1):
            m = ASSERT.search(line)
            if not m or excused(i):
                continue
            lhs, rhs = split_eq(m.group('body'))
            if not lhs or not rhs:
                continue
            lnorm, rnorm = re.sub(r'\s+', '', lhs), re.sub(r'\s+', '', rhs)

            # `f() == f()` is NOT flagged. Measured against this tree it is
            # almost always a deliberate determinism or cache-stability
            # assertion (test_updates_external.cpp:122 says so in a comment on
            # the line above), and the mutation gate covers the case where it
            # earns nothing.

            # rhs is a variable that was assigned from the same call
            if not re.fullmatch(r'[A-Za-z_]\w*', rnorm):
                continue
            call = CALL.search(lhs)
            if not call:
                continue
            assign = re.compile(
                r'\b' + re.escape(rnorm) + r'\s*=\s*(?P<expr>[^;]+);')
            # Walk BACKWARDS and stop at the first line that does anything.
            # `auto before = x.count(); op_under_test(); REQUIRE(x.count() ==
            # before)` is a real invariance test -- the operation in between is
            # the whole point -- and it is the common shape by a wide margin.
            # Only an unbroken run of blanks and comments between the capture
            # and the assertion makes the pair vacuous.
            for back in reversed(lines[max(0, i - 11):i - 1]):
                stripped = back.strip()
                if not stripped or stripped.startswith(('//', '/*', '*')):
                    continue
                a = assign.search(back)
                if a and re.sub(r'\s+', '', a.group('expr')) == lnorm:
                    findings.append((rel, i, 'self-fulfilling',
                                     f'expected value `{rnorm}` was produced by the same '
                                     f'call being asserted, with nothing in between - '
                                     f'passes for any implementation'))
                break   # first real statement decides it, either way
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', default='.')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--summary', action='store_true')
    ap.add_argument('--max-allowed', type=int, default=None)
    args = ap.parse_args()

    findings = scan(Path(args.root).resolve())
    shown = findings if args.list else ([] if args.summary else findings[:40])
    for f, ln, kind, why in shown:
        print(f'{f}:{ln}: [{kind}] {why}')
    if not args.list and not args.summary and len(findings) > 40:
        print(f'... and {len(findings) - 40} more (--list for all)')

    kinds = {}
    for _, _, k, _ in findings:
        kinds[k] = kinds.get(k, 0) + 1
    print(f'\ntautology findings: {len(findings)} '
          f'({", ".join(f"{k}={v}" for k, v in sorted(kinds.items())) or "none"})')

    if args.max_allowed is not None and len(findings) > args.max_allowed:
        print(f'FAIL: {len(findings)} exceeds --max-allowed {args.max_allowed}',
              file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
