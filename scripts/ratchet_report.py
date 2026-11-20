#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The weekly trend of the count ratchets (prestonbrown/helixscreen#2004).

No count ratchet blocks a landing, so nothing at a gate stops them drifting. This prints
each one's count at HEAD against the same count DAYS ago and against its baseline, where
it has one, so drift is seen weekly:

  bookkeeping   C++ test lines, TestAccess references, friend declarations, C++ files
                over the line cap, hardcoded pixels and unreferenced callbacks. Nothing
                at commit or at the gate reports them; this is their only reader.
  design        the architecture ratchets of scripts/design_ratchets.py (namespace,
                layering, vendor branches, declarative UI, swatches, LVGL 9.6 names,
                work markers, comment SHAs). A commit and a landing warn on each new
                site; here every site that appeared in the window is listed with the
                commit that added it (git blame at HEAD) and the reason given on a
                `Ratchet: <rule>: <reason>` line: by that commit, else by the
                first-parent merge that brought it in or another commit of that
                branch, so drift has an author.

Each count is taken on a commit's tree (git archive into a scratch directory), never on
the working tree, and every count uses this checkout's checkers, so the two columns
differ only by what the tree did. The older commit is the last first-parent commit
before the cutoff.

  The bookkeeping counts are whole-tree totals: C++ test lines, TestAccess references,
  friend declarations, and C++ files over check_growth_ratchets.FILE_CAP lines.
  check_hardcoded_pixels.py's total against the --max-allowed in
  scripts/qc/design_pixels.sh; check_orphan_callbacks.py's findings against
  scripts/orphan_callback_baseline.txt.

Run by .github/workflows/ratchet-report.yml every week, and by hand:
  scripts/ratchet_report.py                # HEAD against 7 days ago
  scripts/ratchet_report.py --days 30
  scripts/ratchet_report.py --since REV    # HEAD against REV
  scripts/ratchet_report.py --markdown     # a table for a job summary

Exit 0 whatever the trend; 2 when a count cannot be taken.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tarfile
import tempfile
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))
from check_growth_ratchets import FILE_CAP  # noqa: E402

TEST_ACCESS_RE = re.compile(r"\b\w+TestAccess\b")
FRIEND_RE = re.compile(r"^\s*friend\s")
TREE_PATHS = ("src", "include", "tests", "ui_xml", "firmware", "scripts")
MAX_ALLOWED_RE = re.compile(r"check_hardcoded_pixels\.py --max-allowed (\d+)")
PIXELS_TOTAL_RE = re.compile(r"^\s*TOTAL\s+(\d+)", re.M)


def git(root: Path, *args: str, binary: bool = False):
    out = subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True)
    return out.stdout if binary else out.stdout.decode()


def export(root: Path, rev: str, dest: Path) -> None:
    """REV's tree under TREE_PATHS, written to DEST."""
    present = [p for p in TREE_PATHS
               if git(root, "ls-tree", "--name-only", rev, "--", p).strip()]
    data = git(root, "archive", "--format=tar", rev, "--", *present, binary=True)
    with tarfile.open(fileobj=BytesIO(data)) as tar:
        try:
            tar.extractall(dest, filter="data")
        except TypeError:  # a Python without extraction filters; git archive is trusted
            tar.extractall(dest)


def cxx_files(tree: Path, roots: tuple[str, ...], exts: tuple[str, ...]):
    for r in roots:
        base = tree / r
        if base.is_dir():
            yield from (p for p in base.rglob("*") if p.is_file() and p.suffix in exts)


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def line_count(text: str) -> int:
    return text.count("\n") + (0 if not text or text.endswith("\n") else 1)


@dataclass
class Row:
    name: str
    count: int
    baseline: int | None = None


