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
#   killed       reverting the hunk turned a suite red. A test detects this
#                change. This is the outcome you want.
#   SURVIVED     reverting the hunk left the suite green. NO test detects this
#                change. The change shipped untested, whatever the diff's test
#                files claim.
#   uncompilable reverting the hunk does not build (it removed a declaration
#                something else needs). Reported separately and NEVER counted
#                as a kill: a compiler error proves the code is load-bearing
#                for the build, not that any test checks its behaviour. It
#                leaves the hunk UNPROVEN, so it makes the run incomplete.
#   unreversible the hunk does not apply in reverse against the working tree,
#                so no mutant exists to judge. Also unproven.
#   NOT COVERED  no strategy in this script can mutate the file at all. The
#                hunk is named, counted, and makes the run INCOMPLETE. This is
#                the verdict that must never be silent: a gate that answers
#                "clean" about a file it never opened launders an unproven
#                change as a proven one, and a clean run is what the commit
#                body cites as evidence.
#   SKIPPED      the hunk moves only comments or whitespace, so reverting it
#                cannot change behaviour and no test could ever kill it.
#                Dropped before it costs a build, and kept out of the tally
#                rather than padding it with unkillable survivors.
#                --no-skip-comments mutates them anyway.
#   EXCLUDED     scripts/untestable_paths.txt names the file, with a reason.
#                A reviewed, in-tree decision rather than an omission.
#
# SCOPE
#
# A file is worth mutating when reverting it can change something a runnable
# suite observes, which is a property of the path. COVERAGE below maps paths to
# a strategy; its DEFAULT is "not covered", so a path nobody has classified is
# reported rather than dropped. Cost follows the strategy, not the file count:
# only a compiled hunk pays for a build, and only a hunk whose strategy names a
# suite pays for that suite.
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
# EXIT CODES
#
#   0  every changed hunk was examined and every mutant was killed
#   1  at least one hunk SURVIVED
#   2  the baseline is broken (a red suite makes every mutant read as killed)
#   3  nothing survived, but the run did not examine the whole change
#      (--allow-incomplete downgrades this to 0 once a human has read why)
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
from fnmatch import fnmatch
from pathlib import Path

HUNK_RE = re.compile(r'^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,(\d+))? @@')
RAW_RE = re.compile(r'^:(\d+) (\d+) [0-9a-f]+ [0-9a-f]+ (\w)\d*\t(.*)$')

# Files the suite physically cannot execute (no headless GL, no device). Shared
# with scripts/cov_diff.py so both tools agree on what is un-judgeable rather
# than each reporting it as a different kind of debt.
UNTESTABLE_LIST = 'scripts/untestable_paths.txt'

# ---------------------------------------------------------------------------
# STRATEGIES
#
# `build` is whether the mutant needs `make test-build` before a suite can see
# it. Only code compiled into a binary does. ui_xml/ and assets/config/ are read
# off the source tree at run time on a path relative to the process working
# directory, which is the repo root for both `make test-run` and this script, so
# a reverted XML attribute or JSON value is live on the next suite run with no
# build at all. That is what makes widening the scope affordable.
#
# `suites` names which suites can see this kind of file. A hunk pays only for
# the suites its own strategy lists, and the baseline is established only for
# the suites some hunk will actually use.
# ---------------------------------------------------------------------------
STRATEGIES = {
    'cxx':     {'build': True,  'suites': ('catch2',)},
    'data':    {'build': False, 'suites': ('catch2',)},
    'tooling': {'build': False, 'suites': ('bats', 'pytest')},
}

# (glob, strategy). First match wins. `*` crosses `/`, so `src/*` is the whole
# subtree. Order matters where subtrees overlap.
COVERAGE = (
    ('src/*',                'cxx'),
    ('include/*',            'cxx'),
    ('ui_xml/*.xml',         'data'),
    ('assets/config/*.json', 'data'),
    ('scripts/*.py',         'tooling'),
    ('scripts/*.sh',         'tooling'),
    ('scripts/*.bash',       'tooling'),
)

