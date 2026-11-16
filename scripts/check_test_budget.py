#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gate: a branch does not grow the C++ test suite (prestonbrown/helixscreen#1965).

A large suite is not evidence of a tested codebase. Tests that reach private
state, restate the implementation or pin incidental pixels cost every refactor
and catch little, and the suite was growing faster than the code it tests. So a
branch pays for the tests it adds:

  test lines     net lines added under tests/ in C++ sources. A new test is
                 paid for by cutting or consolidating weaker ones. A commit on
                 the branch may grant a budget with a message line
                 `Test-budget: +N <reason>`, for a genuinely new subsystem;
                 the grants add up across the branch.
  TestAccess     net `<Name>TestAccess` references under tests/, src/ and
                 include/. A test that reaches private state is coupled to the
                 implementation; seed through the public API instead (#1757).
                 No budget.
  friend         net `friend` declarations in src/ and include/. No budget.

Measured from the branch's fork point (scripts/diff_base.py) to HEAD over
committed work only, so there is no baseline file for parallel branches to
conflict on: every landing can only hold the totals or lower them.

Usage:
  check_test_budget.py --base auto     # the fork point diff_base.py picks
  check_test_budget.py --base REV      # REV...HEAD
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

TEST_SOURCES = (":(glob)tests/**/*.cpp", ":(glob)tests/**/*.h")
ACCESS_SOURCES = TEST_SOURCES + (":(glob)src/**/*.cpp", ":(glob)src/**/*.h",
                                 ":(glob)include/**/*.h")
FRIEND_SOURCES = (":(glob)src/**/*.cpp", ":(glob)src/**/*.h", ":(glob)include/**/*.h")

TEST_ACCESS_RE = re.compile(r"\b\w+TestAccess\b")
FRIEND_RE = re.compile(r"^\s*friend\s")
BUDGET_RE = re.compile(r"^Test-budget:\s*\+(\d+)\s+\S", re.M)


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          capture_output=True, text=True).stdout


def net_lines(root: Path, base: str) -> int:
    net = 0
    for row in git(root, "diff", "--numstat", "--no-ext-diff", "--no-renames",
                   f"{base}...HEAD", "--", *TEST_SOURCES).splitlines():
        added, deleted, _ = row.split("\t", 2)
        if added != "-":
            net += int(added) - int(deleted)
    return net


def net_matches(root: Path, base: str, pattern: re.Pattern, paths) -> int:
    """Matches on added lines minus matches on removed lines."""
    net = 0
    for line in git(root, "diff", "-U0", "--no-ext-diff", "--no-renames",
                    f"{base}...HEAD", "--", *paths).splitlines():
        if line.startswith(("+++", "---")):
            continue
        if line.startswith("+"):
            net += len(pattern.findall(line[1:]))
        elif line.startswith("-"):
            net -= len(pattern.findall(line[1:]))
    return net


def granted_budget(root: Path, base: str) -> int:
    log = git(root, "log", "--format=%B", f"{base}..HEAD")
    return sum(int(n) for n in BUDGET_RE.findall(log))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", required=True, help="REV, or 'auto' for diff_base.py's fork point")
    ap.add_argument("--root", default=".")
    args = ap.parse_args(argv)
    root = Path(git(Path(args.root), "rev-parse", "--show-toplevel").strip())

    base = args.base
    if base == "auto":
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from diff_base import resolve_base  # noqa: E402
        base, why = resolve_base(root)
        if base == "HEAD":
            print(f"❌ Test budget: no fork point to measure from ({why}). A shallow or "
                  f"single-branch checkout has no upstream; fetch one, or pass --base REV")
            return 1
    if base == "HEAD" or git(root, "rev-parse", base) == git(root, "rev-parse", "HEAD"):
        print("✅ Test budget: no committed work to measure")
        return 0

    lines = net_lines(root, base)
    budget = granted_budget(root, base)
    access = net_matches(root, base, TEST_ACCESS_RE, ACCESS_SOURCES)
    friends = net_matches(root, base, FRIEND_RE, FRIEND_SOURCES)
    scope = f"{base[:12]}...HEAD"

    failures = []
    if lines > budget:
        granted = f", {budget} granted by Test-budget" if budget else ""
        failures.append(f"test lines grew by {lines}{granted}: cut or consolidate weaker "
                        f"tests to pay for new ones, or grant a budget for a new subsystem "
                        f"with a commit message line `Test-budget: +N <reason>`")
    if access > 0:
        failures.append(f"TestAccess references grew by {access}: seed through the public "
                        f"API, or drop a TestAccess use elsewhere")
    if friends > 0:
        failures.append(f"friend declarations grew by {friends}: test through the public "
                        f"interface instead of befriending the class")
    if failures:
        for f in failures:
            print(f"❌ {f}")
        print(f"   Measured {scope}. Policy: tests/CLAUDE.md § \"A test earns its place\".")
        return 1
    print(f"✅ Test budget ({scope}): test lines {lines:+d} (budget {budget}), "
          f"TestAccess {access:+d}, friend {friends:+d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
