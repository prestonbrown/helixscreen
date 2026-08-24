#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: every <lv_obj> in a home-panel widget must state `scrollable`.
#
# In XML, <lv_obj> keeps LVGL's LV_OBJ_FLAG_SCROLLABLE default, which is ON.
# HelixScreen's theme overrides lv_obj's width/height/border/background/padding
# but NOT scrollable, so an author who reads "our theme makes lv_obj a pure
# layout container" reasonably concludes scrolling is off. It is not.
#
# That shipped a bug. ui_xml/components/panel_widget_print_status.xml had
# print_card_idle and print_card_idle_compact with no `scrollable` attribute, so
# they qualified for a page-scroll gutter and the chevrons rendered on top of the
# widget's thumbnail on an 800x480 ESP32 K-Touch (fixed in 7d69130df). Home
# widget tiles are also drag-scrolled by the grid beneath them, so an accidental
# scrollable container inside a tile steals the drag the grid wants.
#
# The rule is DECLARED INTENT, not a particular value. A genuinely scrolling
# list writes scrollable="true" and passes; a layout container writes
# scrollable="false" and passes. What fails is saying nothing, because saying
# nothing is how the default silently wins.
#
# Flagged, in ui_xml/components/panel_widget_*.xml only:
#   root   <view extends="lv_obj"> with no scrollable=  → same hazard, same fix
#   child  <lv_obj ...>            with no scrollable=
#
# NOT flagged:
#   - <view extends="ui_card"> and any other extends=. ui_card's create handler
#     already clears the flag (src/ui/ui_card.cpp:53), so those roots are safe.
#     Other extends= inherit whatever that component declared; the declaration
#     belongs on the component that actually extends lv_obj, not on every file
#     that composes it.
#   - Widgets other than lv_obj. lv_button, ui_card, text_body and friends are
#     not the hazard this gate is about.
#   - XML files outside ui_xml/components/panel_widget_*.xml. The bug is specific
#     to home-widget tiles living inside a drag-scrolled grid.
#
# There is deliberately no opt-out comment: the fix is one attribute, it is
# always available, and either value passes. An escape hatch would only ever be
# used to avoid deciding, which is exactly what the gate exists to prevent.
#
# This is a RATCHET, not a wall. 21 sites predate the gate. They are NOT fixed
# here on purpose: fixing one means guessing its author's intent, and some of
# them genuinely should scroll, so a blanket scrollable="false" would be a
# behavior change smuggled in under a lint commit. The baseline freezes today's
# count so no NEW undeclared container can be added - the number may fall as
# sites are decided one at a time, never rise.
#
# Usage:
#   check_panel_widget_scrollable.py                   # fail on any violation
#   check_panel_widget_scrollable.py --max-allowed 21  # ratcheting baseline
#   check_panel_widget_scrollable.py --summary         # counts only
#   check_panel_widget_scrollable.py --list            # every site, file:line
#   check_panel_widget_scrollable.py --rule child      # one rule only
#   check_panel_widget_scrollable.py --staged-only     # post-commit tree (pre-commit hook)
#
# --staged-only is NOT "only staged files". It scans the tree the commit WILL
# create (index content for staged paths, HEAD for the rest), built via
# `git write-tree`. The ratchet baseline is a whole-tree count, so the check has
# to see a whole tree; --staged-only makes that tree the would-be-committed one
# rather than the dirty working one, so another session's unstaged WIP cannot
# make a clean commit fail. The pre-commit hook (quality-checks.sh) passes this
# flag; CI and manual runs use the default whole-working-tree scan.

import argparse
import fnmatch
import os
import re
import subprocess
import sys

SCAN_DIR = os.path.join('ui_xml', 'components')
SCAN_GLOB = 'panel_widget_*.xml'

# git speaks posix paths, os.path.join does not on every host.
SCAN_PREFIX = SCAN_DIR.replace(os.sep, '/') + '/'

