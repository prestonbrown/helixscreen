#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ownership gate for the Android staged-asset tree.

`android/app/src/main/assets/` is a GRADLE BUILD OUTPUT, not source. The
`copyAssets` task in android/app/build.gradle deletes the three staged trees and
re-copies them from ui_xml/, assets/ and config/ on every build, so the APK can
never ship stale content. Everything below is about the copy that survives on
disk between builds, and about anything that writes into it behind Gradle's
back.

WHY THIS EXISTS

The tree is ignored wholesale (android/.gitignore:5), so a snapshot from an old
local build sits in the working directory indefinitely, indistinguishable from
source to every tool that walks the repo. One such snapshot went 4 months and
248-of-287 files stale. The damage is not to the APK, it is to everyone reading
the repo:

  - Four separate lint gates independently grew a hand-written exclusion for
    this path (check_overlay_width.py, check_hardcoded_pixels.py,
    check_modal_chrome_budget.py, check_responsive_token_scope.py). Four authors
    each debugged the same false positives before working out that the files
    were not real.
  - A release checklist told developers the Android tree "carries its own copy",
    i.e. edit it too. Edits there are erased by the next Gradle build.
  - mk/filaments.mk cp'd assets/filaments.json into the staged tree after every
    `make regen-filaments`, refreshing exactly one file out of 592 and making a
    dead directory look maintained.

The last one is the shape this gate is really aimed at: a build rule that writes
into Gradle's output dir is invisible until someone diffs the whole tree.

