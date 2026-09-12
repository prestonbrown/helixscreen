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
#   killed       reverting the hunk made a suite REPORT A FAILING TEST. A test
#                detects this change. This is the outcome you want. A runner
#                exiting non-zero is not enough on its own; see INCONCLUSIVE.
#   SURVIVED     reverting the hunk left the suite green. NO test detects this
#                change. The change shipped untested, whatever the diff's test
#                files claim.
#   INCONCLUSIVE a suite ran and nothing it produced settles the question: it
#                exited non-zero without naming a failing test, or the build
#                reported success and left the test binary untouched, so the
#                suite that reddened was running a binary this mutant is not
#                in. Never a kill and never a survivor. It leaves the hunk
#                UNPROVEN, so it makes the run incomplete.
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
# suite pays for that suite. The one build a build-free strategy can still pay
# for is a C++ suite binary that is not on disk, since the suite judging a
# runtime data file is itself a compiled program.
#
# THE BASE
#
# The diff base decides what the run is ABOUT, and getting it wrong is silent:
# a branch cut from a maintenance branch, measured against main, hands the run
# that whole divergence as the change under test -- dozens of foreign hunks, a
# build apiece, every verdict about somebody else's code. Most come back
# `uncompilable`, which is correctly not a kill, so the only symptom is a long
# run of verdicts that reads as a property of the change rather than of the base.
#
# scripts/diff_base.py picks it, and scripts/cov_diff.py asks the same module the
# same question. What is mutate-diff's own is the price of being wrong: the base
# is printed first with the hunk count, and an implausible count on an
# automatically chosen base stops the run instead of spending an hour proving it.
# `--base` overrides all of it and is never second-guessed.
#
# SAFETY
#
# The working tree is restored by writing back bytes saved in memory -- never by
# `git checkout`/`git restore`, which would also discard unrelated uncommitted
# work. Those bytes are read ONCE, before the first mutation. Reading them per
# hunk restores to whatever is on disk at that moment, so a restore that did not
# land becomes the next hunk's baseline and rides into that hunk's verdict;
# against a single capture, every file the run may mutate is compared after each
# restore, and a mismatch stops the run instead of judging a tree the run did
# not choose.
#
# Restored files are touch(1)ed, because make compares mtimes: a byte-identical
# restore with an older mtime leaves make nothing to do and the NEXT run
# silently tests the previous mutant's binary. That the binary actually changed
# is checked as well, either side of each mutant's build, because a build with
# nothing to do leaves the previous mutant's binary under this mutant's suite.
#
# That restore is a `finally:` in the per-hunk loop, so it needs the interpreter
# to keep running long enough to execute it. Interrupt this script with SIGINT
# (Ctrl-C, or `kill -INT`) and the hunk in flight is put back. SIGTERM (a bare
# `kill`, or a harness reaping the process) kills Python without unwinding, and
# leaves that hunk REVERTED in the working tree -- a silent, plausible-looking
# edit that gets committed. SIGKILL likewise, and cannot be helped.
#
# EXIT CODES
#
#   0  every changed hunk was examined and every mutant was killed
#   1  at least one hunk SURVIVED
#   2  the harness could not produce a verdict: the baseline build failed, a
#      baseline suite is red (which would make every mutant read as killed), a
#      baseline suite could not report at all, the C++ suite has no binary and a
#      build did not leave one, or a file the run may mutate stopped matching
#      the tree the run started from
#   3  nothing survived, but the run did not examine the whole change -- an
#      uncovered path, a hunk deferred by --only/--limit, or a hunk whose suite
#      exited without a verdict
#      (--allow-incomplete downgrades this to 0 once a human has read why)
#   4  refused to start: the automatically chosen base yields more hunks than
#      --max-hunks, and nothing confirmed it. Not a verdict; nothing ran.
#
# Usage:
#   python3 scripts/mutate_diff.py --list-only          # what would be mutated
#   python3 scripts/mutate_diff.py                      # full run vs merge-base
#   python3 scripts/mutate_diff.py --tests "[ams]"      # scope the suite
#   python3 scripts/mutate_diff.py --base origin/release/1.0
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