# The one extends= that is already safe: ui_card's create handler clears
# LV_OBJ_FLAG_SCROLLABLE before XML attributes are applied.
SAFE_ROOT_EXTENDS = {'ui_card'}

RULES = ('root', 'child')

FIX = {
    'root':  'scrollable="false" on <view extends="lv_obj"> (or "true" if it really scrolls)',
    'child': 'scrollable="false" on the <lv_obj> (or "true" if it really scrolls)',
}

# An open tag, attributes possibly spanning lines. Quoted values may contain '>',
# so the attribute run consumes whole quoted strings rather than stopping at the
# first '>'.
TAG_RE = re.compile(r'<(lv_obj|view)\b((?:[^>"\']|"[^"]*"|\'[^\']*\')*)/?>')

ATTR_RE = re.compile(r'\bscrollable\s*=')
EXTENDS_RE = re.compile(r'\bextends\s*=\s*"([^"]*)"')
COMMENT_RE = re.compile(r'<!--.*?-->', re.S)


def strip_comments(src):
    """Blank out comment bodies, keeping newlines so line numbers stay true."""
    return COMMENT_RE.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), src)


def is_panel_widget(path):
    return fnmatch.fnmatch(os.path.basename(path), SCAN_GLOB)


def scan_source(path, src):
    """Scan already-sourced text. `path` is only carried into the report."""
    src = strip_comments(src)
    lines = src.split('\n')
    hits = []

    for m in TAG_RE.finditer(src):
        tag, attrs = m.group(1), m.group(2)

        if tag == 'view':
            extends = EXTENDS_RE.search(attrs)
            # A <view> with no extends= is not an lv_obj; one extending ui_card
            # (or any other component) is somebody else's declaration to make.
            if not extends or extends.group(1) != 'lv_obj':
                continue
            rule = 'root'
        else:
            rule = 'child'

        if ATTR_RE.search(attrs):
            continue

        lineno = src.count('\n', 0, m.start()) + 1
        hits.append((path, lineno, rule, lines[lineno - 1].strip()[:100]))

    return hits


def repo_root():
    out = subprocess.run(['git', 'rev-parse', '--show-toplevel'],
                         capture_output=True, text=True, check=False).stdout.strip()
    return out or os.getcwd()


def _git_text(args, root):
    """Run git in root, return stdout (text). Never raises."""
    return subprocess.run(['git', '-C', root] + args,
                          capture_output=True, text=True, check=False).stdout


