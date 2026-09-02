#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Ratchet: a comment must not cite a commit SHA.
#
# Comments explain the code as it is. A SHA explains how it got here, which is what
# the commit message and git blame are for - and unlike them it rots, because the
# commit it names may be squashed, rebased away, or simply older than anyone reading.
# See CLAUDE.md § "Comments describe the code, not its past".
#
# Only the mechanically-checkable half of that rule lives here. Phrasing ("this used
# to...", a narrated bug, a recap of what a review found) is judgment and stays with
# the reviewer; a SHA is objective, so it gets a gate.
#
# Precision comes from asking git rather than guessing at hex. A 7-40 character hex
# token is only reported when `git cat-file -e <token>^{commit}` resolves it in THIS
# repository, so colour literals, content hashes, UUIDs and words like "deadbeef" are
# not candidates for a false positive - they simply are not commits.
#
# Ratchet, not a wall: the tree carries this debt already. Per-file counts may fall
# and never rise, so existing comments are grandfathered while new ones are refused.
# Lower a number by deleting the citation, then re-run with --write-baseline.
#
# Usage:
#   ./scripts/check_comment_archaeology.py
#   ./scripts/check_comment_archaeology.py --list
#   ./scripts/check_comment_archaeology.py --write-baseline

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

SCAN_DIRS = ("src", "include", "tests", "scripts", "mk")
SCAN_SUFFIXES = (".cpp", ".h", ".hpp", ".c", ".py", ".bats", ".sh", ".mk")
EXTRA_FILES = ("Makefile",)

# A hex run that could be an abbreviated or full SHA, as its own word.
HEX = re.compile(r"(?<![0-9a-zA-Z_])([0-9a-f]{7,40})(?![0-9a-zA-Z_])")

# Comment openers, per family. Deliberately coarse: a SHA inside a string literal is
# still a citation nobody wants, and treating those lines as comments only widens the
# net in the direction we want.
COMMENT = re.compile(r"(//|/\*|^\s*\*|#)")


def iter_sources(root: Path):
    for rel in SCAN_DIRS:
        base = root / rel
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in SCAN_SUFFIXES:
                yield path
    for name in EXTRA_FILES:
        path = root / name
        if path.is_file():
            yield path


def resolve_commits(root: Path, tokens: set[str]) -> set[str]:
    """Subset of tokens that name a real commit in this project's history.

    Resolved against the repository this script lives in, NOT against the tree being
    scanned: --repo-root may point at a fixture directory that is not a git
    repository at all, and a SHA cited there still refers to this project.

    One `git cat-file --batch-check` for the whole set. Asking per token would fork
    git once per candidate, which on this tree is well over a hundred processes and
    makes the gate cost more than everything around it.
    """
    git_root = Path(__file__).resolve().parent.parent
    if not tokens:
        return set()
    ordered = sorted(tokens)
    query = "".join(f"{t}^{{commit}}\n" for t in ordered)
    proc = subprocess.run(
        ["git", "cat-file", "--batch-check=%(objectname) %(objecttype)"],
        cwd=git_root,
        input=query,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        return set()
    real: set[str] = set()
    # One reply line per query line, in order. A resolvable ref answers with its type;
    # anything else answers "<token>^{commit} missing".
    for token, reply in zip(ordered, proc.stdout.splitlines()):
        if reply.endswith(" commit"):
            real.add(token)
    return real


def scan(root: Path) -> tuple[dict[str, int], list[str]]:
    hits: list[tuple[str, int, str, str]] = []
    candidates: set[str] = set()

    for path in iter_sources(root):
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        rel = path.relative_to(root).as_posix()
        for lineno, line in enumerate(lines, start=1):
            if not COMMENT.search(line):
                continue
            for match in HEX.finditer(line):
                token = match.group(1)
                candidates.add(token)
                hits.append((rel, lineno, token, line.strip()))

    real = resolve_commits(root, candidates)

    counts: dict[str, int] = {}
    detail: list[str] = []
    for rel, lineno, token, text in hits:
        if token not in real:
            continue
        counts[rel] = counts.get(rel, 0) + 1
        detail.append(f"{rel}:{lineno}: {token}\n      {text[:110]}")
    return counts, detail


def read_baseline(path: Path) -> dict[str, int]:
    if not path.is_file():
        return {}
    out: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        count, _, name = line.partition(" ")
        out[name] = int(count)
    return out


def write_baseline(path: Path, counts: dict[str, int]) -> None:
    body = "".join(f"{counts[k]} {k}\n" for k in sorted(counts))
    path.write_text(
        "# Commit SHAs cited in comments, per file. Ratchet: may fall, never rise.\n"
        "# Lower one by deleting the citation, then re-run with --write-baseline.\n"
        + body,
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path,
                        default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--baseline", type=Path, default=None)
    parser.add_argument("--write-baseline", action="store_true")
    parser.add_argument("--list", action="store_true", help="print every citation found")
    args = parser.parse_args()

    root = args.repo_root
    baseline_path = args.baseline or (root / "scripts" / "comment_archaeology_baseline.txt")

    counts, detail = scan(root)

    if args.list:
        for line in detail:
            print(f"  {line}")
        print(f"\n{sum(counts.values())} citation(s) in {len(counts)} file(s).")
        return 0

    if args.write_baseline:
        write_baseline(baseline_path, counts)
        print(f"Baseline written: {sum(counts.values())} citation(s) in {len(counts)} file(s).")
        return 0

    baseline = read_baseline(baseline_path)
    risen = {f: (n, baseline.get(f, 0)) for f, n in counts.items() if n > baseline.get(f, 0)}

    if risen:
        print("FAIL: a comment cites a commit SHA. Comments explain the code as it is;\n"
              "      how it got here belongs in the commit message.\n")
        for f, (now, was) in sorted(risen.items()):
            print(f"  {f}: {now} citation(s), baseline {was}")
        print("\n  Offending lines:")
        for line in detail:
            if line.split(":", 1)[0] in risen:
                print(f"    {line}")
        print("\n  Delete the SHA and state the constraint in the present tense instead.\n"
              "  See CLAUDE.md § \"Comments describe the code, not its past\".\n")
        return 1

    fallen = {f: (counts.get(f, 0), n) for f, n in baseline.items() if counts.get(f, 0) < n}
    total = sum(counts.values())
    if fallen:
        print(f"OK: {total} commit-SHA citation(s), and {len(fallen)} file(s) improved.\n"
              f"    Ratchet down with --write-baseline.")
    else:
        print(f"OK: {total} commit-SHA citation(s) in comments, none above baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