# The diff base is the same question scripts/cov_diff.py asks, and two
# hand-written copies of one rule agree by convention until they silently do
# not. scripts/diff_base.py is the single answer, and a fixture that copies this
# script has to copy that module beside it.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from diff_base import base_line, resolve_base  # noqa: E402

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


def git_toplevel():
    """The worktree root, or None when the cwd is not inside a git repository."""
    r = run(['git', 'rev-parse', '--show-toplevel'], cwd='.')
    if r.returncode != 0 or not r.stdout.strip():
        return None
    return Path(r.stdout.strip())


def repo_root():
    root = git_toplevel()
    if root is None:
        sys.exit('not a git repository')
    return root


def default_log_path():
    """Run log named for the worktree it is run from.

    Every verdict, every suite's stdout and the diagnostics that quote this path
    go into one file opened with 'w'. A path shared between worktrees is
    truncated and then interleaved by whichever run starts next, and the
    per-hunk attribution the gate exists to produce is exactly what is lost --
    an inline SURVIVED beside a final-summary killed for the same hunk. Two runs
    in the SAME tree still share this path; they already cannot coexist, because
    they fight over the tree and the test binary.
    """
    root = git_toplevel()
    if root is None:
        return '/tmp/mutate-diff.log'
    return f'/tmp/mutate-diff-{root.name}.log'


def rotate_log(path):
    """Keep the last run's log beside the new one, as <path>.prev.

    A verdict is disputed after the run that produced it has ended, and this
    file is the only record of the output behind it. Opened 'w' with nothing
    kept, the next run is the one thing that has to happen for that record to be
    gone.
    """
    previous = Path(path)
    if previous.is_file():
        try:
            previous.replace(str(previous) + '.prev')
        except OSError:
            pass          # an unwritable directory costs the archive, not the run


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


# ---------------------------------------------------------------------------
# WHAT A SUITE RUN ESTABLISHES
#
# A process exit code cannot say whether a test failed. A shard whose filter
# matched no case, a runner that could not start, and a process that aborts
# during static destruction after reporting every assertion green all exit
# non-zero, and none of them judged the change. `killed` is the verdict nobody
# re-checks, because it is the answer the operator was hoping for and it gets
# quoted in a commit body as proof, so it is the one that has to be earned: only
# a suite's own report of a failing test is a detection.
#
#   DETECTED      the suite named a failing test in its output
#   GREEN         the suite ran to completion and named none
#   INCONCLUSIVE  the suite exited non-zero and named none; it judged nothing
# ---------------------------------------------------------------------------
DETECTED, GREEN, INCONCLUSIVE = 'detected', 'green', 'inconclusive'

# How each runner reports a failing test. A pattern with a capture group is
# evidence only when the count it captures is non-zero, which keeps a summary
# line that counts no failures -- Catch2's "failed as expected" for a
# [!shouldfail] case, bats' "0 failures" -- from reading as one.
SUITE_FAILURE_EVIDENCE = {
    'catch2': (re.compile(r'^.+:\d+: FAILED:', re.M),
               re.compile(r'^(?:test cases|assertions):.*?(\d+) failed(?! as expected)',
                          re.M)),
    'bats':   (re.compile(r'^not ok \d+', re.M),
               re.compile(r'^\s*\d+ tests?, (\d+) failures?', re.M)),
    'pytest': (re.compile(r'^FAILED ', re.M),
               re.compile(r'^=*\s*(\d+) failed', re.M)),
}


def suite_detected(name, output):
    """True when a suite's own output reports a failing test."""
    for rx in SUITE_FAILURE_EVIDENCE[name]:
        for m in rx.finditer(output):
            if rx.groups == 0 or int(m.group(1)) > 0:
                return True
    return False


def suite_outcome(name, returncode, output, who=''):
    """(state, reason) for one runner process, read from what it printed."""
    if suite_detected(name, output):
        return DETECTED, ''
    if returncode != 0:
        return INCONCLUSIVE, (f'{who or name} exited {returncode} without '
                              f'naming a failing test')
    return GREEN, ''


class BuildUnavailable(RuntimeError):
    """The Catch2 binary is not there, so no verdict about a mutant is possible.

    Raised rather than reported as a red suite: a suite that could not start is
    not evidence about the change, and letting it read as a kill is the
    laundering this gate exists to stop.
    """


