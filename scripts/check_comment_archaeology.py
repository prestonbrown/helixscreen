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
# Two checks live here. The SHA ratchet covers the whole tree. The phrase check reads
# only the comment lines a change ADDS (`--staged` at commit, `--base REV|auto` for a
# branch's committed work) for a short list of phrases that narrate history: see
# PHRASES. Phrasing in general is judgment and stays with the reviewer; each phrase
# on the list was measured against the tree and kept only where it almost never
# reads as present-tense prose. "no longer", "previously", "originally", a bare
# "this fix" and a bare date are not on it for that reason: "no longer than N", "the
# previously selected item", "the originally clicked child", "only apply this fix for
# 32bpp", "2026-06-15 12:00 UTC is 08:00 EDT". A deliberate one takes
# `ARCHAEOLOGY_OK: <reason>` on its line or the line above.
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
#   ./scripts/check_comment_archaeology.py --staged       # phrases, the index against HEAD
#   ./scripts/check_comment_archaeology.py --base auto    # phrases, fork point...HEAD

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ratchet  # noqa: E402

from check_constant_naming import parse_diff  # noqa: E402
from mutate_diff import comment_only_lines, comment_syntax  # noqa: E402

SCAN_DIRS = ("src", "include", "tests", "scripts", "mk")
SCAN_SUFFIXES = (".cpp", ".h", ".hpp", ".c", ".py", ".bats", ".sh", ".mk")
EXTRA_FILES = ("Makefile",)

# A hex run that could be an abbreviated or full SHA, as its own word.
HEX = re.compile(r"(?<![0-9a-zA-Z_])([0-9a-f]{7,40})(?![0-9a-zA-Z_])")

# Comment openers, per family. Deliberately coarse: a SHA inside a string literal is
# still a citation nobody wants, and treating those lines as comments only widens the
# net in the direction we want.
COMMENT = re.compile(r"(//|/\*|^\s*\*|#)")

# History and process phrases, read in comment text only. A pronoun before the
# past-habit "used to" is what separates history from purpose: "a pipe used to wake
# the loop", "the code used to parse the header" and "whatever used to be in the
# slot" describe the code as it is. A date counts only after "as of": "until
# <date>" as often states an expiry that is still ahead.
PHRASES = (
    ("used to", re.compile(r"\b(?:it|this|that|which|we|they)\s+used\s+to\b", re.I)),
    ("pre-fix", re.compile(r"\bpre-fix\b", re.I)),
    ("the old code", re.compile(r"\bthe\s+old\s+code\b", re.I)),
    ("before the fix", re.compile(r"\bbefore\s+(?:the|this)\s+fix\b", re.I)),
    ("fixes the bug where", re.compile(r"\bfix(?:es|ed)?\s+the\s+bug\s+where\b", re.I)),
    ("was changed to", re.compile(r"\bwas\s+changed\s+to\b", re.I)),
    ("dated note", re.compile(r"(?<![\w-])as\s+of\s+(?:19|20)\d\d-[01]\d-[0-3]\d\b", re.I)),
)
ESCAPE = "ARCHAEOLOGY_OK:"


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


def in_scope(path: str) -> bool:
    if path in EXTRA_FILES:
        return True
    return path.split("/", 1)[0] in SCAN_DIRS and Path(path).suffix in SCAN_SUFFIXES


def comment_key(text: str) -> str:
    """Comment text with its markers dropped and whitespace collapsed, so a line
    re-indented or reflowed onto a different opener compares equal to itself."""
    text = re.sub(r"^\s*(?://+|#+|/\*+|\*+)", "", text)
    text = re.sub(r"\*+/\s*$", "", text)
    return " ".join(text.split())


def removed_comments(diff_text: str) -> Counter:
    """The comment text of every line the diff removes, deleted files included.

    A removed line is read on its own: its comment, or the line itself when it
    is a block-comment continuation (` * text`) the lines around it would show.
    """
    keys: Counter = Counter()
    syntax = "none"
    for line in diff_text.splitlines():
        if line.startswith("--- "):
            syntax = comment_syntax(line[6:]) if line.startswith("--- a/") else "none"
            continue
        if not line.startswith("-") or not line[1:].strip():
            continue
        text = line[1:]
        comment = comment_only_lines(text, syntax)[0] if syntax != "none" else ""
        if not comment.strip() and text.lstrip().startswith("*"):
            comment = text
        key = comment_key(comment)
        if key:
            keys[key] += 1
    return keys


