#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Which commit is a diff-scoped gate measuring against?
#
# THE FAILURE MODE
#
# `merge-base HEAD main` is wrong for any branch cut from a maintenance branch.
# `main` is one long-lived branch among several: a branch grown out of
# `release/1.0` forks from THAT recently, and shares with main only where those
# two last agreed. Measured against main it is handed the entire release-vs-trunk
# divergence as the change under test -- dozens of foreign hunks and files, every
# verdict about somebody else's code.
#
# Neither gate has a symptom of its own for this. mutate-diff spends a build
# apiece to reach `uncompilable`, which is correctly not a kill, so the run just
# reads as slow and stubborn; cov-diff reports other people's files as never
# executed, which reads as a change that dragged in too much. The numbers are
# about the base, and nothing says so.
#
# THE RULE
#
# The base is the NEAREST fork point among the refs this branch could have been
# cut from: its configured upstream, then the release branches, then the trunk.
# Preference order breaks ties between refs that fork at the same commit; it
# never overrides distance, because a branch cut from main forks from main long
# after `release/1.0` parted from it.
#
# WHY THIS IS A MODULE
#
# scripts/mutate_diff.py and scripts/cov_diff.py ask the same question, and two
# hand-written copies of one rule agree by convention until they silently do
# not: one gate reporting a different scope than the other, over the same tree,
# is the shape that failure takes. Callers that copy either script into a
# fixture (the bats gates do) must copy this module beside it.

import subprocess
from pathlib import Path


def _git(args, root):
    return subprocess.run(['git'] + args, cwd=root, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, errors='replace')


def _out(args, root):
    """Stripped stdout of a successful git command, else ''."""
    r = _git(args, root)
    return r.stdout.strip() if r.returncode == 0 else ''


def short_sha(root, rev):
    """12-char sha for `rev`, or `rev` itself when it does not resolve."""
    return _out(['rev-parse', '--short=12', rev], root) or rev


def current_branch(root):
    """The checked-out branch name, or '' on a detached HEAD."""
    return _out(['symbolic-ref', '--quiet', '--short', 'HEAD'], root)


def tracked_upstream(root):
    """The branch's configured upstream, when it names a DIFFERENT branch.

    `git checkout -b fix/x origin/release/1.0` records the branch this work was
    cut from, which is the strongest evidence there is. A branch tracking its
    OWN remote copy -- what `git push -u` leaves behind -- is not that: the ref
    is this same branch, and taking it as the base would scope the run to
    whatever has not been pushed yet, so a fully-pushed branch would measure its
    change against itself and report a clean run over an empty diff.
    """
    branch = current_branch(root)
    if not branch:
        return None                       # detached HEAD tracks nothing
    upstream = _out(['rev-parse', '--abbrev-ref', '@{u}'], root)
    if not upstream:
        return None
    merge = _git(['config', f'branch.{branch}.merge'], root).stdout.strip()
    if merge == f'refs/heads/{branch}':
        return None
    return upstream


def base_candidates(root):
    """Refs this branch could have been cut from, in preference order.

    Order breaks ties between refs that fork at the same commit, so the branch's
    own upstream outranks a release branch, which outranks the trunk.

    The branch you are ON is dropped, which matters when that branch is main or
    a release branch: a ref forks from itself at HEAD, and a base of HEAD shrinks
    the run to uncommitted work, so commits made straight onto the trunk would
    drop out of their own run. Its remote-tracking copy stays, and that is the
    useful answer -- everything since the last push. tracked_upstream() turns the
    same mistake away in its other spelling.
    """
    cands = []
    upstream = tracked_upstream(root)
    if upstream:
        cands.append(upstream)
    cands += _out(['for-each-ref', '--format=%(refname:short)',
                   'refs/remotes/*/release/*', 'refs/heads/release/*'], root).split()
    cands += ['origin/main', 'main']
    here = current_branch(root)
    return [c for c in dict.fromkeys(cands) if c != here]   # first spelling of a ref wins


def auto_base(root):
    """(base, why): the nearest fork point among the candidate upstreams.

    Nearest, rather than main first, because a branch cut from a long-lived
    maintenance branch shares with main only where those two diverged, and
    everything the maintenance branch has done since would land in the diff as
    the change under test. See "THE FAILURE MODE" above.
    """
    candidates = base_candidates(root)
    best_ref = best = None
    for ref in candidates:
        mb = _out(['merge-base', 'HEAD', ref], root)
        if not mb:
            continue                      # ref does not exist here, or no common history
        if best is None:
            best_ref, best = ref, mb
            continue
        # A later fork point is a descendant of an earlier one. An equal one is
        # its own ancestor, so requiring a difference keeps the incumbent and
        # lets preference order settle the tie.
        if mb != best and _git(['merge-base', '--is-ancestor', best, mb], root).returncode == 0:
            best_ref, best = ref, mb
    if best is None:
        return 'HEAD', 'no candidate upstream resolved; diff is uncommitted work only'
    return best, (f'merge-base with {best_ref} '
                  f'(nearest of {len(candidates)} candidate upstream(s))')


def resolve_base(root, explicit=None):
    """(base, why) for a gate's --base argument. An explicit base is taken as given.

    The author naming a ref is the end of the question; second-guessing it would
    leave no way to say what a run is about.
    """
    if explicit:
        return explicit, f'--base {explicit}'
    return auto_base(root)


def base_line(root, base, why):
    """The first line of a gate's output: what this run is measuring.

    One spelling, so the two gates cannot drift into reporting the same fact
    differently and a reader moving between them reads the same line.
    """
    return f'base {short_sha(root, base)}  <- {why}'
