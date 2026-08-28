#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Mutation gate, scoped to the diff: does any test actually detect this change?
#
# THE FAILURE MODE
#
# A test that pins an ADJACENT invariant instead of the changed line. It has
# real assertions, real expansions, and executes the changed function -- so it
# is invisible to check_vacuous_tests.py and green under line coverage. It just
# never asserts on the thing that changed. Reverting the production change
# leaves the suite green, which is the only way to see it.
#
# That experiment, run by hand, is what found 11 such changes in this tree in
# one pass. This script is that experiment as a make target.
#
# THE OPERATOR
#
# The primary mutation is REVERTING THE HUNK ITSELF, not a synthetic token edit
# (flip >= to >, negate a return). Two reasons:
#
#   - It asks the real question. A synthetic mutant asks "would a test notice
#     if this operator were different"; a hunk revert asks "would a test notice
#     if this change had not been made", which is what a reviewer needs to know.
#   - It is far cheaper. One mutant per hunk, against dozens per function for
#     token mutation, and each mutant costs a compile plus a link of a 5 GB
#     binary. Cost is the reason mutation testing does not get run; scoping it
#     to the hunks in the diff is what makes it affordable.
#
# VERDICTS, and why "uncompilable" is not a kill
#
#   killed       reverting the hunk turned the suite red. A test detects this
#                change. This is the outcome you want.
#   SURVIVED     reverting the hunk left the suite green. NO test detects this
#                change. The change shipped untested, whatever the diff's test
#                files claim.
#   uncompilable reverting the hunk does not build (it removed a declaration
#                something else needs). Reported separately and NEVER counted
#                as a kill: a compiler error proves the code is load-bearing
#                for the build, not that any test checks its behaviour.
#
# SAFETY
#
# The working tree is restored by writing back the bytes saved in memory before
# each mutation -- never by `git checkout`/`git restore`, which would also
# discard unrelated uncommitted work. Restored files are touch(1)ed, because
# make compares mtimes: a byte-identical restore with an older mtime leaves
# make nothing to do and the NEXT run silently tests the previous mutant's
# binary.
#
# Usage:
#   python3 scripts/mutate_diff.py --list-only          # what would be mutated
#   python3 scripts/mutate_diff.py                      # full run vs merge-base
#   python3 scripts/mutate_diff.py --tests "[ams]"      # scope the suite
#   make mutate-diff

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

MUTABLE_PREFIXES = ('src/', 'include/')
HUNK_RE = re.compile(r'^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@')


def run(cmd, cwd, capture=True, timeout=None):
    return subprocess.run(cmd, cwd=cwd, timeout=timeout,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.STDOUT if capture else None,
                          text=True)


def repo_root():
    r = run(['git', 'rev-parse', '--show-toplevel'], cwd='.')
    if r.returncode != 0:
        sys.exit('not a git repository')
    return Path(r.stdout.strip())


def default_base(root):
    """Merge base with main, so a feature branch mutates only its own work."""
    for ref in ('origin/main', 'main'):
        r = run(['git', 'merge-base', 'HEAD', ref], cwd=root)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    return 'HEAD'


def collect_hunks(root, base):
    """Split the diff into one reversible single-hunk patch per hunk.

    Each patch carries the original file headers plus exactly one @@ block, so
    `git apply -R` can undo that hunk alone while its siblings stay applied.
    """
    r = run(['git', 'diff', '-U3', base, '--', *MUTABLE_PREFIXES], cwd=root)
    if r.returncode != 0:
        sys.exit(f'git diff failed:\n{r.stdout}')
    hunks, path, current, hdr_lines = [], None, None, []

    def flush():
        if path and current:
            hunks.append({'file': path, 'line': current['line'],
                          'patch': ''.join(hdr_lines + current['body'])})

    for line in r.stdout.splitlines(keepends=True):
        if line.startswith('diff --git '):
            flush()
            current = None
            hdr_lines = [line]
            path = line.split(' b/')[-1].strip()
        elif current is None and (line.startswith(('index ', '--- ', '+++ ',
                                                   'old mode', 'new mode',
                                                   'similarity', 'rename ',
                                                   'new file', 'deleted file'))):
            hdr_lines.append(line)
        elif line.startswith('@@'):
            flush()
            m = HUNK_RE.match(line)
            current = {'line': int(m.group(1)) if m else 0, 'body': [line]}
        elif current is not None:
            current['body'].append(line)
    flush()
    return [h for h in hunks if h['file'].startswith(MUTABLE_PREFIXES)]