def build(root, jobs, log):
    t = time.time()
    r = run(['make', f'-j{jobs}', 'test-build'], cwd=root)
    log.write(r.stdout or '')
    return r.returncode == 0, time.time() - t


def run_catch2(root, test_bin, filt, shards, log):
    """(state, reason) for the C++ suite. Stops at the first failing case.

    Every shard is read for a failing assertion of its own. One that names one
    has detected the mutant whatever the others did; one that exits non-zero and
    names none has judged nothing, and the run says so rather than inferring a
    detection from its exit code.
    """
    argv = [str(test_bin), filt, '-x', '1']
    planned = [('the catch2 suite', argv)] if shards <= 1 else [
        (f'catch2 shard {i}',
         argv + ['--shard-count', str(shards), '--shard-index', str(i)])
        for i in range(shards)]
    procs = [(who, subprocess.Popen(
        cmd,
        # errors='replace': a mutant can make the code under test dump raw
        # bytes into a Catch2 failure message (a reverted raster guard wrote
        # 0xfe pixel data), and a strict decode turns that into a crash that
        # loses the verdict for the one hunk most likely to be killed.
        cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        errors='replace')) for who, cmd in planned]
    detected, unexplained = False, ''
    for who, p in procs:
        out, _ = p.communicate()
        out = out or ''
        log.write(out)
        state, why = suite_outcome('catch2', p.returncode, out, who)
        if state == DETECTED:
            detected = True
        elif state == INCONCLUSIVE and not unexplained:
            unexplained = why
    if detected:
        return DETECTED, ''
    return (INCONCLUSIVE, unexplained) if unexplained else (GREEN, '')


def run_confirmed(suites, name):
    """One suite run, and a second one when the first judged nothing.

    The mutant is still in the working tree, so a re-run costs a suite and no
    build -- the cheap half of a mutant. A runner that exits without naming a
    failing test is most often hiding a green run, and a green re-run is the
    verdict that FAILS the gate, so asking again resolves the common case in the
    safe direction. A second non-verdict is reported as one rather than guessed.
    """
    state, why = suites.run(name)
    if state != INCONCLUSIVE:
        return state, why
    print(f'[{why}; confirming] ', end='', flush=True)
    again, why_again = suites.run(name)
    if again != INCONCLUSIVE:
        return again, why_again
    return INCONCLUSIVE, f'{why}, and again on a re-run'


def judge(suites, names):
    """(state, reason) over every suite that can see one hunk.

    A detection anywhere is a kill and ends the question. Otherwise a runner
    that judged nothing outranks the ones that came back green: a suite that
    could not report cannot contribute a pass.
    """
    unexplained = ''
    for name in names:
        state, why = run_confirmed(suites, name)
        if state == DETECTED:
            return DETECTED, ''
        if state == INCONCLUSIVE and not unexplained:
            unexplained = why
    return (INCONCLUSIVE, unexplained) if unexplained else (GREEN, '')


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
            # Always available: the binary is a thing this machine can make, and
            # ensure_catch2_binary() makes it for any hunk that finds it absent.
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

    def ensure_catch2_binary(self):
        """Build the binary when it is absent, and say so. Returns seconds, or None.

        The Catch2 suite needs the binary even for a hunk whose strategy needs no
        build: a `data` mutant is live off the source tree, but the suite that
        judges it is still a compiled program. The binary can also go missing for
        reasons this run did not cause, at any point in a run that lasts hours,
        so the question is asked before every Catch2 run rather than once.

        Rebuilding is the recovery, not just a check. `make test-build` drops the
        binary in prune-orphan-test-objs, a sibling prerequisite of the link
        rather than a step before it, so a concurrent make can remove what this
        one linked and still exit 0; the orphan is gone by then, and the next
        build links and keeps it.
        """
        if self.catch2_bin.is_file():
            return None
        _, secs = build(self.root, self.jobs, self.log)
        if not self.catch2_bin.is_file():
            raise BuildUnavailable(
                f'{self.catch2_bin} is not there after a build, so the Catch2 '
                f'suite cannot start.\n'
                f'  In {self.args.log}, "[LD] helix-tests" then "Unit test binary '
                f'ready" with no\n'
                f'  "Test linking failed!" means the link succeeded and something '
                f'else took the\n'
                f'  output: prune-orphan-test-objs (mk/tests.mk) in a peer make '
                f'against this tree.\n'
                f'  Rerun with no other make running here.\n'
                f'  Anything else in that log means the build itself failed, and '
                f'it says how.')
        # Named where it happens: a hunk whose strategy is meant to cost no build
        # paying for one is a fact about the tree, not about the hunk.
        print(f'[test binary rebuilt, {secs:.0f}s] ', end='', flush=True)
        return secs

    def run(self, name):
        """(state, reason): what one run of this suite establishes."""
        if name == 'catch2':
            self.ensure_catch2_binary()
            return run_catch2(self.root, self.catch2_bin, self.args.tests,
                              self.args.shards, self.log)
        if name == 'bats':
            cmd = ['bats']
            if shutil.which('parallel'):
                cmd += ['--jobs', str(self.jobs), '--no-parallelize-within-files']
            cmd.append(self.args.shell_tests)
            r = run(cmd, cwd=self.root)
            self.log.write(r.stdout or '')
            return suite_outcome(name, r.returncode, r.stdout or '', 'the bats suite')
        if name == 'pytest':
            r = run([self.python, '-m', 'pytest', self.args.python_tests, '-q', '-x'],
                    cwd=self.root)
            self.log.write(r.stdout or '')
            return suite_outcome(name, r.returncode, r.stdout or '', 'pytest')
        raise AssertionError(name)


