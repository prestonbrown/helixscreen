#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: a unit test must exercise shipped code, not a copy of it.
#
# The failure mode this exists for: a test defines its own copy of a production
# function -- usually with a comment saying "Mirrors Foo::bar()" -- and asserts
# against the copy. It reads as coverage, has real assertions, and cannot fail
# when Foo::bar() breaks. Worse, the copy drifts. Every one of these was found
# green in this tree while production had already moved:
#
#   test_ams_preheat.cpp        returned nozzle_min; production had switched to
#                               nozzle_recommended() precisely to stop two
#                               surfaces preheating a lane differently
#   test_print_controls_char    mirrored build_z_adjust_gcode() without MOVE=1
#                               and without its entire Z= branch, and also
#                               mirrored handle_tune_speed_changed(), which no
#                               longer exists anywhere in src/
#   test_temp_graph_overlay     mirrored update_y_axis_range(), long since
#                               renamed, carrying four stale constants
#   test_ui_panel_print_select  mirrored the file comparator WITHOUT the
#                               filename tiebreaker that fixes a real sort UB
#
# The pattern self-propagates: new mirror files cite older ones as precedent.
#
# TWO SIGNALS
#
#   1. shadow-include -- the file resolves no #include into include/ or src/,
#      so it cannot link a single line we ship. Include form does not matter;
#      "foo.h", "../../include/foo.h" and "system/foo.h" all resolve.
#
#   3. redefined-symbol -- a test file DEFINES a function at file scope whose
#      name is also a function we ship. This is the honest form of the mirror:
#      no comment admits to it, and the file does include production headers, so
#      neither signal above sees it. Both known instances were found this way:
#
#        test_update_checker.cpp:80   defines its own is_update_available() and
#                                     asserts against it ~30 times. Production's
#                                     is_update_available() is in an anonymous
#                                     namespace with zero call sites, so those
#                                     assertions validate a test-local copy of
#                                     dead code.
#        test_snapmaker_preprint_gcode.cpp:143,206
#                                     hardcodes fixed_heads(4, 0) instead of
#                                     asking the AmsBackendSnapmaker it builds
#                                     two lines later. Change
#                                     firmware_default_routing() and both stay
#                                     green.
#
#      File scope is the discriminator: a helper indented inside a fixture or a
#      TEST_CASE is local scaffolding, while a definition at column 0 that
#      shadows a shipped name is the shape that drifts.
#
#      Name collision alone is far too loose -- on this tree it yields 109
#      findings, mostly a generic test helper (push, contains, index_of, token,
#      name_of) sharing a name with an unrelated class METHOD. Two further
#      conditions carry the precision:
#
#        - the shipped symbol must be a FREE function defined at file scope
#          too. A method named push() is not something a test helper shadows.
#        - the test must ASSERT against the redefined name. That is what makes
#          it a mirror rather than a stub: a fake lv_xml_init() called only from
#          setup is scaffolding, while ~30 REQUIREs against a local
#          is_update_available() are a test of the copy.
#
#   2. mirror-comment -- a comment naming a production symbol as being mirrored,
#      copied, replicated or simulated, where that symbol really exists in
#      include/ or src/. A comment that names nothing real is not flagged (it is
#      usually describing the shape of test INPUT, which is fine).
#
# NOT FLAGGED, deliberately:
#
#   - Tests of shipped submodule patches (LVGL grid guards, the lodepng bit-depth
#     guard). They include only lvgl headers by design and DO test shipped code.
#   - Tests that drive ui_xml/ components through lv_xml_create() at runtime.
#   - Tests of the test fixtures themselves.
#   - A "mirrors" comment describing test input rather than reimplemented logic.
#   These all need the opt-out below, which documents WHY for the next reader.
#
# Per-file opt-out -- put it anywhere in the file:
#
#   // TEST_MIRROR_OK: exercises patches/lvgl_grid_update_guard.patch, which has
#   //                 no HelixScreen header to include
#
# Usage:
#   python3 scripts/check_test_mirrors.py --list
#   python3 scripts/check_test_mirrors.py --max-allowed N --summary

import argparse
import os
import re
import sys
from pathlib import Path