def _catfile_batch(root, revs, rels):
    """Yield (rel, text) for each blob rev via one `git cat-file --batch`.

    The byte-count header makes this robust to newlines/binary in content, and
    one streaming process beats spawning a `git show` per file.
    """
    proc = subprocess.Popen(['git', '-C', root, 'cat-file', '--batch'],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        for rev, rel in zip(revs, rels):
            proc.stdin.write((rev + '\n').encode())
            proc.stdin.flush()
            header = proc.stdout.readline().decode('utf-8', 'replace').split()
            # "<sha> blob <size>" - skip "missing" / non-blob (submodule) entries.
            if len(header) < 3 or header[1] != 'blob':
                continue
            size = int(header[2])
            content = proc.stdout.read(size)
            proc.stdout.read(1)  # trailing newline after each blob
            yield (rel, content.decode('utf-8', 'ignore'))
    finally:
        if proc.stdin is not None:
            proc.stdin.close()
        proc.wait()


def _in_scope(rel):
    """True if rel is a file the gate scans - mirrors the default walk's scope.

    Default mode walks ui_xml/components/ for panel_widget_*.xml; the
    post-commit mode (ls-tree) sees the WHOLE tree, so it must apply the same
    scoping or it would count every other XML file in the repo and the ratchet
    would stop meaning anything.
    """
    return rel.startswith(SCAN_PREFIX) and is_panel_widget(rel)


def collect(args, root):
    """Yield (path, source) for every file to scan, content already sourced.

    Three modes:
      - positional paths: read each from the working tree (fixtures, ad-hoc).
        Scope still holds - a hand-passed component that is not a panel widget
        is out of scope, not an exemption to argue about.
      - --staged-only: the POST-COMMIT TREE - `git write-tree` builds the tree
        the commit WILL create (index content for staged paths, HEAD for the
        rest), so unstaged WIP from another session never counts. Blobs stream
        through one `git cat-file --batch`. This is what the pre-commit hook
        needs: the ratchet baseline is a whole-tree count, so the check must
        still see a whole tree, just the would-be-committed one rather than the
        dirty working one.
      - default: the whole working tree (CI, manual runs).
    """
    if args.paths:
        for p in args.paths:
            if not is_panel_widget(p):
                continue
            try:
                yield (p, open(p, errors='ignore').read())
            except OSError:
                continue
        return

    if args.staged_only:
        # The tree this commit will produce, materialised as one tree object;
        # ls-tree lists its files, cat-file --batch streams their blobs.
        tree = _git_text(['write-tree'], root).strip()
        if not tree:
            return  # not a git repo / no index - nothing to check
        rels = [f for f in _git_text(['ls-tree', '-r', '--name-only', tree],
                                     root).split('\n') if f and _in_scope(f)]
        yield from _catfile_batch(root, [f'{tree}:{r}' for r in rels], rels)
        return

    # Default: the whole working tree, relative to wherever the gate was run.
    targets = []
    for walk_root, _, files in os.walk(SCAN_DIR):
        targets += [os.path.join(walk_root, f) for f in files if is_panel_widget(f)]
    for path in sorted(targets):
        try:
            yield (path, open(path, errors='ignore').read())
        except OSError:
            continue


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--max-allowed', type=int, default=None,
                    help='Pass if total violations <= N (ratcheting baseline). '
                         'Default: fail on any.')
    ap.add_argument('--summary', action='store_true', help='Per-rule counts only')
    ap.add_argument('--list', action='store_true', help='Print every site')
    ap.add_argument('--rule', choices=RULES, help='Restrict to one rule')
    ap.add_argument('--staged-only', action='store_true',
                    help='Scan the post-commit tree (index + HEAD), not the '
                         'dirty working tree - what the pre-commit hook uses so '
                         'unstaged WIP from another session cannot trip the ratchet')
    ap.add_argument('paths', nargs='*',
                    help=f'Files to scan (default: {SCAN_DIR}/{SCAN_GLOB})')
    args = ap.parse_args()

    hits = []
    for path, src in collect(args, repo_root()):
        hits += scan_source(path, src)
    if args.rule:
        hits = [h for h in hits if h[2] == args.rule]

    by_rule = {}
    for _, _, rule, _ in hits:
        by_rule[rule] = by_rule.get(rule, 0) + 1
    total = len(hits)

    if args.list:
        for path, lineno, rule, line in hits:
            print(f'{path}:{lineno}: [{rule}] {line}')
        print()

    if args.summary or args.list:
        for rule in RULES:
            n = by_rule.get(rule, 0)
            if not args.rule or args.rule == rule:
                print(f'  {rule:<7} {n:>5}   → {FIX[rule]}')
        print(f'  {"TOTAL":<7} {total:>5}')

    limit = args.max_allowed
    if limit is None:
        if total:
            print(f'❌ Panel-widget scrollable: {total} <lv_obj> with no scrollable attribute.')
            return 1
        print('✅ Panel-widget scrollable: every <lv_obj> states its intent.')
        return 0

    if total > limit:
        print(f'❌ Panel-widget scrollable: {total} undeclared exceeds baseline ({limit}).')
        print('   <lv_obj> defaults to SCROLLABLE; say scrollable="false" (or "true").')
        print('   Run: python3 scripts/check_panel_widget_scrollable.py --list')
        return 1
    if total < limit:
        print(f'✅ Panel-widget scrollable: {total} (baseline {limit} - ratchet the baseline down)')
        return 0
    print(f'✅ Panel-widget scrollable: {total} == baseline ({limit})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