# ---------------------------------------------------------------------------
# THE TREE AND THE BINARY A VERDICT IS ABOUT
#
# A mutant's verdict is only about that mutant if the suite ran against a tree
# holding exactly its reversion, and a binary built from that tree. Two things
# can silently break that, and both borrow the PREVIOUS hunk's red: a restore
# that does not land leaves the previous reversion in a source file, and a build
# that reports success without relinking leaves the previous mutant's binary in
# place. Neither is visible in the verdict, so each is checked directly.
# ---------------------------------------------------------------------------
UNBUILT_MUTANT = ('the build reported success and left the test binary '
                  'untouched, so the suite ran a binary this mutant is not in')


class TreeDrift(RuntimeError):
    """A file the run may mutate stopped matching the tree the run started from.

    A reversion left in place makes the NEXT hunk's suite red for the PREVIOUS
    hunk's reason, and the verdict is recorded against the wrong hunk. There is
    no verdict left worth reporting, so this stops the run.
    """


def binary_fingerprint(path):
    """What identifies this build of the test binary, or None when it is absent.

    Size and modification time rather than a content digest: the question is
    whether the build wrote a new binary at all, which is precisely what a make
    with nothing to do answers differently from a link, and digesting gigabytes
    twice per mutant buys nothing over that.
    """
    try:
        st = path.stat()
    except OSError:
        return None
    return st.st_size, st.st_mtime_ns


def restore_file(root, rel, original):
    """Put one file back the way the run found it.

    touch(1)ed because make compares mtimes: a byte-identical restore with an
    older mtime leaves make nothing to do, and the next mutant's suite runs the
    previous mutant's binary.
    """
    target = root / rel
    if original is None:
        target.unlink(missing_ok=True)
        return
    target.write_bytes(original)
    os.utime(target, None)


def verify_pristine(root, pristine, when):
    """Raise unless every mutable file still matches what the run captured.

    Checked across ALL of them rather than the one last mutated: a restore that
    did not land is invisible in the file the next hunk is about.

    Nothing is written back. What is on disk may be a reversion this run failed
    to undo or an edit another session made to a shared tree mid-run, and from
    here the two are indistinguishable -- overwriting would destroy the second to
    repair the first.
    """
    drift = [rel for rel, want in sorted(pristine.items())
             if ((root / rel).read_bytes() if (root / rel).is_file() else None) != want]
    if drift:
        raise TreeDrift(
            f'{", ".join(drift)} no longer matches the tree this run started '
            f'from ({when}).\n'
            f'  A reversion left in place makes the next hunk\'s suite red for '
            f'this hunk\'s reason,\n'
            f'  so no verdict from here on would be about its own hunk.\n'
            f'  Nothing was written back: check `git diff` against what you '
            f'expect before rerunning.')


