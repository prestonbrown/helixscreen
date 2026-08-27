#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: a widget `name=` must be unique within its XML file.
#
# Background (#1136): ui_xml/ams_panel.xml declared name="endless_arrows" on two
# different elements. lv_obj_find_by_name() returns the FIRST match, so the
# second element was unreachable — never configured, never updated, no warning
# anywhere. The lookup silently succeeded on the wrong widget.
#
# Nothing in the stack catches this:
#
#   lib/lvgl/src/core/lv_obj_tree.c:631  lv_obj_find_by_name(parent, name)
#       lv_obj_t * child = find_by_name_direct(parent, name, UINT16_MAX);
#       if(child) return child;                      /* first sibling wins */
#       for(i = 0; i < child_cnt; i++) {             /* then depth-first    */
#           found = lv_obj_find_by_name(child, name);
#           if(found != NULL) return found;
#
# There is no scope boundary and no duplicate check — not at registration, not
# at lookup. The search walks straight through component-instance roots, so the
# only thing that bounds it is the `parent` the caller passes in. Panels almost
# always pass their own view root (`lv_obj_find_by_name(panel_, "...")`), which
# makes the whole file's view tree one flat namespace.
#
# WHAT COUNTS AS A COLLISION
#   Every element carrying `name=` inside a file's <view>, one file at a time.
#   Only the widget namespace: <style>/<bind_style*> `name=` refers to a style,
#   and <api>/<consts>/<subjects> live outside <view> entirely.
#
# NOT FLAGGED (each would otherwise be noise on every commit):
#   - Names in the two branches of an <if>/<else>: only one branch is ever
#     materialized, so the pair can never coexist.
#   - Interpolated names — `$i`, `${i}`, `$param` — resolve per expansion.
#   - Names ending in `#`: lv_obj_get_name_resolved() auto-indexes those per
#     sibling group (`slot_#` -> slot_0, slot_1), so they are distinct by design.
#   - Cross-FILE reuse. A row component instantiated twelve times contributes
#     twelve copies of its internal `label`, and every caller that looks one up
#     scopes the search to the row (ui_settings_machine_limits.cpp:222 does
#     exactly that). Flagging those would condemn the row-component pattern.
#
# ALSO FLAGGED
#   A literal `name=` inside a <repeat> body: written once, materialized `count`
#   times, so every expansion past the first is unreachable. Use `$i`/`${i}`.
#
# This is a ratchet with a per-name baseline, same shape as
# check_subscription_null_safety.py's CONST_SUBSCRIPT_BASELINE: a new key or a
# higher count fails; fixing a site makes its entry stale and the gate says so.
#
# Per-element opt-out — an XML comment on the line, or the line above:
#   <lv_obj name="value"/>  <!-- DUPLICATE_NAME_OK: nothing looks this up -->
#
# Usage:
#   ./scripts/check_duplicate_xml_names.py            # scan ui_xml/
#   ./scripts/check_duplicate_xml_names.py --staged-only
#   ./scripts/check_duplicate_xml_names.py --list     # every duplicated name
#   ./scripts/check_duplicate_xml_names.py --summary  # counts only
#   ./scripts/check_duplicate_xml_names.py ui_xml/ams_panel.xml

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable

SCAN_DIR = 'ui_xml'
OPT_OUT = 'DUPLICATE_NAME_OK'

# Known-duplicate names, keyed by "<path>::<name>" so unrelated edits above a
# site don't churn the table. The value is how many times that name appears in
# that file's <view>.
#
# Both files below are settings/about rows built inline: an icon + a label + a
# value per row, repeated down the page. Nothing looks those names up from the
# overlay root — the C++ that reads a row's `label`/`value` passes the ROW as
# the search parent, so the flat-namespace collision never fires. They are
# baselined rather than renamed because renaming them changes nothing that runs.
DUPLICATE_NAME_BASELINE: dict[str, int] = {
    'ui_xml/about_settings_overlay.xml::row_icon': 2,
    'ui_xml/about_settings_overlay.xml::label': 2,
    'ui_xml/about_settings_overlay.xml::value': 11,
    'ui_xml/printer_manager_overlay.xml::value': 3,
}

# An element open/close tag. The attribute run tolerates `>` inside a quoted
# value and the LVGL colon syntax (style_bg_color:checked="#primary"), which is
# why this is a tokenizer and not ElementTree — those colons are unbound
# namespace prefixes and make every strict XML parser bail.
TAG_RE = re.compile(r'<(/?)([A-Za-z_][\w.:-]*)((?:"[^"]*"|\'[^\']*\'|[^>"\'])*?)(/?)>', re.S)
ATTR_RE = re.compile(r'([\w.:-]+)\s*=\s*"([^"]*)"')
COMMENT_RE = re.compile(r'<!--.*?-->', re.S)