# Content no program reads, so no mutant of it could change behaviour. Named in
# the report and does not make the run incomplete. The bar is "nothing executes
# this", not "testing it would be awkward" -- anything a program reads belongs
# in COVERAGE or in the not-covered default, where it stays visible.
NON_BEHAVIOURAL = (
    ('docs/*',           'documentation'),
    ('*.md',             'documentation'),
    ('LICENSE',          'project metadata'),
    ('COPYRIGHT',        'project metadata'),
    ('CONTRIBUTORS.txt', 'project metadata'),
    ('.gitignore',       'project metadata'),
    ('.gitattributes',   'project metadata'),
)

# Not covered, with a reason worth stating instead of the generic one. These
# still make a run incomplete: a stated reason is an explanation, not coverage.
UNMUTATABLE = (
    ('tests/*',        'a test is proven by mutating the code it pins, not by reverting itself'),
    ('lib/*',          'vendored or submodule code; a superproject diff carries no hunks for it'),
    ('patches/*',      'a patch against vendored code; the build applies the stack as a whole'),
    ('translations/*', 'locale content; the translation gates pin it and a revert cannot'),
)
DEFAULT_REASON = 'no mutation strategy covers this path'
GITLINK_REASON = 'a submodule pointer; run the gate inside the submodule'


def load_untestable(root):
    """[(path prefix, reason)] the tools must not judge."""
    out = []
    f = root / UNTESTABLE_LIST
    if not f.is_file():
        return out
    for line in f.read_text(errors='replace').splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        path, _, reason = line.partition('#')
        if path.strip():
            out.append((path.strip(), reason.strip()))
    return out


def classify(path, is_gitlink):
    """('mutate', strategy) | ('inert', reason) | ('uncovered', reason).

    The default is 'uncovered'. Every other outcome has to be claimed by a rule,
    which is what keeps a path nobody thought about out of a clean result.
    """
    if is_gitlink:
        return 'uncovered', GITLINK_REASON
    for pattern, strategy in COVERAGE:
        if fnmatch(path, pattern):
            return 'mutate', strategy
    for pattern, reason in NON_BEHAVIOURAL:
        if fnmatch(path, pattern):
            return 'inert', reason
    for pattern, reason in UNMUTATABLE:
        if fnmatch(path, pattern):
            return 'uncovered', reason
    return 'uncovered', DEFAULT_REASON


def run(cmd, cwd, capture=True, timeout=None):
    return subprocess.run(cmd, cwd=cwd, timeout=timeout,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.STDOUT if capture else None,
                          # Same reason as run_tests(): a compiler echoing a source
                          # line, or a tool quoting one, can carry a byte that is not
                          # UTF-8, and a strict decode loses the whole run to it.
                          text=True, errors='replace')


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


def changed_files(root, base):
    """[(path, is_gitlink)] for the whole diff.

    Read from `git diff --raw` rather than from the hunks, so a file the text
    diff has nothing to say about -- a binary asset, a submodule pointer, a mode
    change -- still reaches the report instead of vanishing between the two.
    """
    r = run(['git', '-c', 'core.quotePath=false', 'diff', '--raw', base], cwd=root)
    if r.returncode != 0:
        sys.exit(f'git diff --raw failed:\n{r.stdout}')
    out = []
    for line in r.stdout.splitlines():
        m = RAW_RE.match(line)
        if not m:
            continue
        old_mode, new_mode, _status, paths = m.groups()
        path = paths.split('\t')[-1]          # rename: destination is what exists now
        out.append((path, '160000' in (old_mode, new_mode)))
    return out