def counts(tree: Path) -> list[Row]:
    tests = [read(p) for p in cxx_files(tree, ("tests",), (".cpp", ".h"))]
    app = [read(p) for p in cxx_files(tree, ("src", "include"), (".cpp", ".h"))]
    rows = [
        Row("C++ test lines", sum(line_count(t) for t in tests)),
        Row("TestAccess references", sum(len(TEST_ACCESS_RE.findall(t)) for t in tests + app)),
        Row("friend declarations",
            sum(1 for t in app for line in t.splitlines() if FRIEND_RE.match(line))),
        Row(f"C++ files over {FILE_CAP} lines",
            sum(1 for t in tests + app if line_count(t) > FILE_CAP)),
    ]

    design = tree / "scripts/qc/design_pixels.sh"
    limit = MAX_ALLOWED_RE.search(read(design)) if design.is_file() else None
    out = subprocess.run([sys.executable, str(SCRIPTS / "check_hardcoded_pixels.py"),
                          "--summary"], cwd=tree, capture_output=True, text=True)
    total = PIXELS_TOTAL_RE.search(out.stdout)
    if not total:
        raise RuntimeError(f"check_hardcoded_pixels.py gave no total:\n{out.stdout}{out.stderr}")
    rows.append(Row("hardcoded pixels", int(total.group(1)),
                    int(limit.group(1)) if limit else None))

    out = subprocess.run([sys.executable, str(SCRIPTS / "check_orphan_callbacks.py"),
                          "--repo-root", str(tree), "--list"], capture_output=True, text=True)
    if out.returncode not in (0, 3):
        raise RuntimeError(f"check_orphan_callbacks.py failed:\n{out.stdout}{out.stderr}")
    found = [ln.split()[0] for ln in out.stdout.splitlines() if ":" in ln.split(" ")[0]]
    baseline_file = tree / "scripts/orphan_callback_baseline.txt"
    accepted = None
    if baseline_file.is_file():
        accepted = sum(1 for ln in read(baseline_file).splitlines()
                       if ln.strip() and not ln.startswith("#"))
    rows.append(Row("orphan callbacks", len(found), accepted))
    return rows


@dataclass
class Growth:
    rule: str
    site: object  # design_ratchets.Site
    commit: str
    subject: str
    reason: str


def design_sites(tree: Path) -> dict[str, list]:
    """Every site of every design rule in TREE, by rule; a rule that cannot be listed is left out."""
    import design_ratchets
    out = {}
    for rule in design_ratchets.RULES:
        try:
            out[rule.name] = rule.lister(tree, None)
        except Exception as e:  # noqa: BLE001 - one broken lister must not hide the rest
            print(f"ratchet_report: {rule.name}: {e}", file=sys.stderr)
    return out


def take(root: Path, rev: str):
    with tempfile.TemporaryDirectory(prefix="ratchet-report-") as d:
        export(root, rev, Path(d))
        return counts(Path(d)), design_sites(Path(d))


def blame(root: Path, path: str, line: int) -> str:
    out = subprocess.run(["git", "-C", str(root), "blame", "--porcelain", "-L", f"{line},{line}",
                          "HEAD", "--", path], capture_output=True, text=True)
    return out.stdout.split(" ", 1)[0] if out.returncode == 0 and out.stdout else ""


def carriers(root: Path, sha: str, mainline: list[str]) -> list[str]:
    """Messages that may state the reason for SHA's line when SHA's own does not: the
    first-parent merge that brought SHA into HEAD, then every commit of the branch that
    merge joined. A commit made on the first-parent line has none."""
    if sha in mainline:
        return []
    descendants = set(git(root, "rev-list", "--ancestry-path", f"{sha}..HEAD").split())
    # mainline runs newest first; the oldest first-parent descendant is the merge.
    merge = next((c for c in reversed(mainline) if c in descendants), "")
    if not merge:
        return []
    out = [git(root, "show", "-s", "--format=%B", merge)]
    parents = git(root, "show", "-s", "--format=%P", merge).split()
    if len(parents) > 1:
        out += [m for m in git(root, "log", "--format=%B%x00",
                                f"{parents[0]}..{parents[1]}").split("\0") if m.strip()]
    return out


def growth(root: Path, then_rev: str, now: dict, then: dict) -> list[Growth]:
    """Sites at HEAD that the older tree lacked, each with the commit that wrote its line."""
    import design_ratchets
    from collections import Counter
    rows = []
    mainline = git(root, "rev-list", "--first-parent", f"{then_rev}..HEAD").split()
    for rule, sites in now.items():
        if rule not in then:
            continue
        old = Counter((s.path, s.text) for s in then[rule])
        for site in sites:
            key = (site.path, site.text)
            if old[key]:
                old[key] -= 1
                continue
            sha = blame(root, site.path, site.line)
            subject = reason = ""
            if sha and subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor",
                                       sha, then_rev]).returncode != 0:
                full = sha
                body = git(root, "show", "-s", "--format=%h%n%s%n%b", sha)
                sha, subject, rest = (body.split("\n", 2) + ["", ""])[:3]
                for message in [f"{subject}\n{rest}", *carriers(root, full, mainline)]:
                    said = design_ratchets.stated_reasons(message).get(rule, [])
                    if said:
                        reason = "; ".join(said)
                        break
            else:
                sha = ""
            rows.append(Growth(rule, site, sha, subject, reason))
    return rows