# `name=` on these refers to a style, not a widget: <style name="card"/> applies
# a style declared in <styles>, and every bind_style* variant names one too.
STYLE_TAGS = ('style', 'bind_style')


def _blank_comments(src: str) -> str:
    """Replace comments with same-length whitespace so offsets still map to
    the original line numbers."""
    return COMMENT_RE.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), src)


def _is_style_tag(tag: str) -> bool:
    return tag == 'style' or tag.startswith('bind_style')


def _is_dynamic(value: str) -> bool:
    """A name resolved at expansion time — `$i`, `${i}`, `$param` — or one
    auto-indexed per sibling group by lv_obj_get_name_resolved()."""
    return '$' in value or value.endswith('#')


def _has_index(value: str) -> bool:
    """Carries the <repeat> loop index, so each expansion gets its own name."""
    return '$i' in value or '${i}' in value


def scan_file(path: Path) -> list[tuple[str, str, str, list[int]]]:
    """Return [(key, kind, name, lines)] for every collision in this file.

    kind is 'duplicate' (the same name on N elements) or 'repeat' (one literal
    name inside a <repeat>, materialized count times)."""
    try:
        src = path.read_text(errors='replace')
    except OSError:
        return []

    clean = _blank_comments(src)
    src_lines = src.splitlines()

    def line_of(off: int) -> int:
        return src.count('\n', 0, off) + 1

    def opted_out(ln: int) -> bool:
        if 0 < ln <= len(src_lines) and OPT_OUT in src_lines[ln - 1]:
            return True
        # A comment on its own line belongs to the element below it. One that
        # trails an element belongs to THAT element, so it must not leak down
        # onto the next one.
        prev = src_lines[ln - 2] if ln >= 2 else ''
        return OPT_OUT in prev and prev.lstrip().startswith('<!--')

    stack: list[str] = []
    # One entry per enclosing <if>: [id, branch_index]. <else/> bumps the index
    # of the innermost one, so the two branches never share a path.
    branches: list[list[int]] = []
    if_depth_at: list[int] = []   # parallel to `branches`: len(stack) when pushed
    repeat_counts: list[str] = []

    hits: dict[str, list[tuple[int, tuple[tuple[int, int], ...], str]]] = {}
    repeat_hits: list[tuple[str, int]] = []

    for m in TAG_RE.finditer(clean):
        closing, tag, attrs, selfclose = m.groups()

        if closing:
            while stack and stack[-1] != tag:
                stack.pop()
            if stack:
                stack.pop()
            while branches and if_depth_at[-1] > len(stack):
                branches.pop()
                if_depth_at.pop()
            if tag == 'repeat' and repeat_counts:
                repeat_counts.pop()
            continue

        if tag == 'else':
            if branches:
                branches[-1][1] += 1
            continue

        a = dict(ATTR_RE.findall(attrs))
        name = a.get('name')

        # The <view> element's own `name=` lands on the root object too (the tag
        # is parsed as whatever it `extends`), so it shares the file's namespace.
        in_view = 'view' in stack or tag == 'view'
        if name and in_view and not _is_style_tag(tag):
            ln = line_of(m.start())
            if not opted_out(ln):
                in_repeat = bool(repeat_counts)
                if in_repeat and not _has_index(name):
                    # Materialized `count` times under one literal name. count="1"
                    # is degenerate but legal and produces no duplicate.
                    if repeat_counts[-1] != '1':
                        repeat_hits.append((name, ln))
                elif not _is_dynamic(name):
                    path_key = tuple((b[0], b[1]) for b in branches)
                    hits.setdefault(name, []).append((ln, path_key, tag))

        if not selfclose:
            stack.append(tag)
            if tag == 'if':
                branches.append([m.start(), 0])
                if_depth_at.append(len(stack))
            elif tag == 'repeat':
                repeat_counts.append(a.get('count', ''))

    rel = path.as_posix()
    findings: list[tuple[str, str, str, list[int]]] = []

    for name, occurrences in hits.items():
        # Two occurrences coexist only if every <if> they share resolves to the
        # same branch. Group by branch path and keep the largest coexisting set.
        groups: dict[tuple[tuple[int, int], ...], list[int]] = {}
        for ln, path_key, _tag in occurrences:
            groups.setdefault(path_key, []).append(ln)
        coexisting: list[int] = []
        for key_a, lines_a in groups.items():
            live = list(lines_a)
            for key_b, lines_b in groups.items():
                if key_b is key_a:
                    continue
                shared = dict(key_a)
                if all(shared.get(if_id, branch) == branch for if_id, branch in key_b):
                    live += lines_b
            if len(live) > len(coexisting):
                coexisting = live
        if len(coexisting) > 1:
            findings.append((f'{rel}::{name}', 'duplicate', name, sorted(coexisting)))

    for name, ln in repeat_hits:
        findings.append((f'{rel}::{name}', 'repeat', name, [ln]))

    return findings