OPT_OUT = re.compile(r'//\s*TEST_MIRROR_OK\s*:')

# "Mirrors Foo::bar()", "replicated from baz.cpp", "same logic as X", ...
MIRROR_COMMENT = re.compile(
    r'//.*?\b(?:mirrors?|mirroring|replicates?|replicated\s+from|copy\s+of|'
    r'hand-copied|re-?implements?|simulates\s+(?:what|the)|same\s+(?:logic|algorithm)\s+as)\b'
    r'(?P<rest>.*)', re.IGNORECASE)

# A production symbol named inside such a comment: Foo::bar or bare bar()
SYMBOL_IN_COMMENT = re.compile(r'\b(?:(\w+)::)?(\w+)\s*\(\s*\)')

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.M)

# A function definition at column 0: a return type, then the name, then a body.
# Anchored to the line start with no leading whitespace, which is what keeps
# fixture methods, lambdas and in-TEST_CASE helpers out.
FILE_SCOPE_DEF = re.compile(
    r'^(?:static\s+|inline\s+|constexpr\s+|const\s+)*'
    r'(?:[A-Za-z_]\w*(?:::\w+)*(?:\s*<[^>;{]*>)?[\s*&]+)+'
    r'(?P<name>[A-Za-z_]\w*)\s*\([^;{)]*\)\s*(?:const\s+)?(?:noexcept\s*)?\{',
    re.M)

# Names that are test scaffolding by convention rather than shadows of shipped
# code. Kept deliberately short -- every entry here is a hole in the signal.
SCAFFOLD_NAMES = {'main', 'SetUp', 'TearDown'}


def production_headers(root: Path):
    """Basenames of every header we ship, plus every .cpp (for src/-relative includes)."""
    names = set()
    for sub in ('include', 'src'):
        d = root / sub
        if not d.is_dir():
            continue
        for p in d.rglob('*'):
            if p.suffix in ('.h', '.hpp', '.cpp'):
                names.add(p.name)
    return names


def production_free_functions(root: Path):
    """Names defined as free functions at file scope in shipped code.

    Deliberately NOT methods: a test helper named push() shadows nothing.
    """
    names = set()
    for sub in ('include', 'src'):
        d = root / sub
        if not d.is_dir():
            continue
        for p in d.rglob('*'):
            if p.suffix not in ('.h', '.hpp', '.cpp'):
                continue
            try:
                for m in FILE_SCOPE_DEF.finditer(p.read_text(errors='replace')):
                    names.add(m.group('name'))
            except OSError:
                pass
    return names


def production_symbols(root: Path):
    """Function names defined in src/ or declared in include/."""
    syms = set()
    pat = re.compile(r'\b(\w+)\s*\([^;{)]*\)\s*(?:const\s*)?(?:noexcept\s*)?[;{]')
    for sub in ('include', 'src'):
        d = root / sub
        if not d.is_dir():
            continue
        for p in d.rglob('*'):
            if p.suffix not in ('.h', '.hpp', '.cpp'):
                continue
            try:
                for m in pat.finditer(p.read_text(errors='replace')):
                    syms.add(m.group(1))
            except OSError:
                pass
    return syms


def test_local_headers(root: Path):
    names = set()
    d = root / 'tests'
    if d.is_dir():
        for p in d.rglob('*.h'):
            names.add(p.name)
    return names