def trend(now: int, then: int) -> str:
    if now == then:
        return "flat"
    return f"{now - then:+d} (worse)" if now > then else f"{now - then:+d} (better)"


def against(row: Row) -> str:
    if row.baseline is None:
        return "-"
    if row.count > row.baseline:
        return f"{row.baseline} (over by {row.count - row.baseline})"
    if row.count < row.baseline:
        return f"{row.baseline} (lower it to {row.count})"
    return f"{row.baseline} (at it)"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--days", type=int, default=7)
    ap.add_argument("--since", help="compare against this commit instead of DAYS ago")
    ap.add_argument("--markdown", action="store_true")
    ap.add_argument("--root", default=".")
    args = ap.parse_args(argv)
    root = Path(git(Path(args.root), "rev-parse", "--show-toplevel").strip())

    head = git(root, "rev-parse", "HEAD").strip()
    if args.since:
        then_rev, label = git(root, "rev-parse", args.since).strip(), args.since
    else:
        then_rev = git(root, "rev-list", "-1", "--first-parent",
                       f"--before={args.days} days ago", "HEAD").strip()
        label = f"{args.days} days ago"
        if not then_rev:
            print(f"ratchet_report: no commit is {args.days} days old in this history; "
                  f"fetch more, or pass --since REV", file=sys.stderr)
            return 2
    try:
        (now, design_now), (then, design_then) = take(root, head), take(root, then_rev)
    except (RuntimeError, subprocess.CalledProcessError) as e:
        print(f"ratchet_report: {e}", file=sys.stderr)
        return 2
    now += [Row(f"design: {r}", len(v)) for r, v in design_now.items() if r in design_then]
    then += [Row(f"design: {r}", len(v)) for r, v in design_then.items() if r in design_now]
    grown = growth(root, then_rev, design_now, design_then)

    def when(rev):
        return git(root, "show", "-s", "--format=%h %cs", rev).strip()

    title = f"Ratchet report: HEAD {when(head)} against {label}, {when(then_rev)}"
    header = ("ratchet", "now", label, "change", "baseline")
    table = [(n.name, str(n.count), str(t.count), trend(n.count, t.count), against(n))
             for n, t in zip(now, then)]
    if args.markdown:
        print(f"### {title}\n")
        print("| " + " | ".join(header) + " |")
        print("|" + "---|" * len(header))
        for r in table:
            print("| " + " | ".join(r) + " |")
        print_growth(grown, markdown=True)
        return 0
    widths = [max(len(r[i]) for r in (header, *table)) for i in range(len(header))]
    print(title)
    for r in (header, *table):
        print("  " + "  ".join(c.ljust(w) for c, w in zip(r, widths)).rstrip())
    print_growth(grown, markdown=False)
    return 0


def print_growth(grown: list[Growth], markdown: bool) -> None:
    """Each new design-ratchet site with the commit that added it and the reason it gave."""
    title = "New design-ratchet sites in the window"
    if not grown:
        print(f"\n{'### ' if markdown else ''}{title}: none")
        return
    print(f"\n{'### ' if markdown else ''}{title}: {len(grown)}")
    if markdown:
        print("\n| rule | site | commit | stated reason |\n|---|---|---|---|")
    for g in grown:
        where = f"{g.site.path}:{g.site.line}"
        commit = f"{g.commit} {g.subject}".strip() or "(older than the window: moved or reworded)"
        reason = g.reason or "(no reason stated)"
        if markdown:
            print(f"| {g.rule} | `{where}` | {commit.replace('|', '/')} | {reason.replace('|', '/')} |")
        else:
            print(f"  {g.rule}  {where}\n      {commit}\n      reason: {reason}")


if __name__ == "__main__":
    sys.exit(main())
