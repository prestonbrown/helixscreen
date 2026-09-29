#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: work markers in comments are counted against a ratcheting baseline.
#
# A marker is a comment that says a piece of the product is not done. Printed
# as informational output it is free to accumulate, and a handful of them were
# user-facing controls that did nothing (prestonbrown/helixscreen#1373). The
# count may fall - fix the marker, split it into an issue and cite it as
# `MARKER(#NNNN): <the constraint>`, or annotate why it stays - but never rise.
#
# Counted: a comment line (// or /* or a leading * in C-family sources, # in
# shell) carrying one of the marker words as a whole word. Code and string
# literals are not comments, so `class=XXX--text` and `mod_XXXXXX` style
# placeholders never match, and neither does an echo that names the words.
#
# Usage:
#   check_todo_markers.py                   # whole tree, fail on any marker
#   check_todo_markers.py --list            # every site
#   check_todo_markers.py --summary         # counts only
#   check_todo_markers.py --max-allowed 12  # ratcheting baseline
#   check_todo_markers.py path/to/file.cpp  # one file (tests use this)

import argparse
import os
import re
import sys

SCAN_DIRS = ('src', 'include', 'scripts')
C_EXTS = ('.cpp', '.h', '.hpp', '.mm', '.c')
SHELL_EXTS = ('.sh',)
SCAN_EXTS = C_EXTS + SHELL_EXTS

# The words are assembled at runtime so this file's own comments stay clean of
# them; the gate scans scripts/ too.
WORDS = ('TO' + 'DO', 'FIX' + 'ME')
WORD_RE = re.compile(r'\b(' + '|'.join(WORDS) + r')\b')
C_COMMENT_RE = re.compile(r'//|/\*|^\s*\*')
SHELL_COMMENT_RE = re.compile(r'#')


def comment_tail(line, path):
    """The part of `line` that is comment, or None when none of it is."""
    if path.endswith(SHELL_EXTS):
        # A shebang is not a comment about the code.
        if line.startswith('#!'):
            return None
        m = SHELL_COMMENT_RE.search(line)
    else:
        m = C_COMMENT_RE.search(line)
    if not m:
        return None
    return line[m.start():]


def scan_file(path):
    hits = []
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            for lineno, line in enumerate(f, 1):
                tail = comment_tail(line.rstrip('\n'), path)
                if tail is None:
                    continue
                if WORD_RE.search(tail):
                    hits.append((path, lineno, line.strip()))
    except OSError:
        pass
    return hits


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--max-allowed', type=int, default=None,
                    help='Pass if the marker count <= N (ratcheting baseline). '
                         'Without it any marker fails.')
    ap.add_argument('--summary', action='store_true', help='Count only')
    ap.add_argument('--list', action='store_true', help='Print every site')
    ap.add_argument('paths', nargs='*',
                    help='Files or directories to scan (default: src/ include/ scripts/)')
    args = ap.parse_args()

    def walk(dirs):
        found = []
        for d in dirs:
            for root, _, files in os.walk(d):
                found += [os.path.join(root, f) for f in files if f.endswith(SCAN_EXTS)]
        return found

    if args.paths:
        targets = [p for p in args.paths if p.endswith(SCAN_EXTS) and os.path.isfile(p)]
        targets += walk([p for p in args.paths if os.path.isdir(p)])
    else:
        targets = walk(SCAN_DIRS)

    hits = []
    for path in sorted(targets):
        hits += scan_file(path)
    total = len(hits)

    if args.list:
        for path, lineno, line in hits:
            print(f'{path}:{lineno}: {line}')
        print()

    if args.summary or args.list:
        print(f'  {"TOTAL":<11} {total:>5}')

    limit = args.max_allowed
    if limit is None:
        if total:
            print(f'❌ Markers: {total} in comments.')
            return 1
        print('✅ Markers: none')
        return 0

    if total > limit:
        print(f'❌ Markers: {total} exceeds baseline ({limit}). Fix it, or split it into '
              f'an issue and cite it as (#NNNN) with the constraint on the same line.')
        return 1
    if total < limit:
        print(f'✅ Markers: {total} (baseline {limit} — ratchet the baseline down)')
        return 0
    print(f'✅ Markers: {total} == baseline ({limit})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