WHAT IS FLAGGED (failures)

  - Any TRACKED file under android/app/src/main/assets/. The tree is a build
    output; committing into it means the next `copyAssets` silently deletes the
    committed file, and until then it shadows the real source.
  - Any Makefile / mk/*.mk / scripts/* line that writes into the staged tree
    (cp, mkdir, install, rsync, cat/tee/> redirection). Gradle owns that path.
    Reading from it is fine; only writes are flagged.

WHAT IS WARNED (never fails the build)

  - The staged tree exists and predates the newest file in ui_xml/, assets/ or
    config/. That is normal and harmless right after a source edit -- it means
    only "your last Gradle build is older than your last source edit". It is
    reported so that a stale tree is attributable when someone greps it by
    accident, not so anyone has to act on it.

Deliberately NOT flagged: the tree merely existing, or differing from source.
That is the steady state between builds and always will be.

Opt-out for a genuinely necessary write (there is currently none):
    cp foo android/app/src/main/assets/...   # ANDROID_STAGING_OK: <reason>

Usage:
    ./scripts/check_android_asset_staging.py
    ./scripts/check_android_asset_staging.py --list
    ./scripts/check_android_asset_staging.py --summary
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

OPT_OUT = "ANDROID_STAGING_OK"

STAGED_ROOT = Path("android/app/src/main/assets")
# The trees copyAssets rebuilds. Used only for the advisory staleness warning.
SOURCE_TREES = [Path("ui_xml"), Path("assets"), Path("config")]

# Files whose build rules could write into the staged tree. Gradle files are
# excluded on purpose: build.gradle is where those writes belong.
RULE_GLOBS = ["Makefile", "mk/*.mk", "scripts/*.sh", "scripts/*.py"]

# A write is a shell verb whose DESTINATION is the staged path. Matching the
# path alone would flag the four lint gates that legitimately exclude it, and
# this very file.
WRITE_PATTERNS = [
    re.compile(r"\b(?:cp|mv|install|rsync|ln)\b[^\n#]*android/app/src/main/assets"),
    re.compile(r"\bmkdir\b[^\n#]*android/app/src/main/assets"),
    re.compile(r"[>]{1,2}\s*[\"']?[^\s\"';|]*android/app/src/main/assets"),
    re.compile(r"\btee\b[^\n#]*android/app/src/main/assets"),
]


@dataclass
class Findings:
    tracked: list[str] = field(default_factory=list)
    writes: list[tuple[str, int, str]] = field(default_factory=list)
    stale_warning: str | None = None

    def any(self) -> bool:
        """Only the two hard rules gate the build; staleness is advisory."""
        return bool(self.tracked or self.writes)


def find_tracked(repo: Path) -> list[str]:
    """Files git tracks under the staged tree. Should always be empty."""
    try:
        out = subprocess.run(
            ["git", "ls-files", "--", str(STAGED_ROOT)],
            cwd=repo, capture_output=True, text=True, check=True,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []  # Not a git checkout (tarball, shallow CI stage) — skip.
    return [line for line in out.splitlines() if line.strip()]


def find_writes(repo: Path) -> list[tuple[str, int, str]]:
    """Build rules that write into Gradle's output dir."""
    hits: list[tuple[str, int, str]] = []
    seen: set[Path] = set()
    for glob in RULE_GLOBS:
        for path in sorted(repo.glob(glob)):
            if not path.is_file() or path in seen:
                continue
            seen.add(path)
            if path.resolve() == Path(__file__).resolve():
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for lineno, line in enumerate(text.splitlines(), 1):
                if OPT_OUT in line:
                    continue
                if any(p.search(line) for p in WRITE_PATTERNS):
                    rel = path.relative_to(repo).as_posix()
                    hits.append((rel, lineno, line.strip()))
    return hits


def newest_mtime(root: Path) -> float:
    """Newest regular-file mtime under root, or 0.0 if it has none."""
    newest = 0.0
    for p in root.rglob("*"):
        if p.is_file() and not p.is_symlink():
            try:
                newest = max(newest, p.stat().st_mtime)
            except OSError:
                continue
    return newest


def check_staleness(repo: Path) -> str | None:
    staged = repo / STAGED_ROOT
    if not staged.is_dir():
        return None
    staged_newest = newest_mtime(staged)
    if staged_newest == 0.0:
        return None
    for tree in SOURCE_TREES:
        src = repo / tree
        if not src.is_dir():
            continue
        src_newest = newest_mtime(src)
        if src_newest > staged_newest:
            age_days = (src_newest - staged_newest) / 86400.0
            return (
                f"staged tree is {age_days:.1f} days behind {tree}/ — it is last "
                "build's output, not source"
            )
    return None


def compute(repo: Path) -> Findings:
    return Findings(
        tracked=find_tracked(repo),
        writes=find_writes(repo),
        stale_warning=check_staleness(repo),
    )


def report(f: Findings) -> None:
    if f.tracked:
        print(
            f"FAIL: {len(f.tracked)} tracked file(s) under {STAGED_ROOT}/ — that "
            "tree is a Gradle build output and must not be committed.",
            file=sys.stderr,
        )
        for path in f.tracked[:20]:
            print(f"  {path}", file=sys.stderr)
        if len(f.tracked) > 20:
            print(f"  ... and {len(f.tracked) - 20} more", file=sys.stderr)
        print(
            "  Fix: git rm --cached the file and edit the real source under "
            "ui_xml/, assets/ or config/ instead.",
            file=sys.stderr,
        )
    if f.writes:
        print(
            f"FAIL: {len(f.writes)} build rule(s) write into {STAGED_ROOT}/ — "
            "the copyAssets task in android/app/build.gradle owns that path and "
            "wipes it on every build.",
            file=sys.stderr,
        )
        for path, lineno, line in f.writes:
            print(f"  {path}:{lineno}: {line}", file=sys.stderr)
        print(
            f"  Fix: drop the write. If it is genuinely required, annotate the "
            f"line with a `{OPT_OUT}: <reason>` comment.",
            file=sys.stderr,
        )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("--list", action="store_true", help="print every finding")
    ap.add_argument("--summary", action="store_true", help="one-line counts")
    args = ap.parse_args()

    f = compute(args.repo)

    if args.summary:
        print(
            f"android staging: {len(f.tracked)} tracked, {len(f.writes)} writes, "
            f"stale={'yes' if f.stale_warning else 'no'}"
        )
        return 1 if f.any() else 0

    if f.any():
        report(f)
        return 1

    if f.stale_warning:
        print(f"NOTE: {f.stale_warning}")
    print(f"OK: {STAGED_ROOT}/ is untracked and Gradle-owned.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