def phrase_findings(diff_text: str, read_new) -> list[tuple[str, int, str, str]]:
    """(path, lineno, phrase, line) for each added comment line naming history.

    read_new(path) returns the file's new content: a line inside a block comment
    is only known to be comment from the lines around it. An added comment whose
    text the same diff also removes is moved or reflowed, not written: each
    removed line excuses one added copy.
    """
    out = []
    moved = removed_comments(diff_text)
    for path, (added, _removed) in sorted(parse_diff(diff_text).items()):
        if not in_scope(path) or not added:
            continue
        lines = read_new(path).splitlines()
        comments = comment_only_lines("\n".join(lines), comment_syntax(path))
        for lineno, text in added:
            if not 1 <= lineno <= len(comments):
                continue
            if ESCAPE in text or (lineno >= 2 and ESCAPE in lines[lineno - 2]):
                continue
            for name, rx in PHRASES:
                if rx.search(comments[lineno - 1]):
                    key = comment_key(comments[lineno - 1])
                    if moved[key] > 0:
                        moved[key] -= 1
                    else:
                        out.append((path, lineno, name, text.strip()))
                    break
    return out


def git_out(root: Path, *args: str) -> str:
    r = subprocess.run(["git", *args], cwd=root, stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL)
    return r.stdout.decode("utf-8", "replace") if r.returncode == 0 else ""


def git_checked(root: Path, *args: str) -> str | None:
    """git's stdout, or None after printing its error: a diff that fails must
    not read as a diff with nothing in it."""
    r = subprocess.run(["git", *args], cwd=root, capture_output=True)
    if r.returncode != 0:
        err = r.stderr.decode("utf-8", "replace").strip()
        print(f"ERROR: git {' '.join(args)} failed: {err}", file=sys.stderr)
        return None
    return r.stdout.decode("utf-8", "replace")


# Renames off, deletions in: a renamed file's lines are then all added, and the
# deleted side supplies the removed lines that excuse the ones it carried over.
DIFF_ARGS = ("-U0", "--no-ext-diff", "--no-renames", "--diff-filter=AMD")


def check_phrases(root: Path, staged: bool, base: str | None) -> int:
    root = Path(git_out(root, "rev-parse", "--show-toplevel").strip() or root)
    if staged:
        diff = git_checked(root, "diff", "--cached", *DIFF_ARGS)
        scope = "staged"

        def read_new(p):
            return git_out(root, "show", f":{p}")
    else:
        if base == "auto":
            from diff_base import resolve_base
            base, _ = resolve_base(root)
        if base == "HEAD":
            print("OK: no committed work to check for history phrases.")
            return 0
        if subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{base}^{{commit}}"],
                          cwd=root, capture_output=True).returncode != 0:
            print(f"ERROR: --base {base} does not name a commit.", file=sys.stderr)
            return 2
        diff = git_checked(root, "diff", *DIFF_ARGS, f"{base}...HEAD")
        scope = f"{base[:12]}...HEAD"

        def read_new(p):
            return git_out(root, "show", f"HEAD:{p}")

    if diff is None:
        return 2
    hits = phrase_findings(diff, read_new)
    if not hits:
        print(f"OK: no history phrases in added comments ({scope}).")
        return 0
    print("FAIL: an added comment narrates history. Comments explain the code as it is;\n"
          "      how it got here belongs in the commit message.\n")
    for path, lineno, name, text in hits:
        print(f"  {path}:{lineno}: \"{name}\"\n      {text[:110]}")
    print(f"\n  State the constraint in the present tense instead, or mark a deliberate\n"
          f"  one `{ESCAPE} <reason>` on the line or the one above.\n")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path,
                        default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--baseline", type=Path, default=None)
    parser.add_argument("--write-baseline", action="store_true")
    parser.add_argument("--list", action="store_true", help="print every citation found")
    phrases = parser.add_mutually_exclusive_group()
    phrases.add_argument("--staged", action="store_true",
                         help="check added comment lines in the index for history phrases")
    phrases.add_argument("--base", metavar="REV",
                         help="check added comment lines in REV...HEAD; 'auto' = diff_base.py")
    args = parser.parse_args()

    root = args.repo_root
    if args.staged or args.base:
        return check_phrases(root, args.staged, args.base)
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
        return ratchet.under_baseline(
            "Comment archaeology",
            f"{ratchet.shown(baseline_path, root)} (python3 scripts/check_comment_archaeology.py --write-baseline)",
            [ratchet.lowered(f, was, now) for f, (now, was) in sorted(fallen.items())],
            f"OK: {total} commit-SHA citation(s), and {len(fallen)} file(s) improved.\n"
            f"    Ratchet down with --write-baseline.")
    print(f"OK: {total} commit-SHA citation(s) in comments, none above baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