def collect_hunks(root, base):
    """Split the diff into one reversible single-hunk patch per hunk.

    Each patch carries the original file headers plus exactly one @@ block, so
    `git apply -R` can undo that hunk alone while its siblings stay applied.
    The diff is taken over the WHOLE tree; what may be mutated is decided by
    classify(), which reports what it declines rather than filtering it away.
    """
    r = run(['git', '-c', 'core.quotePath=false', 'diff', '-U3', base], cwd=root)
    if r.returncode != 0:
        sys.exit(f'git diff failed:\n{r.stdout}')
    hunks, path, current, hdr_lines = [], None, None, []

    def flush():
        if path and current:
            hunks.append({'file': path, 'line': current['line'],
                          'old_line': current['old_line'],
                          'body': list(current['body']),
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
            current = {'old_line': int(m.group(1)) if m else 0,
                       'line': int(m.group(2)) if m else 0, 'body': [line]}
        elif current is not None:
            current['body'].append(line)
    flush()
    return hunks


# ---------------------------------------------------------------------------
# Comment classification
#
# Every dialect resolves ambiguity toward CODE. Being wrong that way costs one
# wasted mutant; being wrong the other way silently drops a behavioural change,
# which is the whole failure this gate exists to prevent.
# ---------------------------------------------------------------------------
COMMENT_SYNTAX = {
    'c':    {'line': ('//',), 'block': ('/*', '*/'),    'raw': True,  'quotes': '"\''},
    'hash': {'line': ('#',),  'block': None,            'raw': False, 'quotes': '"\''},
    'xml':  {'line': (),      'block': ('<!--', '-->'), 'raw': False, 'quotes': ''},
    'none': {'line': (),      'block': None,            'raw': False, 'quotes': ''},
}

_EXT_SYNTAX = {
    'c': 'c', 'h': 'c', 'cc': 'c', 'cpp': 'c', 'cxx': 'c', 'hpp': 'c',
    'hh': 'c', 'inl': 'c', 'ipp': 'c', 'm': 'c', 'mm': 'c',
    'py': 'hash', 'sh': 'hash', 'bash': 'hash', 'bats': 'hash', 'mk': 'hash',
    'yml': 'hash', 'yaml': 'hash', 'cmake': 'hash',
    'xml': 'xml', 'html': 'xml', 'svg': 'xml',
}


def comment_syntax(path):
    """Which comment dialect a path is written in.

    An unknown extension gets 'none': nothing is treated as a comment, so a
    changed line can only be skipped when it differs by whitespace alone.
    """
    name = path.rsplit('/', 1)[-1]
    if name in ('Makefile', 'GNUmakefile'):
        return 'hash'
    ext = name.rsplit('.', 1)[-1].lower() if '.' in name else ''
    return _EXT_SYNTAX.get(ext, 'none')


def code_only_lines(text, syntax='c'):
    """Split source into lines with every comment blanked out.

    Two revisions of a file that differ only inside comments produce identical
    output here, which is what lets a comment-only hunk be recognised without
    guessing from the shape of a line (`*` starts a doxygen continuation AND a
    pointer store; `//` appears inside string literals).

    The scanner tracks block comments, string/char literals and C++11 raw
    strings.
    """
    rules = COMMENT_SYNTAX[syntax]
    line_tokens, block, quotes = rules['line'], rules['block'], rules['quotes']
    hash_style = '#' in line_tokens
    out, line = [], []
    state, raw_delim = 'code', ''
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '\n':
            out.append(''.join(line))
            line = []
            i += 1
            if state == 'line':
                state = 'code'
            continue
        if state == 'code':
            if rules['raw'] and c == 'R' and text.startswith('R"', i):
                j = text.find('(', i + 2)
                if j != -1 and '\n' not in text[i + 2:j]:
                    raw_delim = ')' + text[i + 2:j] + '"'
                    state = 'raw'
                    line.append(text[i:j + 1])
                    i = j + 1
                    continue
            tok = next((t for t in line_tokens if text.startswith(t, i)), None)
            # A hash is a comment only where a shell or python one can be: at
            # the start of a line or after whitespace. `${v#a}` and `$#` are
            # code, and blanking from there could make two different lines
            # compare equal and drop a real change. `#!` is a shebang, which
            # selects the interpreter and is therefore behaviour.
            if tok and hash_style and (text.startswith('#!', i)
                                       or (line and line[-1] not in ' \t')):
                tok = None
            if tok:
                state = 'line'
                i += len(tok)
                continue
            if block and text.startswith(block[0], i):
                state = 'block'
                i += len(block[0])
                continue
            line.append(c)
            if c in quotes:
                state = 'str' if c == '"' else 'chr'
            i += 1
            continue
        if state == 'line':
            i += 1
            continue
        if state == 'block':
            if text.startswith(block[1], i):
                state = 'code'
                i += len(block[1])
                continue
            i += 1
            continue
        if state == 'raw':
            if text.startswith(raw_delim, i):
                line.append(raw_delim)
                i += len(raw_delim)
                state = 'code'
                continue
            line.append(c)
            i += 1
            continue
        # 'str' / 'chr'
        if c == '\\' and i + 1 < n:
            line.append(text[i:i + 2])
            i += 2
            continue
        line.append(c)
        i += 1
        if (state == 'str' and c == '"') or (state == 'chr' and c == "'"):
            state = 'code'
    out.append(''.join(line))
    return out


def hunk_is_comment_only(root, base, h, cache):
    """True when reverting this hunk could not change behaviour.

    Every +/- line is compared with its comments stripped; if the code that
    remains is identical, the hunk moved only comments or whitespace. Building
    such a mutant costs a compile and a whole-program link to prove something no
    test could ever detect, so it is skipped rather than reported as a survivor.

    Any surprise -- unreadable pre-image, a hunk body that does not line up with
    the files -- returns False, and the hunk gets mutated as usual.
    """
    path = h['file']
    syntax = comment_syntax(path)

    def sides(key, loader):
        if key not in cache:
            try:
                cache[key] = code_only_lines(loader(), syntax)
            except Exception:
                cache[key] = None
        return cache[key]

    def pre_loader():
        r = run(['git', 'show', f'{base}:{path}'], cwd=root)
        return r.stdout if r.returncode == 0 else ''

    pre = sides(('pre', path), pre_loader)
    post = sides(('post', path), lambda: (root / path).read_text(errors='replace'))
    if pre is None or post is None:
        return False

    removed, added = [], []
    old_i, new_i = h['old_line'] - 1, h['line'] - 1
    try:
        for raw in h['body'][1:]:          # body[0] is the @@ header
            if raw.startswith('\\'):        # "\ No newline at end of file"
                continue
            tag = raw[:1]
            if tag == ' ':
                old_i += 1
                new_i += 1
            elif tag == '-':
                removed.append(pre[old_i])
                old_i += 1
            elif tag == '+':
                added.append(post[new_i])
                new_i += 1
            else:
                return False
    except IndexError:
        return False
    keep = lambda xs: [t for t in (x.strip() for x in xs) if t]
    return keep(removed) == keep(added)


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


def run_catch2(root, test_bin, filt, shards, log):
    """Return True if the C++ suite passed. Stops at the first failing case."""
    if shards <= 1:
        r = run([str(test_bin), filt, '-x', '1'], cwd=root)
        log.write(r.stdout or '')
        return r.returncode == 0
    procs = []
    for i in range(shards):
        procs.append(subprocess.Popen(
            [str(test_bin), filt, '-x', '1',
             '--shard-count', str(shards), '--shard-index', str(i)],
            # errors='replace': a mutant can make the code under test dump raw
            # bytes into a Catch2 failure message (a reverted raster guard wrote
            # 0xfe pixel data), and a strict decode turns that into a crash that
            # loses the verdict for the one hunk most likely to be killed.
            cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            errors='replace'))
    ok = True
    for p in procs:
        out, _ = p.communicate()
        log.write(out or '')
        if p.returncode != 0:
            ok = False
    return ok


class Suites:
    """The runnable suites, and which of them this machine actually has.

    A missing runner is reported, never treated as a pass: `bats not installed`
    reaching the report as a green tooling mutant would be the same laundering
    this gate exists to stop.
    """

    def __init__(self, root, args, log):
        self.root, self.args, self.log = root, args, log
        self.catch2_bin = root / 'build' / 'bin' / 'helix-tests'
        self.jobs = max(1, args.jobs)
        # The python gates are run from the repo venv, which is where their
        # dependencies are installed; a bare python3 misses them and reports the
        # import failure as a red baseline.
        venv = root / '.venv' / 'bin' / 'python3'
        self.python = str(venv) if venv.is_file() else 'python3'
        self._pytest_gap = None

    def missing(self, name):
        """Why this suite cannot run here, or '' when it can.

        Each check asks after the runner the way `run()` will invoke it. `bats`
        is executed as itself, so finding it on PATH is the whole question;
        pytest is executed as a module of an interpreter, so an interpreter
        existing answers nothing about it.
        """
        if name == 'catch2':
            # The baseline build produces the binary, and a build that cannot
            # run stops the whole run at exit 2 with the build log.
            return ''
        if name == 'bats':
            if not shutil.which('bats'):
                return 'bats is not installed'
            return '' if (self.root / self.args.shell_tests).exists() else f'no {self.args.shell_tests}'
        if name == 'pytest':
            gap = self._pytest_gap_reason()
            if gap:
                return gap
            return '' if (self.root / self.args.python_tests).exists() else f'no {self.args.python_tests}'
        return f'unknown suite {name}'

    def _pytest_gap_reason(self):
        """'' when `<python> -m pytest` can actually start, else why it cannot.

        An interpreter without pytest installed fails at import, which the run
        would otherwise read as a RED BASELINE and blame on the change. The
        answer cannot move during a run, so the probe is taken once.
        """
        if self._pytest_gap is None:
            if self.python == 'python3' and not shutil.which('python3'):
                self._pytest_gap = 'python3 is not installed'
            elif run([self.python, '-c', 'import pytest'], cwd=self.root).returncode != 0:
                self._pytest_gap = f'pytest is not importable by {self.python}'
            else:
                self._pytest_gap = ''
        return self._pytest_gap

    def run(self, name):
        if name == 'catch2':
            return run_catch2(self.root, self.catch2_bin, self.args.tests,
                              self.args.shards, self.log)
        if name == 'bats':
            cmd = ['bats']
            if shutil.which('parallel'):
                cmd += ['--jobs', str(self.jobs), '--no-parallelize-within-files']
            cmd.append(self.args.shell_tests)
            r = run(cmd, cwd=self.root)
            self.log.write(r.stdout or '')
            return r.returncode == 0
        if name == 'pytest':
            r = run([self.python, '-m', 'pytest', self.args.python_tests, '-q', '-x'],
                    cwd=self.root)
            self.log.write(r.stdout or '')
            return r.returncode == 0
        raise AssertionError(name)


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
    ap.add_argument('--shell-tests', default='tests/shell',
                    help='bats path run for a shell/tooling mutant; a single .bats file scopes it')
    ap.add_argument('--python-tests', default='tests/python',
                    help='pytest path run for a python/tooling mutant; a single file scopes it')
    ap.add_argument('--no-skip-comments', action='store_true',
                    help='mutate comment/whitespace-only hunks too (they can never be killed)')
    ap.add_argument('--allow-incomplete', action='store_true',
                    help='exit 0 when nothing survived but part of the change was not examined')
    ap.add_argument('--list-only', action='store_true', help='list hunks, mutate nothing')
    ap.add_argument('--log', default='/tmp/mutate-diff.log')
    args = ap.parse_args()

    root = repo_root()
    base = args.base or default_base(root)
    hunks = collect_hunks(root, base)
    untestable = load_untestable(root)

    # ---- classify the whole change -------------------------------------
    verdict_of = {}       # path -> ('mutate', strategy) | ('inert'|'uncovered', reason)
    for path, is_gitlink in changed_files(root, base):
        verdict_of[path] = classify(path, is_gitlink)
    for h in hunks:                       # a hunk git names but --raw did not
        verdict_of.setdefault(h['file'], classify(h['file'], False))

    hunks_of = {}
    for h in hunks:
        hunks_of.setdefault(h['file'], []).append(h)

    inert, uncovered = [], []
    for path, (kind, detail) in sorted(verdict_of.items()):
        n = len(hunks_of.get(path, ()))
        if kind == 'inert':
            inert.append((path, n, detail))
        elif kind == 'uncovered':
            uncovered.append((path, n, detail))

    mutable = [h for h in hunks if verdict_of[h['file']][0] == 'mutate']

    # scripts/untestable_paths.txt: an in-tree, reasoned exclusion, so it is
    # reported as a decision rather than as a gap in what the tool can see.
    skipped_untestable = []
    for h in list(mutable):
        hit = next(((p_, r) for p_, r in untestable if h['file'].startswith(p_)), None)
        if hit:
            skipped_untestable.append((f"{h['file']}:{h['line']}", hit[1]))
            mutable.remove(h)
    # A comment-only hunk costs a compile plus a 5 GB link to produce a mutant
    # no test could possibly detect, then lands in the tally as a survivor and
    # reads as real debt. Drop it before it costs anything.
    skipped_comment = []
    if not args.no_skip_comments:
        code_cache = {}
        for h in list(mutable):
            if hunk_is_comment_only(root, base, h, code_cache):
                skipped_comment.append(f"{h['file']}:{h['line']}")
                mutable.remove(h)

    # --only and --limit are deliberate narrowings, but a narrowed run still did
    # not examine the whole change, so what they set aside is counted too.
    deferred = 0
    if args.only:
        keep = [h for h in mutable if args.only in h['file']]
        deferred += len(mutable) - len(keep)
        mutable = keep
    if args.limit and len(mutable) > args.limit:
        deferred += len(mutable) - args.limit
        mutable = mutable[:args.limit]

    log = open(args.log, 'w')
    suites = Suites(root, args, log)

    # A strategy whose suite is not installed here cannot judge its hunks. Move
    # them to not-covered rather than letting an unrunnable suite read green.
    needed, unrunnable = set(), []
    for h in list(mutable):
        strategy = verdict_of[h['file']][1]
        gaps = [(s, suites.missing(s)) for s in STRATEGIES[strategy]['suites']]
        gaps = [(s, why) for s, why in gaps if why]
        if gaps:
            unrunnable.append((f"{h['file']}:{h['line']}", gaps[0][1]))
            mutable.remove(h)
            continue
        needed.update(STRATEGIES[strategy]['suites'])
    uncovered += [(label, 1, why) for label, why in unrunnable]

    for label, reason in skipped_untestable:
        print(f'  EXCLUDED    {label} - {reason}')
    for label in skipped_comment:
        print(f'  SKIPPED     {label} - comment/whitespace only')
    for path, n, reason in inert:
        print(f'  not behavioural  {path} - {reason}')
    for path, n, reason in uncovered:
        hunk_note = f' ({n} hunk{"s" if n != 1 else ""})' if n else ''
        print(f'  NOT COVERED {path}{hunk_note} - {reason}')
    if deferred:
        print(f'  DEFERRED    {deferred} hunk(s) set aside by --only/--limit')

    incomplete = bool(uncovered) or bool(deferred)

    if not mutable:
        print(f'\n0 hunk(s) to mutate, vs base {base[:12]}.')
    else:
        print(f'\n{len(mutable)} hunk(s) to mutate, vs base {base[:12]}')
        for h in mutable:
            print(f'  {h["file"]}:{h["line"]}  [{verdict_of[h["file"]][1]}]')

    if args.list_only:
        if incomplete:
            print(report_incomplete(uncovered, deferred))
        return 0
    if not mutable:
        if incomplete:
            print(report_incomplete(uncovered, deferred))
            return 0 if args.allow_incomplete else 3
        print('\nVERDICT: nothing in this change is mutatable, and nothing was skipped.')
        return 0

    needs_build = any(STRATEGIES[verdict_of[h['file']][1]]['build'] for h in mutable)

    # A red baseline makes every mutant look killed. Establish green first.
    print('\n=== baseline: build + suites must be GREEN before mutating ===')
    if needs_build or 'catch2' in needed:
        ok, secs = build(root, args.jobs, log)
        if not ok:
            print(f'FAIL: baseline build is broken. See {args.log}', file=sys.stderr)
            return 2
        print(f'  build ok ({secs:.0f}s)')
    for suite in sorted(needed):
        if not suites.run(suite):
            print(f'FAIL: baseline {suite} suite is RED. Fix it first, or every '
                  f'mutant will read as killed. See {args.log}', file=sys.stderr)
            return 2
        print(f'  {suite} green')
    print('  baseline established\n')

    results, judged_by = [], set()
    for n, h in enumerate(mutable, 1):
        strategy = verdict_of[h['file']][1]
        plan = STRATEGIES[strategy]
        target = root / h['file']
        # None means the change deletes the file: reverting recreates it, and
        # restoring means removing it again.
        original = target.read_bytes() if target.is_file() else None
        label = f'{h["file"]}:{h["line"]}'
        print(f'[{n}/{len(mutable)}] reverting {label} ... ', end='', flush=True)
        judged_by.update(plan['suites'])
        applied, why = apply_reverse(root, h['patch'])
        if not applied:
            print('unreversible (hunk would not reverse cleanly)')
            results.append((label, 'unreversible', why.strip().splitlines()[:1]))
            continue
        try:
            if plan['build']:
                built, secs = build(root, args.jobs, log)
                if not built:
                    print(f'uncompilable ({secs:.0f}s)')
                    results.append((label, 'uncompilable', ''))
                    continue
            passed = all(suites.run(s) for s in plan['suites'])
            if passed:
                print('SURVIVED  <-- no test detects this change')
                results.append((label, 'survived', ''))
            else:
                print('killed')
                results.append((label, 'killed', ''))
        finally:
            if original is None:
                target.unlink(missing_ok=True)
            else:
                target.write_bytes(original)
                os.utime(target, None)   # make compares mtimes; see SAFETY above

    # Leave the tree as found, with a rebuilt baseline binary so the next
    # `make test-run` is not testing the last mutant.
    if needs_build:
        print('\n=== restoring baseline binary ===')
        build(root, args.jobs, log)

    print('\n' + '=' * 68)
    for label, verdict, extra in results:
        mark = 'SURVIVED' if verdict == 'survived' else verdict
        print(f'  {mark:<13} {label}')
    tally = {}
    for _, v, _ in results:
        tally[v] = tally.get(v, 0) + 1
    print(f'\n{len(results)} hunk(s) mutated: '
          + ', '.join(f'{v}={n}' for v, n in sorted(tally.items())))
    print(f'log: {args.log}')

    survived = [r for r in results if r[1] == 'survived']
    unproven = [r for r in results if r[1] in ('uncompilable', 'unreversible')]
    if unproven or uncovered or deferred:
        incomplete = True

    if survived:
        # Name the suites that actually ran: a tooling hunk is judged by bats and
        # pytest, and quoting the Catch2 filter at it would misreport the verdict.
        where = ' + '.join(sorted(judged_by))
        if 'catch2' in judged_by:
            where = where.replace('catch2', f'catch2 {args.tests!r}')
        sys.stdout.flush()
        print(f'\nFAIL: {len(survived)} hunk(s) survived reversion - nothing in '
              f'{where} detects them:', file=sys.stderr)
        for label, _, _ in survived:
            print(f'  {label}', file=sys.stderr)
        return 1
    if incomplete:
        print(report_incomplete(uncovered, deferred, unproven))
        return 0 if args.allow_incomplete else 3
    print('\nVERDICT: CLEAN - every changed hunk was mutated and every mutant was killed.')
    return 0


def report_incomplete(uncovered, deferred, unproven=()):
    """The line that stops a partial run from reading like a whole one."""
    parts = []
    if uncovered:
        parts.append(f'{len(uncovered)} path(s) NOT COVERED')
    if unproven:
        parts.append(f'{len(unproven)} hunk(s) mutated but never judged by a test')
    if deferred:
        parts.append(f'{deferred} hunk(s) deferred by --only/--limit')
    return ('\nVERDICT: INCOMPLETE - this run did not examine the whole change: '
            + '; '.join(parts) + '.\n'
            'Nothing survived, but that is not evidence for the part above. '
            'Mutate it by hand and say so in the commit body, or accept it with '
            '--allow-incomplete.')


if __name__ == '__main__':
    sys.exit(main())
