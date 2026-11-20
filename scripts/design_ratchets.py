#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The architecture ratchets: a warning that names the new site and asks for a better design.

Each rule below guards a design decision the tree already made: code lives under
helix::, lower layers do not include the UI, generic code asks a capability question
instead of naming a vendor, XML owns its widgets, a swatch comes from the one painter.
None of them blocks a commit or a landing (prestonbrown/helixscreen#2004), and none of
them is optional either. When a change adds a site, this prints a block for each rule it
grew: the rule, why it exists, where it is written down, and the new file:line. The
author's job is then to find the design that does not need the site, and only when there
is none to say why in the commit body with a line

    Ratchet: <rule>: <reason>

scripts/ratchet_report.py lists every growth each week with its commit and that reason.

A site is new when it sits on a line the change adds: the staged diff at commit
(--staged), the fork point to the working tree at a push or the land gate (--base auto).
The checkers that count each rule are unchanged and still answer for the whole tree.

  design_ratchets.py --staged          # the commit hook (scripts/qc/design_ratchets.sh)
  design_ratchets.py --base auto       # pre-push, make land-gate, CI
  design_ratchets.py --list-rules

Exit 0 when nothing grew, WARN_EXIT (scripts/ratchet.py) when a rule grew, 2 when the
change cannot be measured.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))
import ratchet  # noqa: E402

CXX = (".cpp", ".h")
SITE_RE = re.compile(
    r"^\s*(?:HAND\s+)?((?:src|include|tests|firmware|ui_xml|scripts)/[^\s:;]+):(\d+)\b:?\s*(.*)$")
REASON_RE = re.compile(r"^Ratchet:[ \t]*([\w-]+)[ \t]*:[ \t]*(\S.*)$", re.M)
LAYERING_GRANT_RE = re.compile(r"^Layering:[ \t]*(\S+)[ \t]+(\S.*)$", re.M)


@dataclass(frozen=True)
class Site:
    path: str
    line: int
    text: str


@dataclass(frozen=True)
class Rule:
    name: str
    why: str
    doc: str
    scope: re.Pattern
    # (root, files or None for the whole tree) -> sites
    lister: Callable[[Path, list[str] | None], list[Site]]
    # The rule names its sites by the files a change touches. A cross-file rule
    # (a subject nobody reads) lists the whole tree and keeps the added lines.
    per_file: bool = True
    # For a whole-tree rule: what an added line must contain for the rule to run.
    wake: re.Pattern | None = None


def parse_sites(text: str, root: Path) -> list[Site]:
    sites = []
    for raw in text.splitlines():
        m = SITE_RE.match(raw)
        if m:
            sites.append(Site(rel(m.group(1), root), int(m.group(2)), m.group(3).strip()))
    return sites


def rel(path: str, root: Path) -> str:
    """A path as the repo spells it, whichever root a checker printed it against."""
    p = Path(path)
    if not p.is_absolute():
        if (root / p).exists():
            return p.as_posix()
        p = (SCRIPTS.parent / p).resolve()
    try:
        return p.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return Path(path).as_posix()


def run(args: list[str], root: Path) -> str:
    r = subprocess.run([sys.executable, *args], cwd=root, capture_output=True, text=True)
    return r.stdout + r.stderr


def via_cli(script: str, *flags: str, abs_paths: bool = False, whole: tuple[str, ...] = ()):
    """A lister that runs a checker's own --list over the files, or the whole tree."""
    def lister(root: Path, files: list[str] | None) -> list[Site]:
        if files is not None and not files:
            return []
        if files is None:
            # A checker that reads paths against its own repo takes the directories
            # absolute; the rest scan their default directories from cwd.
            targets = [str(root / d) for d in whole if (root / d).exists()] if abs_paths else []
        else:
            targets = [str(root / f) if abs_paths else f for f in files]
        return parse_sites(run([str(SCRIPTS / script), *flags, *targets], root), root)
    return lister


def texts(root: Path, files: list[str] | None, dirs, suffixes) -> dict[str, str]:
    if files is None:
        files = [p.relative_to(root).as_posix() for d in dirs if (root / d).is_dir()
                 for p in sorted((root / d).rglob("*")) if p.is_file() and p.suffix in suffixes]
    out = {}
    for f in files:
        try:
            out[f] = (root / f).read_text(encoding="utf-8", errors="replace")
        except OSError:
            pass
    return out