def apply_reverse(root, patch_text):
    p = subprocess.run(['git', 'apply', '-R', '--recount', '-'],
                       cwd=root, input=patch_text, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode == 0, p.stdout


def build(root, jobs, log):
    t = time.time()
    r = run(['make', f'-j{jobs}', 'test-build'], cwd=root)
    log.write(r.stdout or '')
    return r.returncode == 0, time.time() - t


def run_tests(root, test_bin, filt, shards, log):
    """Return True if the suite passed. Stops at the first failing test case."""
    if shards <= 1:
        r = run([str(test_bin), filt, '-x', '1'], cwd=root)
        log.write(r.stdout or '')
        return r.returncode == 0
    procs = []
    for i in range(shards):
        procs.append(subprocess.Popen(
            [str(test_bin), filt, '-x', '1',
             '--shard-count', str(shards), '--shard-index', str(i)],
            cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
    ok = True
    for p in procs:
        out, _ = p.communicate()
        log.write(out or '')
        if p.returncode != 0:
            ok = False
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', default=None, help='diff base (default: merge-base with main)')
    ap.add_argument('--tests', default='~[.]~[slow]', help='Catch2 filter for the scoped suite')
    ap.add_argument('--jobs', type=int, default=6, help='make -j (link is memory-gated; 6 is safe here)')
    ap.add_argument('--shards', type=int, default=8, help='parallel test shards per mutant')
    ap.add_argument('--limit', type=int, default=None, help='stop after N hunks')
    # A mutant costs a compile plus a whole-program link, so a 52-hunk range is
    # hours. Scoping to the files worth confirming is how this stays usable.
    ap.add_argument('--only', default=None,
                    help='restrict to changed files whose path contains this')
    ap.add_argument('--list-only', action='store_true', help='list hunks, mutate nothing')
    ap.add_argument('--log', default='/tmp/mutate-diff.log')
    args = ap.parse_args()

    root = repo_root()
    base = args.base or default_base(root)
    hunks = collect_hunks(root, base)
    if args.only:
        hunks = [h for h in hunks if args.only in h['file']]
    if args.limit:
        hunks = hunks[:args.limit]

    if not hunks:
        print(f'No changed hunks under {"/".join(MUTABLE_PREFIXES)} vs {base[:12]}.')
        return 0

    print(f'{len(hunks)} hunk(s) to mutate, vs base {base[:12]}')
    for h in hunks:
        print(f'  {h["file"]}:{h["line"]}')
    if args.list_only:
        return 0

    test_bin = root / 'build' / 'bin' / 'helix-tests'
    log = open(args.log, 'w')

    # A red baseline makes every mutant look killed. Establish green first.
    print('\n=== baseline: build + scoped suite must be GREEN before mutating ===')
    ok, secs = build(root, args.jobs, log)
    if not ok:
        print(f'FAIL: baseline build is broken. See {args.log}', file=sys.stderr)
        return 2
    print(f'  build ok ({secs:.0f}s)')
    if not run_tests(root, test_bin, args.tests, args.shards, log):
        print(f'FAIL: baseline suite is RED for filter {args.tests!r}. '
              f'Fix it first, or every mutant will read as killed. See {args.log}',
              file=sys.stderr)
        return 2
    print('  suite green - baseline established\n')

    results = []
    for n, h in enumerate(hunks, 1):
        target = root / h['file']
        original = target.read_bytes()
        label = f'{h["file"]}:{h["line"]}'
        print(f'[{n}/{len(hunks)}] reverting {label} ... ', end='', flush=True)
        applied, why = apply_reverse(root, h['patch'])
        if not applied:
            print('SKIP (hunk would not reverse cleanly)')
            results.append((label, 'unreversible', why.strip().splitlines()[:1]))
            continue
        try:
            built, secs = build(root, args.jobs, log)
            if not built:
                print(f'uncompilable ({secs:.0f}s)')
                results.append((label, 'uncompilable', ''))
                continue
            passed = run_tests(root, test_bin, args.tests, args.shards, log)
            if passed:
                print('SURVIVED  <-- no test detects this change')
                results.append((label, 'survived', ''))
            else:
                print('killed')
                results.append((label, 'killed', ''))
        finally:
            target.write_bytes(original)
            os.utime(target, None)   # make compares mtimes; see SAFETY above

    # Leave the tree as found, with a rebuilt baseline binary so the next
    # `make test-run` is not testing the last mutant.
    print('\n=== restoring baseline binary ===')
    build(root, args.jobs, log)

    print('\n' + '=' * 68)
    survived = [r for r in results if r[1] == 'survived']
    for label, verdict, extra in results:
        mark = 'SURVIVED' if verdict == 'survived' else verdict
        print(f'  {mark:<13} {label}')
    tally = {}
    for _, v, _ in results:
        tally[v] = tally.get(v, 0) + 1
    print(f'\n{len(results)} hunk(s): ' + ', '.join(f'{v}={n}' for v, n in sorted(tally.items())))
    print(f'log: {args.log}')

    if survived:
        print(f'\nFAIL: {len(survived)} hunk(s) survived reversion - no test in '
              f'{args.tests!r} detects them:', file=sys.stderr)
        for label, _, _ in survived:
            print(f'  {label}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
