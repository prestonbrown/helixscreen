#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Which tests/shell files a change has to run.

Every shell test file is product unless tests/shell/tooling-triggers.tsv lists
it. Product files always run. A listed (tooling) file runs only when the change
touches one of its trigger paths or the file itself, because a test of the
build tooling cannot fail on a change the tooling never sees. The change is
everything that differs from the base: committed since it, staged, unstaged and
untracked.

A change to what every file shares (tests/shell/helpers.bash, setup_suite.bash,
fixtures/, the table or this script) runs everything, as does --all or
BATS_ALL=1. The nightly runs everything, so a trigger the table is missing is
caught within a day.

The table is checked on every run: a listed file that does not exist, a row
listed twice, or a trigger path that matches no tracked path is an error, since
a renamed script would otherwise stop triggering its test without a word.

Prints the selected files, one per line, and a one-line summary on stderr.

Usage: bats_select.py [--base REF] [--all] [--check] [--root DIR]
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import diff_base  # noqa: E402

SHELL_DIR = 'tests/shell'
TABLE = f'{SHELL_DIR}/tooling-triggers.tsv'
RUN_ALL = (
    f'{SHELL_DIR}/helpers.bash',
    f'{SHELL_DIR}/setup_suite.bash',
    f'{SHELL_DIR}/fixtures/',
    TABLE,
    'scripts/bats_select.py',
    'scripts/diff_base.py',
)


class TableError(Exception):
    pass


def git(root, *args):
    r = subprocess.run(['git', *args], cwd=root, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True, errors='replace')
    if r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


def glob_re(pattern):
    """A trigger path as a regex over repo-relative paths (see the table's header)."""
    if pattern.endswith('/'):
        return re.compile(re.escape(pattern) + '.*')
    out, i = '', 0
    while i < len(pattern):
        if pattern.startswith('**', i):
            out += '.*'
            i += 2
        elif pattern[i] == '*':
            out += '[^/]*'
            i += 1
        elif pattern[i] == '?':
            out += '[^/]'
            i += 1
        else:
            out += re.escape(pattern[i])
            i += 1
    return re.compile(out)


def read_table(root):
    """{bats file: [compiled trigger regexes]}, with the raw patterns for errors."""
    rows = {}
    for n, line in enumerate((root / TABLE).read_text().splitlines(), 1):
        if not line.strip() or line.startswith('#'):
            continue
        name, sep, rest = line.partition('\t')
        if not sep or not rest.split():
            raise TableError(f'{TABLE}:{n}: want "<bats file> TAB <path> ...": {line!r}')
        if name in rows:
            raise TableError(f'{TABLE}:{n}: {name} is listed twice')
        rows[name] = rest.split()
    return rows


def in_git_tree(root):
    """Is root a git checkout? An rsynced mirror is not, and has no diff to read."""
    try:
        return git(root, 'rev-parse', '--is-inside-work-tree').strip() == 'true'
    except RuntimeError:
        return False


def check_table(root, rows, tracked):
    """Every listed file exists and, given the tracked paths, every trigger matches one."""
    problems = []
    for name, patterns in rows.items():
        if not (root / name).is_file():
            problems.append(f'{name}: listed in {TABLE} but does not exist')
        if tracked is None:
            continue
        for p in patterns:
            rx = glob_re(p)
            if not any(rx.fullmatch(t) for t in tracked):
                problems.append(f'{name}: trigger {p} matches no tracked path')
    if problems:
        raise TableError('\n'.join(problems))


def changed_paths(root, base):
    """Paths that differ between base and the working tree, untracked included."""
    paths = set(git(root, 'diff', '--name-only', '--no-renames', base, '--').splitlines())
    paths |= set(git(root, 'ls-files', '--others', '--exclude-standard').splitlines())
    return paths


def select(all_files, rows, changed, run_all=False):
    """(files to run, why) for a change. Pure, for the tests."""
    triggers = {name: [glob_re(p) for p in pats] for name, pats in rows.items()}
    if run_all:
        return sorted(all_files), 'everything was asked for'
    shared = sorted(c for c in changed for r in RUN_ALL
                    if c == r or (r.endswith('/') and c.startswith(r)))
    if shared:
        return sorted(all_files), f'{shared[0]} changed, which every file shares'
    chosen = []
    for f in sorted(all_files):
        if f not in triggers:
            chosen.append(f)
        elif f in changed or any(rx.fullmatch(c) for rx in triggers[f] for c in changed):
            chosen.append(f)
    tooling = sum(1 for f in chosen if f in triggers)
    return chosen, (f'{len(chosen) - tooling} product file(s) and {tooling} of '
                    f'{len(triggers)} tooling file(s) whose paths changed')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--base', help='diff against this ref (default: the fork point)')
    ap.add_argument('--all', action='store_true', help='select every file')
    ap.add_argument('--check', action='store_true', help='validate the table and exit')
    ap.add_argument('--root', default='.')
    args = ap.parse_args(argv)
    root = Path(args.root).resolve()

    repo = in_git_tree(root)
    try:
        rows = read_table(root)
        check_table(root, rows, git(root, 'ls-files').splitlines() if repo else None)
    except (TableError, RuntimeError) as e:
        print(f'bats_select: {e}', file=sys.stderr)
        return 2
    if args.check:
        print(f'bats_select: {len(rows)} tooling file(s) listed, every path resolves',
              file=sys.stderr)
        return 0

    all_files = {f'{SHELL_DIR}/{p.name}' for p in (root / SHELL_DIR).glob('*.bats')}
    run_all = args.all or os.environ.get('BATS_ALL') == '1'
    changed, base_note = set(), 'every file asked for'
    if not repo:
        run_all, base_note = True, 'not a git checkout, so no change to read'
    if not run_all:
        base, why = diff_base.resolve_base(root, args.base)
        try:
            changed = changed_paths(root, base)
        except RuntimeError as e:
            print(f'bats_select: {e}; running everything', file=sys.stderr)
            run_all = True
        base_note = diff_base.base_line(root, base, why)
    chosen, why = select(all_files, rows, changed, run_all)
    print(f'bats_select: {base_note}; {len(changed)} changed path(s); '
          f'running {len(chosen)} of {len(all_files)} file(s): {why}', file=sys.stderr)
    print('\n'.join(chosen))
    return 0


if __name__ == '__main__':
    sys.exit(main())