# The progress line each verdict gets. `killed` is deliberately the quiet one:
# it is the expected outcome, and everything else wants reading.
LOUD_VERDICT = {'survived': 'SURVIVED', 'inconclusive': 'INCONCLUSIVE'}


def verdict_line(verdict, note):
    if verdict == 'unreversible':
        return 'unreversible (hunk would not reverse cleanly)'
    if verdict == 'uncompilable':
        return f'uncompilable ({note})'
    if verdict == 'survived':
        return 'SURVIVED  <-- no test detects this change'
    if verdict == 'inconclusive':
        return f'INCONCLUSIVE  <-- {note}'
    return 'killed'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', default=None,
                    help='diff base (default: nearest fork point among the branch upstream, '
                         'release branches and main)')
    # 58 hunks out of a 3-hunk change is what a base from the wrong trunk looks
    # like, and it costs a build each to find that out one verdict at a time.
    ap.add_argument('--max-hunks', type=int, default=25,
                    help='confirm before mutating more than N hunks off an auto-chosen '
                         'base (0 disables the check)')
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
    ap.add_argument('--log', default=default_log_path(),
                    help='run log (default: named for this worktree, so runs in '
                         'different trees do not overwrite each other)')
    args = ap.parse_args()

    root = repo_root()
    base, base_why = resolve_base(root, args.base)
    hunks = collect_hunks(root, base)
    files = changed_files(root, base)
    untestable = load_untestable(root)

    # First, before anything is built: the base is what the run is about, and a
    # wrong one is otherwise invisible until an hour of verdicts has gone by.
    # Flushed: stdout to a pipe is block-buffered while stderr is not, so an
    # unflushed heading reaches a log file BELOW the epilogue it heads.
    print(base_line(root, base, base_why), flush=True)
    print(f'diff {len(hunks)} hunk(s) across {len(files)} file(s)')

    # ---- classify the whole change -------------------------------------
    verdict_of = {}       # path -> ('mutate', strategy) | ('inert'|'uncovered', reason)
    for path, is_gitlink in files:
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

    # A listing judges nothing, so it writes nothing: opening the log for one
    # would spend the single rotation slot the last real run's output has.
    if args.list_only:
        log = open(os.devnull, 'w')
    else:
        rotate_log(args.log)
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

    # A count far above the size of the change means the base is wrong, and the
    # bill for finding out later is a build per hunk. An explicit --base is the
    # author saying what they mean, so it is never questioned.
    if not args.base and args.max_hunks and len(mutable) > args.max_hunks:
        print(f'\n{len(mutable)} hunk(s) is more than --max-hunks {args.max_hunks}, '
              f'off a base nothing confirmed:')
        print(f'  {base_line(root, base, base_why)}')
        print('  A count far above the size of your own change means the base is '
              'wrong: name\n  the branch you cut from with --base <ref>, or pass '
              '--max-hunks 0 to accept it.')
        if not sys.stdin.isatty():
            print('FAIL: refusing to spend a build per hunk on an unconfirmed base.',
                  file=sys.stderr)
            return 4
        if input('  Continue anyway? [y/N] ').strip().lower() not in ('y', 'yes'):
            return 4

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
        state, why = suites.run(suite)
        if state == DETECTED:
            print(f'FAIL: baseline {suite} suite is RED. Fix it first, or every '
                  f'mutant will read as killed. See {args.log}', file=sys.stderr)
            return 2
        if state == INCONCLUSIVE:
            # No re-run here: the baseline exists to show the harness works, and
            # a runner that cannot report is a reason to stop rather than to ask
            # a second time.
            print(f'FAIL: baseline {suite} suite judged nothing - {why}. Every '
                  f'mutant would be measured by a suite that cannot report. '
                  f'See {args.log}', file=sys.stderr)
            return 2
        print(f'  {suite} green')

    # Read once, before the first mutation: see SAFETY.
    pristine = {}
    for h in mutable:
        target = root / h['file']
        pristine.setdefault(h['file'],
                            target.read_bytes() if target.is_file() else None)
    print('  baseline established\n')

    results, judged_by = [], set()
    for n, h in enumerate(mutable, 1):
        strategy = verdict_of[h['file']][1]
        plan = STRATEGIES[strategy]
        # A pristine entry of None means the change deletes the file: reverting
        # recreates it, and restoring means removing it again.
        original = pristine[h['file']]
        label = f'{h["file"]}:{h["line"]}'
        print(f'[{n}/{len(mutable)}] reverting {label} ... ', end='', flush=True)
        judged_by.update(plan['suites'])
        applied, why = apply_reverse(root, h['patch'])
        verdict, note = '', ''
        if not applied:
            verdict, note = 'unreversible', why.strip().splitlines()[:1]
        else:
            try:
                # Only a mutant whose strategy builds, judged by the suite that
                # binary IS, can be measured against the wrong one.
                fingerprinted = plan['build'] and 'catch2' in plan['suites']
                rebuilt = True
                if plan['build']:
                    was = binary_fingerprint(suites.catch2_bin)
                    built, secs = build(root, args.jobs, log)
                    if not built:
                        verdict, note = 'uncompilable', f'{secs:.0f}s'
                    elif fingerprinted:
                        rebuilt = binary_fingerprint(suites.catch2_bin) != was
                if not verdict:
                    state, unexplained = judge(suites, plan['suites'])
                    if state == DETECTED and not rebuilt:
                        state, unexplained = INCONCLUSIVE, UNBUILT_MUTANT
                    verdict = {DETECTED: 'killed', GREEN: 'survived',
                               INCONCLUSIVE: 'inconclusive'}[state]
                    note = unexplained
            finally:
                restore_file(root, h['file'], original)
        verify_pristine(root, pristine, f'after restoring {label}')
        print(verdict_line(verdict, note))
        results.append((label, verdict, note))

    # Leave the tree as found, with a rebuilt baseline binary so the next
    # `make test-run` is not testing the last mutant.
    if needs_build:
        print('\n=== restoring baseline binary ===')
        build(root, args.jobs, log)

    print('\n' + '=' * 68)
    for label, verdict, extra in results:
        mark = LOUD_VERDICT.get(verdict, verdict)
        why = f'  - {extra}' if verdict == 'inconclusive' and extra else ''
        print(f'  {mark:<13} {label}{why}')
    tally = {}
    for _, v, _ in results:
        tally[v] = tally.get(v, 0) + 1
    print(f'\n{len(results)} hunk(s) mutated: '
          + ', '.join(f'{v}={n}' for v, n in sorted(tally.items())))
    print(f'log: {args.log}')

    survived = [r for r in results if r[1] == 'survived']
    unproven = [r for r in results if r[1] in ('uncompilable', 'unreversible')]
    unjudged = [r for r in results if r[1] == 'inconclusive']
    if unproven or unjudged or uncovered or deferred:
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
        print(report_incomplete(uncovered, deferred, unproven, unjudged))
        return 0 if args.allow_incomplete else 3
    print('\nVERDICT: CLEAN - every changed hunk was mutated and every mutant was killed.')
    return 0


def report_incomplete(uncovered, deferred, unproven=(), unjudged=()):
    """The line that stops a partial run from reading like a whole one."""
    parts = []
    if uncovered:
        parts.append(f'{len(uncovered)} path(s) NOT COVERED')
    if unproven:
        parts.append(f'{len(unproven)} hunk(s) mutated but never judged by a test')
    if unjudged:
        parts.append(f'{len(unjudged)} hunk(s) whose suite exited without a verdict')
    if deferred:
        parts.append(f'{deferred} hunk(s) deferred by --only/--limit')
    return ('\nVERDICT: INCOMPLETE - this run did not examine the whole change: '
            + '; '.join(parts) + '.\n'
            'Nothing survived, but that is not evidence for the part above. '
            'Mutate it by hand and say so in the commit body, or accept it with '
            '--allow-incomplete.')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (BuildUnavailable, TreeDrift) as e:
        # Exit 2 is the harness-stopped code, alongside a broken or red baseline:
        # the run produced no verdict, which is not the same as a clean one.
        # Flushed first: stdout is block-buffered to a pipe while stderr is not,
        # so an unflushed progress line otherwise lands below the failure.
        sys.stdout.flush()
        print(f'\nFAIL: {e}', file=sys.stderr)
        sys.exit(2)