def vendor_sites(root: Path, files: list[str] | None) -> list[Site]:
    import check_vendor_branches as vb
    sites = []
    for f, text in texts(root, files, vb.SCAN_DIRS, vb.SCAN_SUFFIXES).items():
        sites += parse_sites("\n".join(vb.count_text(f, text)[1]), root)
    return sites


def swatch_sites(root: Path, files: list[str] | None) -> list[Site]:
    import check_hand_rolled_swatch as sw
    sites = []
    for f, text in texts(root, files, sw.CPP_DIRS + sw.XML_DIRS,
                         sw.CPP_SUFFIXES + sw.XML_SUFFIXES).items():
        if sw.exempt(f):
            continue
        count = sw.count_xml if f.endswith(sw.XML_SUFFIXES) else sw.count_cpp
        sites += parse_sites("\n".join(count(f, text)[1]), root)
    return sites


def widget_pointer_sites(root: Path, files: list[str] | None) -> list[Site]:
    import check_cached_widget_pointers as cwp
    if files is None:
        paths = sorted(p for d in ("src", "include") if (root / d).is_dir()
                       for p in (root / d).rglob("*") if p.suffix in CXX)
    else:
        paths = [root / f for f in files]
    return parse_sites("\n".join(cwp.scan(root, paths)), root)


def overlay_sites(root: Path, files: list[str] | None) -> list[Site]:
    import check_overlay_boilerplate as ob
    from source_strip import blank_cxx
    sites = []
    for f, text in texts(root, files, ("src/ui",), CXX).items():
        if not f.startswith("src/ui/"):
            continue
        code = blank_cxx(text, strings="keep")
        for key, pat in ob.PATTERNS.items():
            for m in pat.finditer(code):
                n = code.count("\n", 0, m.start()) + 1
                sites.append(Site(f, n, f"{key}: {text.splitlines()[n - 1].strip()[:100]}"))
    return sites


def comment_sha_sites(root: Path, files: list[str] | None) -> list[Site]:
    import check_comment_archaeology as ca
    if files is None:
        return parse_sites("\n".join(ca.scan(root)[1]), root)
    hits = []
    for f, text in texts(root, [f for f in files if f.endswith(ca.SCAN_SUFFIXES)], (), ()).items():
        for n, line in enumerate(text.splitlines(), 1):
            if ca.COMMENT.search(line):
                hits += [(f, n, m.group(1)) for m in ca.HEX.finditer(line)]
    real = ca.resolve_commits(root, {t for _, _, t in hits})
    return [Site(f, n, t) for f, n, t in hits if t in real]


def orphan_subject_sites(root: Path, files: list[str] | None) -> list[Site]:
    out = run([str(SCRIPTS / "check_orphan_subjects.py"), "--list", "--repo-root", str(root)], root)
    sites, name = [], ""
    for raw in out.splitlines():
        if raw and not raw.startswith(" "):
            name = raw.strip()
            continue
        for part in raw.split(";"):
            m = SITE_RE.match(part)
            if m:
                sites.append(Site(rel(m.group(1), root), int(m.group(2)),
                                  f"subject \"{name}\" is registered and never read"))
    return sites


def layering_sites(root: Path, files: list[str] | None) -> list[Site]:
    import check_layering as cl
    tracked = [p for p in cl.git(root, "ls-files").split() if cl.is_source(p)] \
        if (root / ".git").exists() else \
        [p.relative_to(root).as_posix() for d in ("src", "include") if (root / d).is_dir()
         for p in (root / d).rglob("*") if cl.is_source(p.relative_to(root).as_posix())]
    targets = tracked if files is None else [f for f in files if cl.is_source(f)]
    known = set(tracked) | set(targets)
    owner = cl.owners(known)
    sites = []
    for f, text in texts(root, targets, (), ()).items():
        d = cl.src_dir(f) if f.startswith("src/") else owner.get(f)
        if d is None or d not in cl.LAYER_OF:
            continue
        for n, line in enumerate(text.splitlines(), 1):
            for inc in cl.INCLUDE_RE.findall(line):
                h = cl.resolve(inc, f, known)
                o = owner.get(h) if h and h != f else None
                if o in cl.LAYER_OF and cl.LAYER_OF[o] > cl.LAYER_OF[d]:
                    sites.append(Site(f, n, cl.describe(h, d, o)))
    return sites


