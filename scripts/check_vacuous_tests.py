#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: a test case must actually assert something about shipped behaviour.
#
# This gate reads a Catch2 XML report produced with --success, so every finding
# is an observed fact about a run, not a guess about source text. That matters:
# a static pass over assertion SYNTAX cannot tell a real test from a vacuous one.
# Measured on this tree, 2334 of 12758 cases (18%) look "all-weak" by assertion
# shape, and the two worst offenders were both good tests --
# test_bt_discovery_utils.cpp is predicate coverage of both branches plus null,
# empty and case, and the AFC console corpus asserts inside a check_bare()
# helper. REQUIRE(is_valid_data_root(p)) and REQUIRE(ptr != nullptr) have the
# same shape and are not the same test. Precision at the syntax level is ~0, and
# a gate that fires on legitimate code gets switched off.
#
# TWO SIGNALS, both exact:
#
#   1. no-assertion -- the case ran to completion and Catch2 recorded zero
#      assertions. Not a heuristic: the test asserted nothing. Cases that
#      SKIP()ped are excluded (a skip is a decision, not a vacuum), as are
#      cases that failed (a failure is already loud).
#
#      Note this correctly PASSES table-driven tests that assert inside a
#      helper -- the assertions are recorded against the case that called it,
#      which is why the signal has to come from a run and not from the source.
#
#   2. literal-tautology -- Catch2 expands each side of an assertion to its
#      runtime value. When the expansion is character-identical to the source
#      text, nothing was read from the code under test: REQUIRE(5 == 5) expands
#      to "5 == 5", REQUIRE(true) to "true". A comparison against anything the
#      program computed expands differently (x == 5 becomes 5 == 5), so this
#      does not fire on ordinary assertions.
#
#      Macros that do not decompose their argument (REQUIRE_NOTHROW, the
#      _THROWS family, _THAT) always report Original == Expanded and are
#      therefore excluded -- they are a different kind of weak, and the
#      mutation gate is what judges them.
#
# What this gate deliberately does NOT try to catch: an assertion that is real
# but weak (existence-only checks, a constant the test also hardcodes). No
# report can see that. `make mutate-diff` is the oracle for it.
#
# Per-case opt-out lives in a baseline file rather than the source, because the
# finding is about a run and the test name is the only stable key:
#
#   scripts/vacuous_test_baseline.txt
#     BusThread starts and stops cleanly  # asserted by TSAN, not by Catch2
#
# Usage:
#   ./build/bin/helix-tests "~[.]" --reporter xml --success > report.xml
#   python3 scripts/check_vacuous_tests.py report.xml --list
#   python3 scripts/check_vacuous_tests.py report.xml --max-allowed 0

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# Tests that shell out (the plugin installer suite, for one) print ANSI-coloured
# status straight to stdout, which lands INSIDE the report when Catch2 is also
# writing XML to stdout. The result is a complete but unparseable report: raw
# ESC bytes are not legal XML 1.0 characters. Always generate reports with
# `--out FILE` so the two streams cannot interleave; this filter is the belt to
# that suspenders, so a report captured the naive way is still readable.
ILLEGAL_XML = re.compile(
    rb'[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]')


class _StripControlChars:
    """Byte-level file wrapper that drops characters XML cannot represent."""

    def __init__(self, path):
        self._f = open(path, 'rb')
        self.stripped = 0

    def read(self, n=-1):
        chunk = self._f.read(n)
        clean = ILLEGAL_XML.sub(b'', chunk)
        self.stripped += len(chunk) - len(clean)
        return clean

    def close(self):
        self._f.close()

# Assertion macros whose argument Catch2 cannot decompose. Their Expanded text
# is always a copy of Original, so the tautology signal is meaningless for them.
NON_DECOMPOSING = {
    'REQUIRE_NOTHROW', 'CHECK_NOTHROW',
    'REQUIRE_THROWS', 'CHECK_THROWS',
    'REQUIRE_THROWS_AS', 'CHECK_THROWS_AS',
    'REQUIRE_THROWS_WITH', 'CHECK_THROWS_WITH',
    'REQUIRE_THROWS_MATCHES', 'CHECK_THROWS_MATCHES',
    'REQUIRE_THAT', 'CHECK_THAT',
}