def scan(root: Path):
    prod_hdrs = production_headers(root)
    prod_syms = production_symbols(root)
    prod_free = production_free_functions(root)
    local_hdrs = test_local_headers(root)
    findings = []

    unit = root / 'tests' / 'unit'
    if not unit.is_dir():
        return findings

    for path in sorted(unit.rglob('*.cpp')):
        try:
            text = path.read_text(errors='replace')
        except OSError:
            continue
        lines = text.split('\n')
        # Each signal's opt-out is scoped to that signal's own scope. shadow-include is a
        # claim about the whole file ("this file reaches production via a fixture header"),
        # so any TEST_MIRROR_OK in the file answers it. mirror-comment is a claim about one
        # comment, so only an annotation on or beside that comment answers it — otherwise a
        # single annotation anywhere silences every finding in the file and the ratchet
        # falls faster than the cleanup does.
        opt_out_lines = {i for i, l in enumerate(lines, 1) if OPT_OUT.search(l)}
        file_opt_out = bool(opt_out_lines)

        rel = path.relative_to(root)

        # Signal 1: does any include resolve into shipped code?
        reaches_production = False
        for inc in INCLUDE.findall(text):
            base = os.path.basename(inc)
            if base in prod_hdrs and base not in local_hdrs:
                reaches_production = True
                break
        if not reaches_production:
            if not file_opt_out:
                findings.append((str(rel), 0, 'shadow-include',
                                 'includes nothing from include/ or src/ — cannot execute shipped code'))
            continue  # the stronger signal; do not double-report

        # Signal 2: a comment claiming to mirror a real production symbol.
        for i, line in enumerate(lines, 1):
            m = MIRROR_COMMENT.search(line)
            if not m:
                continue
            # An annotation anywhere in the SAME contiguous comment block answers this
            # finding. A block is the unit a reader takes in at once, and the natural
            # place to write the justification is at the end of it, after the prose that
            # earns it — so a fixed +/-N line window around the flagged line misses it.
            lo = i
            while lo > 1 and lines[lo - 2].lstrip().startswith('//'):
                lo -= 1
            hi = i
            while hi < len(lines) and lines[hi].lstrip().startswith('//'):
                hi += 1
            if any(lo <= j <= hi for j in opt_out_lines):
                continue
            for cls, fn in SYMBOL_IN_COMMENT.findall(m.group('rest')):
                if fn in prod_syms:
                    named = f'{cls}::{fn}()' if cls else f'{fn}()'
                    findings.append((str(rel), i, 'mirror-comment',
                                     f'claims to mirror {named}, which exists in production'))
                    break

        # Signal 3: a file-scope definition shadowing a name we ship.
        asserted = set(re.findall(
            r'\b(?:REQUIRE|CHECK)[A-Z_]*\s*\([^;]*?\b(\w+)\s*\(', text))
        for m in FILE_SCOPE_DEF.finditer(text):
            name = m.group('name')
            if name in SCAFFOLD_NAMES or name not in prod_free:
                continue
            if name not in asserted:
                continue   # a stub called from setup, not a mirror under test
            i = text[:m.start()].count('\n') + 1
            lo = i
            while lo > 1 and lines[lo - 2].lstrip().startswith('//'):
                lo -= 1
            if any(lo <= j <= i + 1 for j in opt_out_lines):
                continue
            findings.append((str(rel), i, 'redefined-symbol',
                             f'defines {name}() at file scope, shadowing the '
                             f'{name}() we ship - assertions against this copy '
                             f'cannot fail when production changes'))
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', default='.', help='repo root')
    ap.add_argument('--list', action='store_true', help='print every finding')
    ap.add_argument('--summary', action='store_true', help='print counts only')
    ap.add_argument('--max-allowed', type=int, default=None,
                    help='fail if findings exceed this (ratchet)')
    args = ap.parse_args()

    root = Path(args.root).resolve()
    findings = scan(root)

    if args.list:
        for f, ln, kind, why in findings:
            loc = f'{f}:{ln}' if ln else f
            print(f'{loc}: [{kind}] {why}')
    elif not args.summary:
        for f, ln, kind, why in findings[:40]:
            loc = f'{f}:{ln}' if ln else f
            print(f'{loc}: [{kind}] {why}')
        if len(findings) > 40:
            print(f'... and {len(findings) - 40} more (use --list)')

    kinds = {}
    for _, _, k, _ in findings:
        kinds[k] = kinds.get(k, 0) + 1
    print(f'test-mirror findings: {len(findings)} '
          f'({", ".join(f"{k}={v}" for k, v in sorted(kinds.items())) or "none"})')

    if args.max_allowed is not None and len(findings) > args.max_allowed:
        print(f'FAIL: {len(findings)} exceeds --max-allowed {args.max_allowed}')
        print('A unit test must exercise shipped code, not a copy of it.')
        print('Legitimate exception? Annotate the file:')
        print('  // TEST_MIRROR_OK: <why this file cannot include a production header>')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