SRC_CXX = re.compile(r"^(src|include)/.*\.(cpp|h)$")
RULES = (
    Rule("namespace",
         "Code outside helix:: sets the precedent the next global declaration follows.",
         "docs/devel/DEVELOPMENT.md (Namespace organization)", SRC_CXX,
         via_cli("check_namespace_compliance.py", "--list", whole=("src", "include"))),
    Rule("layering",
         "Lower layers emit data and events; when one includes a higher layer, nothing can "
         "be built or tested without the UI.",
         "docs/devel/ARCHITECTURE.md (The layer map)", SRC_CXX, layering_sites),
    Rule("vendor-branches",
         "A vendor belongs to one module per capability; a branch on it in generic code "
         "breaks the next printer that differs.",
         ".claude/rules/vendor-abstraction.md", SRC_CXX, vendor_sites),
    Rule("hand-rolled-swatch",
         "A swatch drawn by hand skips the contrast edge, the multi-color split and the "
         "finish every other swatch shows.",
         "docs/devel/COLOR_SWATCHES.md", re.compile(r"^((src|include)/.*\.(cpp|h)|ui_xml/.*\.xml)$"),
         swatch_sites),
    Rule("imperative-ui",
         "A widget XML owns, driven from C++, falls out of step with its subject and the "
         "layouts that bind it.",
         ".claude/rules/declarative-ui.md", SRC_CXX,
         via_cli("check_imperative_ui.py", "--list", whole=("src", "include"))),
    Rule("orphan-subjects",
         "A subject nobody reads renders nothing and costs every update that writes it.",
         ".claude/rules/declarative-ui.md", re.compile(r"^(src|include)/.*\.(cpp|h)$|^ui_xml/"),
         orphan_subject_sites, per_file=False, wake=re.compile(r"subject", re.I)),
    Rule("overlay-boilerplate",
         "OverlayBase::show() owns the create-once lifecycle and lazy_global<T> the "
         "instance; a hand-rolled copy drifts from both.",
         "docs/devel/ARCHITECTURE.md (Names you meet on day one)", re.compile(r"^src/ui/.*\.(cpp|h)$"),
         overlay_sites),
    Rule("raw-widget-pointers",
         "A raw lv_obj_t* member outlives its widget whenever someone else deletes the tree.",
         "docs/devel/THREADING.md (WidgetRef)", SRC_CXX, widget_pointer_sites),
    Rule("lvgl-deprecated",
         "LVGL 9.6 deprecates these names; each one is a later migration and a build "
         "warning today.",
         "docs/devel/ARCHITECTURE.md (lvgl96_rename.py), include/ui_obj_flags.h",
         re.compile(r"^(src|include|tests|firmware)/.*\.(c|cc|cpp|h|hpp|inc)$"),
         via_cli("lvgl96_rename.py", "--check", abs_paths=True,
                 whole=("src", "include", "tests", "firmware"))),
    Rule("todo-markers",
         "An unowned marker is a promise nobody tracks; it belongs in an issue.",
         "docs/devel/CI_CD_GUIDE.md (XML and work-marker checks)",
         re.compile(r"\.(cpp|c|h|hpp|mm|sh)$"),
         via_cli("check_todo_markers.py", "--list", whole=("src", "include", "scripts"))),
    Rule("comment-sha",
         "A comment explains the code as it is; how it got here belongs in the commit message.",
         "CLAUDE.md (Code Standards: Comment archaeology)",
         re.compile(r"^(src|include|tests|scripts)/"), comment_sha_sites),
)
RULE_BY_NAME = {r.name: r for r in RULES}


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          capture_output=True, text=True).stdout


# A block names this many new sites per rule and counts the rest.
SHOWN = 12
HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", re.M)


def added_lines(diff: str) -> dict[str, set[int]]:
    """{path: new-side line numbers} the diff adds."""
    out: dict[str, set[int]] = {}
    path = None
    for line in diff.splitlines():
        if line.startswith("+++ "):
            path = None if line[4:] == "/dev/null" else line[6:]
            if path is not None:
                out.setdefault(path, set())
        elif line.startswith("@@") and path is not None:
            m = HUNK_RE.match(line)
            start, count = int(m.group(1)), int(m.group(2) or 1)
            out[path].update(range(start, start + count))
    return out