def collect_files(args: argparse.Namespace) -> Iterable[Path]:
    if args.staged_only:
        out = subprocess.run(
            ['git', 'diff', '--cached', '--name-only', '--diff-filter=ACM'],
            capture_output=True, text=True, check=False,
        )
        for line in out.stdout.splitlines():
            p = Path(line)
            if p.suffix == '.xml' and p.exists():
                yield p
        return
    if args.files:
        for f in args.files:
            p = Path(f)
            if p.suffix == '.xml' and p.exists():
                yield p
        return
    yield from sorted(Path(SCAN_DIR).rglob('*.xml'))


def report(findings: list[tuple[str, str, str, list[int]]],
           summary: bool, full_scan: bool) -> int:
    observed: dict[str, int] = {}
    detail: dict[str, tuple[str, str, list[int]]] = {}
    for key, kind, name, lines in findings:
        observed[key] = observed.get(key, 0) + (len(lines) if kind == 'duplicate' else 1)
        detail[key] = (kind, name, lines)

    new_sites = [k for k, n in observed.items() if n > DUPLICATE_NAME_BASELINE.get(k, 0)]

    if new_sites:
        print(f'❌ Duplicate XML widget names: {len(new_sites)} name(s) '
              f'unreachable via lv_obj_find_by_name().')
        for key in sorted(new_sites):
            file = key.split('::', 1)[0]
            kind, name, lines = detail[key]
            if kind == 'repeat':
                print(f'   {file}:{lines[0]}: name="{name}" is a literal inside '
                      f'<repeat> — every expansion gets the same name')
            else:
                shown = ', '.join(str(n) for n in lines)
                print(f'   {file}: name="{name}" on {len(lines)} elements '
                      f'(lines {shown})')
        print()
        print('   lv_obj_find_by_name() returns the FIRST depth-first match and warns')
        print('   about nothing, so every later element with that name is unreachable —')
        print('   built, then silently never configured. That was #1136 (ams_panel.xml,')
        print('   name="endless_arrows" twice).')
        print('   Fix: rename one, or inside <repeat> use name="thing_${i}".')
        print('   Suppress per-element: <!-- DUPLICATE_NAME_OK: <reason> -->')
        return 1

    # Only meaningful on a full scan — an unscanned file trivially contributes
    # zero and would be misreported as fixed.
    stale = ([k for k, v in DUPLICATE_NAME_BASELINE.items() if observed.get(k, 0) < v]
             if full_scan else [])
    total = sum(observed.values())
    if stale:
        print(f'✅ Duplicate XML widget names: {total} '
              f'(baseline {sum(DUPLICATE_NAME_BASELINE.values())} — '
              f'{len(stale)} entr{"y" if len(stale) == 1 else "ies"} now fixed, '
              f'please drop from DUPLICATE_NAME_BASELINE)')
        if not summary:
            for k in sorted(stale):
                print(f'     stale baseline entry: {k}')
    else:
        print(f'✅ Duplicate XML widget names: {total} == baseline')
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*', help=f'XML files to check (default: scan {SCAN_DIR}/)')
    ap.add_argument('--staged-only', action='store_true', help='Check only staged XML files')
    ap.add_argument('--list', action='store_true',
                    help='Print every duplicated name (baselined or not) and exit 0')
    ap.add_argument('--summary', action='store_true',
                    help='Counts only — omit the per-site stale-baseline listing')
    args = ap.parse_args()

    repo_root = subprocess.run(
        ['git', 'rev-parse', '--show-toplevel'], capture_output=True, text=True, check=False,
    ).stdout.strip()
    if repo_root and not args.files:
        import os
        os.chdir(repo_root)

    findings: list[tuple[str, str, str, list[int]]] = []
    for path in collect_files(args):
        findings += scan_file(path)

    if args.list:
        for key, kind, name, lines in sorted(findings):
            file = key.split('::', 1)[0]
            baselined = ' [baselined]' if key in DUPLICATE_NAME_BASELINE else ''
            print(f'{file}: [{kind}] name="{name}" x{len(lines)} '
                  f'lines={lines}{baselined}')
        print(f'--- {len(findings)} duplicated name(s)')
        return 0

    full_scan = not args.files and not args.staged_only
    return report(findings, args.summary, full_scan)


if __name__ == '__main__':
    sys.exit(main())