def load_baseline(path: Path):
    """Test-case names exempted from the no-assertion signal, with the reason."""
    allowed = {}
    if not path or not path.is_file():
        return allowed
    for line in path.read_text(errors='replace').splitlines():
        line = line.strip()
        # A comment is '#' followed by space or nothing. A bare '#' hugging its
        # text starts a test name - `#1127 seeding state costs no extra bytes`
        # is a real case, and treating it as a comment silently drops the entry.
        if not line or line == '#' or line.startswith('# '):
            continue
        # The documented format separates name from reason with two spaces
        # before the '#', so a name may contain one.
        name, sep, reason = line.partition('  #')
        allowed[name.strip()] = reason.strip() if sep else ''
    return allowed


def text_of(elem, tag):
    child = elem.find(tag)
    return (child.text or '').strip() if child is not None else ''


def scan(report: Path, baseline: dict):
    """Walk the report once, yielding (kind, case, file, line, detail)."""
    findings = []
    case = None          # attributes of the TestCase we are inside
    n_assertions = 0
    n_skips = 0
    failed = False

    src = _StripControlChars(report)
    try:
        for event, elem in ET.iterparse(src, events=('start', 'end')):
            if event == 'start':
                if elem.tag == 'TestCase':
                    case, n_assertions, n_skips, failed = elem.attrib, 0, 0, False
                continue

            if elem.tag == 'Expression' and case is not None:
                n_assertions += 1
                if elem.get('success') == 'false':
                    failed = True
                mtype = elem.get('type', '')
                if mtype not in NON_DECOMPOSING:
                    original = text_of(elem, 'Original')
                    expanded = text_of(elem, 'Expanded')
                    if original and original == expanded:
                        findings.append((
                            'literal-tautology',
                            case.get('name', '?'),
                            elem.get('filename', case.get('filename', '?')),
                            elem.get('line', '0'),
                            f'{mtype}({original}) compares literals - '
                            f'the expansion is identical to the source, so nothing '
                            f'the program computed was read'))

            elif elem.tag == 'Skip' and case is not None:
                n_skips += 1

            elif elem.tag == 'Failure' and case is not None:
                failed = True

            elif elem.tag == 'OverallResult' and case is not None:
                n_skips += int(elem.get('skips', '0') or 0)

            elif elem.tag == 'TestCase':
                name = case.get('name', '?') if case else '?'
                if (n_assertions == 0 and n_skips == 0 and not failed
                        and name not in baseline):
                    findings.append((
                        'no-assertion', name,
                        case.get('filename', '?'), case.get('line', '0'),
                        'ran to completion and asserted nothing'))
                case = None
                elem.clear()
    except ET.ParseError as e:
        sys.stderr.write(
            f'{report}: malformed Catch2 XML ({e}).\n'
            f'If the report ends without </Catch2TestRun> the run crashed - check '
            f'the binary exit code before trusting this gate. If it is complete, '
            f'regenerate it with `--out FILE` so test stdout cannot interleave '
            f'with the report.\n')
        return None
    finally:
        if src.stripped:
            sys.stderr.write(
                f'note: dropped {src.stripped} control byte(s) written into the '
                f'report by test stdout; use `--out FILE` to avoid this.\n')
        src.close()
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('report', help='Catch2 XML report (produced with --success)')
    ap.add_argument('--baseline', default='scripts/vacuous_test_baseline.txt')
    ap.add_argument('--list', action='store_true', help='print every finding')
    ap.add_argument('--summary', action='store_true', help='print counts only')
    ap.add_argument('--max-allowed', type=int, default=None,
                    help='exit non-zero if findings exceed this (ratchet)')
    args = ap.parse_args()

    report = Path(args.report)
    if not report.is_file():
        sys.stderr.write(f'{report}: no such report\n')
        return 2

    findings = scan(report, load_baseline(Path(args.baseline)))
    if findings is None:
        return 2

    by_kind = {}
    for kind, *_ in findings:
        by_kind[kind] = by_kind.get(kind, 0) + 1

    if args.list:
        shown = findings
    elif args.summary:
        shown = []
    else:
        shown = findings[:40]

    for kind, case, f, ln, why in shown:
        print(f'{f}:{ln}: [{kind}] {case}\n    {why}')
    if not args.list and not args.summary and len(findings) > 40:
        print(f'... and {len(findings) - 40} more (--list for all)')

    counts = ', '.join(f'{k}={v}' for k, v in sorted(by_kind.items())) or 'none'
    print(f'\nvacuous-test findings: {len(findings)} ({counts})')

    if args.max_allowed is not None and len(findings) > args.max_allowed:
        print(f'FAIL: {len(findings)} findings exceeds --max-allowed '
              f'{args.max_allowed}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