def change(root: Path, staged: bool, base: str | None):
    """(added lines per path, reasons per rule from the branch's commit messages)."""
    if staged:
        diff = git(root, "diff", "--cached", "-U0", "--no-ext-diff", "--no-color",
                   "--diff-filter=AMR")
        return added_lines(diff), {}
    from diff_base import resolve_base
    rev, _why = resolve_base(root, None if base == "auto" else base)
    fork = git(root, "merge-base", rev, "HEAD").strip() if rev != "HEAD" else "HEAD"
    diff = git(root, "diff", "-U0", "--no-ext-diff", "--no-color", "--diff-filter=AMR", fork)
    log = git(root, "log", "--format=%B", f"{fork}..HEAD") if fork != "HEAD" else ""
    return added_lines(diff), stated_reasons(log)


def stated_reasons(messages: str) -> dict[str, list[str]]:
    reasons: dict[str, list[str]] = {}
    for rule, why in REASON_RE.findall(messages):
        reasons.setdefault(rule, []).append(why.strip())
    for path, why in LAYERING_GRANT_RE.findall(messages):
        reasons.setdefault("layering", []).append(f"{path}: {why.strip()}")
    return reasons


def added_text(root: Path, files: list[str], added: dict[str, set[int]]) -> str:
    out = []
    for f, text in texts(root, files, (), ()).items():
        lines = text.splitlines()
        out += [lines[n - 1] for n in sorted(added[f]) if n <= len(lines)]
    return "\n".join(out)


def new_sites(rule: Rule, root: Path, added: dict[str, set[int]]) -> list[Site]:
    files = sorted(f for f in added if rule.scope.search(f) and (root / f).is_file())
    if not files:
        return []
    # A whole-tree rule runs only when an added line could create one of its sites.
    if rule.wake and not rule.wake.search(added_text(root, files, added)):
        return []
    sites = rule.lister(root, files if rule.per_file else None)
    return sorted({s for s in sites if s.line in added.get(s.path, ())},
                  key=lambda s: (s.path, s.line))


def block(rule: Rule, sites: list[Site], reasons: list[str]) -> str:
    bar = "━" * 8
    lines = [f"⚠️  {bar} Design ratchet: {rule.name} grew by {len(sites)} {bar}",
             f"   Why the rule exists: {rule.why}",
             f"   Read: {rule.doc}"]
    lines += [f"     {s.path}:{s.line}: {s.text[:110]}" for s in sites[:SHOWN]]
    if len(sites) > SHOWN:
        lines.append(f"     ... and {len(sites) - SHOWN} more")
    if reasons:
        lines.append("   Reason stated on this branch: " + "; ".join(reasons))
    return "\n".join(lines)


FOOTER = ("   Stop and look for the design that does not need these before you go on.\n"
          "   A warning is not permission. If there is truly no better way, say why in\n"
          "   the commit body:  Ratchet: <rule>: <reason>   (scripts/ratchet_report.py\n"
          "   lists every growth weekly with its commit and that reason).")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--staged", action="store_true", help="the staged diff (commit hook)")
    mode.add_argument("--base", metavar="REV", help="REV, or 'auto' for the fork point")
    mode.add_argument("--list-rules", action="store_true")
    ap.add_argument("--root", default=".")
    args = ap.parse_args(argv)
    if args.list_rules:
        for r in RULES:
            print(f"{r.name}: {r.why} ({r.doc})")
        return 0
    try:
        root = Path(git(Path(args.root), "rev-parse", "--show-toplevel").strip())
        added, reasons = change(root, args.staged, args.base)
    except subprocess.CalledProcessError as e:
        print(f"design ratchets: cannot measure the change: {e.stderr or e}", file=sys.stderr)
        return 2
    os.chdir(root)
    blocks = []
    for rule in RULES:
        sites = new_sites(rule, root, added)
        if sites:
            blocks.append(block(rule, sites, reasons.get(rule.name, [])))
    if not blocks:
        print("✅ Design ratchets: no new sites")
        return 0
    print("\n".join(blocks))
    print(FOOTER)
    return ratchet.WARN_EXIT


if __name__ == "__main__":
    sys.exit(main())
