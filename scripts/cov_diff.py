#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Diff coverage: did the suite execute the lines this change actually touched?
#
# This is the cheap pre-filter for `make mutate-diff`. A changed line the tests
# never execute cannot possibly be tested, and finding that out costs one run
# instead of one build-and-run per hunk. The expensive mutation gate is then
# only worth spending on the lines that ARE executed.
#
# What it does NOT prove: that an executed line is TESTED. A test that runs the
# changed function and asserts on an adjacent invariant is green here and still
# worthless -- that is precisely the failure mode `mutate-diff` exists for, and
# the reason this script is a screen rather than a verdict. Read a clean report
# as "no line is trivially untested", never as "this change is covered".
#
# Usage:
#   make cov-build          # once: COVERAGE=1 test binary
#   make cov-run            # run the suite, producing .gcda
#   python3 scripts/cov_diff.py --base main
#   make cov-diff           # all three

import argparse
import gzip
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HUNK = re.compile(r'^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@')


def sh(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)


def repo_root():
    r = sh(['git', 'rev-parse', '--show-toplevel'])
    if r.returncode:
        sys.exit('not a git repository')
    return Path(r.stdout.strip())


def default_base(root):
    for ref in ('origin/main', 'main'):
        r = sh(['git', 'merge-base', 'HEAD', ref], cwd=root)
        if not r.returncode and r.stdout.strip():
            return r.stdout.strip()
    return 'HEAD'


def changed_lines(root, base):
    """{relative src path: {line numbers added or modified}} on the + side."""
    r = sh(['git', 'diff', '-U0', base, '--', 'src/'], cwd=root)
    if r.returncode:
        sys.exit(f'git diff failed:\n{r.stdout}')
    out, path = {}, None
    for line in r.stdout.splitlines():
        if line.startswith('diff --git '):
            path = line.split(' b/')[-1].strip()
        elif line.startswith('@@') and path:
            m = HUNK.match(line)
            if m:
                start = int(m.group(1))
                count = int(m.group(2) or 1)
                out.setdefault(path, set()).update(range(start, start + count))
    return {k: v for k, v in out.items() if v and k.endswith(('.cpp', '.c'))}


def gcov_lines(root, objdir, src_rel, tmp):
    """{line number: execution count} for one source file, or None if no data."""
    stem = Path(src_rel).relative_to('src').with_suffix('')
    gcda = objdir / f'{stem}.gcda'
    if not gcda.is_file():
        return None
    r = sh(['gcov', '-j', '-o', str(gcda.parent), str(root / src_rel)], cwd=tmp)
    if r.returncode:
        return None
    counts = {}
    for js in Path(tmp).glob('*.gcov.json.gz'):
        with gzip.open(js, 'rt') as fh:
            data = json.load(fh)
        want = (root / src_rel).resolve()
        for f in data.get('files', []):
            # gcov reports every TU the .gcda covers, so this must select ours
            # exactly. A basename or endswith test is wrong: "other_config.cpp"
            # ends with "config.cpp". -fprofile-abs-path is what makes the
            # absolute comparison reliable.
            reported = Path(f.get('file', ''))
            if not reported.is_absolute():
                reported = (root / reported)
            try:
                if reported.resolve() != want:
                    continue
            except OSError:
                continue
            for ln in f.get('lines', []):
                n = ln.get('line_number')
                if n is not None:
                    counts[n] = max(counts.get(n, 0), ln.get('count', 0))
        js.unlink()
    return counts or None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', default=None)
    ap.add_argument('--objdir', default='build/obj-cov')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()

    root = repo_root()
    base = args.base or default_base(root)
    objdir = root / args.objdir
    if not objdir.is_dir():
        sys.exit(f'{objdir} missing - run `make cov-build` first')

    changed = changed_lines(root, base)
    if not changed:
        print(f'No changed src/ lines vs {base[:12]}.')
        return 0

    uncovered, unbuilt, total_changed, total_exec = {}, [], 0, 0
    with tempfile.TemporaryDirectory() as tmp:
        for src_rel, lines in sorted(changed.items()):
            total_changed += len(lines)
            counts = gcov_lines(root, objdir, src_rel, tmp)
            if counts is None:
                unbuilt.append(src_rel)
                continue
            # A changed line with no gcov entry is not executable (a comment, a
            # brace, a declaration). Only lines gcov knows about are judged.
            dead = sorted(n for n in lines if counts.get(n, None) == 0)
            live = [n for n in lines if counts.get(n, 0) > 0]
            total_exec += len(live)
            if dead:
                uncovered[src_rel] = dead

    print(f'Diff coverage vs {base[:12]}: {len(changed)} file(s), '
          f'{total_changed} changed line(s)\n')
    for src_rel in unbuilt:
        print(f'  {src_rel}: NOT LINKED into the test binary - no test can '
              f'reach any of it')
    for src_rel, dead in uncovered.items():
        shown = dead if args.list else dead[:12]
        tail = '' if len(shown) == len(dead) else f' ... (+{len(dead)-len(shown)} more)'
        print(f'  {src_rel}: {len(dead)} changed line(s) never executed')
        print(f'      {", ".join(str(n) for n in shown)}{tail}')

    bad = sum(len(v) for v in uncovered.values())
    print(f'\n{total_exec} changed line(s) executed, {bad} never executed, '
          f'{len(unbuilt)} file(s) not linked into tests')
    if bad or unbuilt:
        print('\nA changed line the suite never executes cannot be tested. '
              'Note the converse does NOT hold: run `make mutate-diff` to find '
              'lines that ARE executed but still untested.', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
