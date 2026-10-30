#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gate: a branch does not grow the codebase sideways (prestonbrown/helixscreen#1945).

Three ratchets, each measured from the branch's fork point (scripts/diff_base.py)
to HEAD over committed work, so there is no baseline file for parallel branches
to conflict on. A count may fall; a branch may not raise it.

  file size        No C++ file under src/, include/ or tests/ may end the branch
                   over FILE_CAP lines and larger than it started, unless lines
                   removed on the branch from files that were over the cap
                   cover the growth (so splitting a file passes). A rename is
                   compared against its old path. A branch commit may exempt one file
                   with a message line `File-size: <path> <reason>`, for a fix
                   that cannot wait for the split.
  global access    A class that receives a dependency through a constructor or
                   method parameter uses that parameter, not the global
                   accessor. Counted per file: calls to the accessor inside the
                   class body or inside an out-of-line `Class::method` body, for
                   every class that has a parameter of the dependency's type
                   anywhere in the tree (its class body, or the parameter list
                   of a `Class::method` definition). A file may not gain one.
  active instance  Static-storage pointers that hand an XML callback "the
                   current instance" (`static Foo* active_instance_;`,
                   `static Foo* s_active_`, an anonymous-namespace
                   `Foo* s_active_ctx = nullptr;`, ...). The branch's total may
                   not rise; an out-of-line `Foo* Foo::active_instance_ =`
                   definition is not a second pointer and is not counted.

Usage:
  check_growth_ratchets.py --base auto     # the fork point diff_base.py picks
  check_growth_ratchets.py --base REV      # REV...HEAD
  check_growth_ratchets.py --report        # current counts on HEAD, per file
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

# p97 of the tree's C++ files (median 180, p95 ~1080). Lower, and the frozen set
# takes in hundreds of ordinary ~1000-line test files every routine fix touches.
FILE_CAP = 1500

CXX_SOURCES = (":(glob)src/**/*.cpp", ":(glob)src/**/*.h", ":(glob)include/**/*.h",
               ":(glob)tests/**/*.cpp", ":(glob)tests/**/*.h")
APP_SOURCES = (":(glob)src/**/*.cpp", ":(glob)src/**/*.h", ":(glob)include/**/*.h")

SIZE_GRANT_RE = re.compile(r"^File-size:[ \t]*(\S+)[ \t]+\S", re.M)

# (label, parameter type, global accessor)
DEPENDENCIES = (
    ("PrinterState", r"PrinterState", "get_printer_state"),
    ("Moonraker API", r"I?MoonrakerAPI", "get_moonraker_api"),
)

_QUAL = r"(?:\w+::)*"
_PTR_TAIL = r"(?:const\s*)?[&*]+\s*(?:const\s+)?\w*\s*(?:=[^,;(){}]*)?[,)]"


def _param_re(type_re: str) -> re.Pattern:
    """A parameter (named or not, defaulted or not) of `type_re` by reference or pointer."""
    return re.compile(rf"[(,]\s*(?:const\s+)?{_QUAL}{type_re}\s*{_PTR_TAIL}")


_ACTIVE_NAME = r"(?:s_|g_)?\w*active\w*|(?:s_|g_)\w+_instance_?"
_KEYWORDS = {"return", "delete", "throw", "case", "goto", "else", "new"}
ACTIVE_STATIC_RE = re.compile(
    rf"^[ \t]*(?:inline\s+)?static\s+(?:inline\s+)?(?:const\s+)?([\w:<>]+)\s*\*\s*(?:const\s+)?"
    rf"({_ACTIVE_NAME})\s*(?:=\s*nullptr\s*|\{{\s*(?:nullptr)?\s*\}}\s*)?;", re.M)
ACTIVE_GLOBAL_RE = re.compile(
    rf"^[ \t]*(?:const\s+)?([\w:<>]+)\s*\*\s*((?:s_|g_)\w*active\w*|(?:s_|g_)\w+_instance_?)"
    rf"\s*(?:=\s*nullptr\s*|\{{\s*\}}\s*)?;", re.M)

CLASS_HEAD_RE = re.compile(
    r"(?<![\w])(?:class|struct)\s+(?:\[\[[^\]]*\]\]\s*)?(?:[A-Z_][A-Z0-9_]*\s+)*(\w+)\s*"
    r"(?:final\s*)?(?::[^;{}]*)?$")
ENUM_HEAD_RE = re.compile(r"\benum\b")
NAMESPACE_HEAD_RE = re.compile(r"(?:\bnamespace\b[\w:\s]*|\bextern\s+\"C\"\s*)$")
METHOD_HEAD_RE = re.compile(r"(\w+)\s*(?:<[^<>()]*>)?\s*::\s*(~?\w+|operator\s*[^\s(]+)\s*\(")


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          capture_output=True, text=True).stdout


def read_blobs(root: Path, rev: str, paths) -> dict[str, str]:
    """Contents of `paths` at `rev`; a path missing there is left out."""
    if not paths:
        return {}
    req = "".join(f"{rev}:{p}\n" for p in paths).encode()
    out = subprocess.run(["git", "-C", str(root), "cat-file", "--batch"], input=req,
                         check=True, capture_output=True).stdout
    blobs, pos = {}, 0
    for p in paths:
        nl = out.index(b"\n", pos)
        header = out[pos:nl].split()
        pos = nl + 1
        if header[-1] == b"missing":
            continue
        size = int(header[2])
        blobs[p] = out[pos:pos + size].decode("utf-8", "replace")
        pos += size + 1
    return blobs


def strip_code(text: str) -> str:
    """Comments, string and char literals blanked to spaces; newlines kept."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == 'R' and text.startswith('R"', i) and (i == 0 or not text[i - 1].isalnum()):
            m = re.match(r'R"([^(\s]*)\(', text[i:])
            if not m:
                out.append(c)
                i += 1
                continue
            end = text.find(")" + m.group(1) + '"', i)
            j = n if end < 0 else end + len(m.group(1)) + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c in "\"'":
            if c == "'" and i > 0 and text[i - 1].isalnum():  # digit separator 1'000
                out.append(" ")
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


STATIC_DECL_RE = re.compile(r"(?:^|[;{}:])\s*(?:inline\s+|constexpr\s+)*static\b[^;{}()=]*?\b(~?\w+)\s*\(")
STATIC_HEAD_RE = re.compile(r"^\s*(?:(?:public|private|protected)\s*:\s*)?(?:\[\[[^\]]*\]\]\s*)?"
                            r"(?:inline\s+|constexpr\s+)*static\b")


class Span:
    """A class body, or a member function: its owning class, its name when it is an
    out-of-line definition, and whether it is a static member (no instance to use)."""
    __slots__ = ("start", "end", "owner", "method", "static")

    def __init__(self, start, end, owner, method=None, static=False):
        self.start, self.end, self.owner = start, end, owner
        self.method, self.static = method, static


def scopes(code: str) -> list[Span]:
    """Every class body, every member function defined inside one, and every
    `Owner::method` definition with its parameter list. Namespaces are transparent."""
    spans = []
    stack = []  # (kind, owner, start, method, static); kind: ns | class | fn | block
    head = 0
    owner_fn = method_fn = None  # the out-of-line definition later top-level blocks continue
    for m in re.finditer(r"[{};]", code):
        ch, i = m.group(), m.start()
        in_class = bool(stack) and stack[-1][0] == "class"
        top = not stack or stack[-1][0] == "ns"
        if ch == ";":
            if top:
                owner_fn = method_fn = None
            head = i + 1
            continue
        if ch == "{":
            header = code[head:i]
            stripped = header.strip()
            if top and NAMESPACE_HEAD_RE.search(stripped):
                stack.append(("ns", None, i, None, False))
            elif (top or in_class) and (cm := CLASS_HEAD_RE.search(stripped)) \
                    and not ENUM_HEAD_RE.search(stripped):
                stack.append(("class", cm.group(1), i, None, False))
            elif top:
                start = i
                if "(" in header:
                    heads = METHOD_HEAD_RE.findall(header.split("(")[0] + "(")
                    owner_fn, method_fn = heads[-1] if heads else (None, None)
                    start = head
                stack.append(("fn", owner_fn, start, method_fn, False))
            elif in_class:
                is_static = "(" in header and bool(STATIC_HEAD_RE.match(header))
                stack.append(("member", stack[-1][1], head, None, is_static))
            else:
                stack.append(("block", None, i, None, False))
            head = i + 1
            continue
        if stack:
            kind, owner, start, method, static = stack.pop()
            if kind in ("class", "fn", "member") and owner:
                spans.append(Span(start, i, owner, method, static))
        head = i + 1
    return spans


def span_at(spans: list[Span], pos: int) -> Span | None:
    best = None
    for sp in spans:
        if sp.start <= pos <= sp.end and (best is None or sp.end - sp.start < best.end - best.start):
            best = sp
    return best


def static_methods(spans: list[Span], code: str) -> dict[str, set[str]]:
    found: dict[str, set[str]] = {}
    for sp in spans:
        if sp.method is None and not sp.static and code[sp.start] == "{":
            names = STATIC_DECL_RE.findall(code[sp.start:sp.end + 1])
            found.setdefault(sp.owner, set()).update(names)
    return found


def has_instance(sp: Span | None, statics: dict[str, set[str]]) -> bool:
    if sp is None or sp.static:
        return False
    return not (sp.method and sp.method in statics.get(sp.owner, ()))


def receivers(blobs: dict[str, str], param: re.Pattern):
    """Classes with a non-static member taking `param`, and every class's static methods."""
    parsed = []
    statics: dict[str, set[str]] = {}
    for text in blobs.values():
        code = strip_code(text)
        spans = scopes(code)
        parsed.append((code, spans))
        for owner, names in static_methods(spans, code).items():
            statics.setdefault(owner, set()).update(names)
    found = set()
    for code, spans in parsed:
        for m in param.finditer(code):
            sp = span_at(spans, m.start())
            if not has_instance(sp, statics):
                continue
            if sp.method is None and code[sp.start] == "{":
                # A declaration in the class body: static if its own statement says so.
                decl = max(code.rfind(c, sp.start, m.start()) for c in ";{}") + 1
                if STATIC_HEAD_RE.match(code[decl:m.start()]):
                    continue
            found.add(sp.owner)
    return found, statics


def accessor_calls(text: str, accessor: str, classes: set[str],
                   statics: dict[str, set[str]]) -> int:
    code = strip_code(text)
    calls = [m.start() for m in re.finditer(rf"\b{accessor}\s*\(", code)]
    if not calls:
        return 0
    spans = scopes(code)
    local = static_methods(spans, code)
    merged = {k: statics.get(k, set()) | local.get(k, set()) for k in set(statics) | set(local)}
    n = 0
    for pos in calls:
        sp = span_at(spans, pos)
        if sp is not None and sp.owner in classes and has_instance(sp, merged):
            n += 1
    return n


def active_instances(text: str) -> list[str]:
    code = strip_code(text)
    names = []
    for rx in (ACTIVE_STATIC_RE, ACTIVE_GLOBAL_RE):
        for m in rx.finditer(code):
            if m.group(1) not in _KEYWORDS:
                names.append(m.group(2))
    return names


def is_source(path: str, tests: bool = True) -> bool:
    roots = ("src/", "include/", "tests/") if tests else ("src/", "include/")
    if not path.startswith(roots):
        return False
    return path.endswith(".h") or (path.endswith(".cpp") and not path.startswith("include/"))


def list_files(root: Path, rev: str, tests: bool) -> list[str]:
    return [p for p in git(root, "ls-tree", "-r", "--name-only", rev).split()
            if is_source(p, tests)]


def tree_with(root: Path, rev: str, needle: str) -> dict[str, str]:
    """App sources at `rev` that contain the fixed string `needle`."""
    try:
        hits = git(root, "grep", "-l", "-F", needle, rev, "--", *APP_SOURCES).split()
    except subprocess.CalledProcessError:
        hits = []
    paths = [h.split(":", 1)[1] for h in hits]
    return read_blobs(root, rev, paths)


def receivers_at(root: Path, rev: str, type_re: str):
    return receivers(tree_with(root, rev, type_re.replace("I?", "")), _param_re(type_re))


def global_access_counts(root: Path, rev: str, type_re: str, accessor: str,
                         classes: set[str] | None = None, only=None) -> Counter:
    """Accessor calls per file inside `classes` (default: the classes receiving the
    dependency at `rev`), with static members judged by `rev`'s own declarations."""
    rev_classes, statics = receivers_at(root, rev, type_re)
    classes = rev_classes if classes is None else classes
    blobs = tree_with(root, rev, accessor)
    counts = Counter()
    for path, text in blobs.items():
        if only is not None and path not in only:
            continue
        n = accessor_calls(text, accessor, classes, statics)
        if n:
            counts[path] = n
    return counts


def changed(root: Path, base: str):
    """(old_path, new_path) for every C++ source the branch touched; '' for absent."""
    rows = []
    for row in git(root, "diff", "--name-status", "-M", "--no-ext-diff",
                   f"{base}...HEAD", "--", *CXX_SOURCES).splitlines():
        parts = row.split("\t")
        status = parts[0][0]
        if status == "R" or status == "C":
            rows.append((parts[1] if status == "R" else "", parts[2]))
        elif status == "A":
            rows.append(("", parts[1]))
        elif status == "D":
            rows.append((parts[1], ""))
        else:
            rows.append((parts[1], parts[1]))
    return rows


def size_grants(root: Path, fork: str) -> set[str]:
    """Paths a branch commit exempts from the size cap: `File-size: <path> <reason>`."""
    log = git(root, "log", "--format=%B", f"{fork}..HEAD")
    return set(SIZE_GRANT_RE.findall(log))


def line_count(text: str | None) -> int:
    if not text:
        return 0
    return text.count("\n") + (0 if text.endswith("\n") else 1)


def report(root: Path) -> int:
    for label, type_re, accessor in DEPENDENCIES:
        counts = global_access_counts(root, "HEAD", type_re, accessor)
        print(f"{label}: {sum(counts.values())} {accessor}() calls in {len(counts)} files "
              f"inside classes that receive it")
        for path, n in sorted(counts.items()):
            print(f"  {n:4d}  {path}")
    total = 0
    for path, text in read_blobs(root, "HEAD", list_files(root, "HEAD", tests=False)).items():
        for name in active_instances(text):
            total += 1
            print(f"  active instance: {path}: {name}")
    print(f"active instance pointers: {total}")
    big = [(line_count(t), p) for p, t in
           read_blobs(root, "HEAD", list_files(root, "HEAD", tests=True)).items()
           if line_count(t) > FILE_CAP]
    print(f"files over {FILE_CAP} lines: {len(big)}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", help="REV, or 'auto' for diff_base.py's fork point")
    ap.add_argument("--report", action="store_true", help="print HEAD's counts and exit")
    ap.add_argument("--root", default=".")
    args = ap.parse_args(argv)
    root = Path(git(Path(args.root), "rev-parse", "--show-toplevel").strip())
    if args.report:
        return report(root)
    if not args.base:
        ap.error("--base is required")

    base = args.base
    if base == "auto":
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from diff_base import resolve_base  # noqa: E402
        base, why = resolve_base(root)
        if base == "HEAD":
            print(f"❌ Growth ratchets: no fork point to measure from ({why}). A shallow "
                  f"or single-branch checkout has no upstream; fetch one, or pass --base REV")
            return 1
    if base == "HEAD" or git(root, "rev-parse", base) == git(root, "rev-parse", "HEAD"):
        print("✅ Growth ratchets: no committed work to measure")
        return 0
    fork = git(root, "merge-base", base, "HEAD").strip()
    scope = f"{base[:12]}...HEAD"

    rows = changed(root, base)
    old = read_blobs(root, fork, [o for o, _ in rows if o])
    new = read_blobs(root, "HEAD", [n for _, n in rows if n])
    failures = []
    granted = size_grants(root, fork)

    # Over-cap growth is judged across the branch, so a split passes: the lines a
    # new over-cap file holds are paid for by the lines removed from files that
    # were over the cap at the fork point.
    grown_files, paid = [], 0
    for o, n in rows:
        before = line_count(old.get(o)) if o else 0
        after = line_count(new.get(n)) if n else 0
        if before > FILE_CAP and after < before:
            paid += before - after
        if n and after > FILE_CAP and after > before and n not in granted:
            what = (f"new file, {after} lines" if not o else
                    f"{before} -> {after} lines" + (f", renamed from {o}" if o != n else ""))
            grown_files.append((n, after - before, what))
    if sum(g for _, g, _ in grown_files) > paid:
        credit = f"; {paid} lines left over-cap files on this branch" if paid else ""
        for n, _, what in grown_files:
            failures.append(f"{n} is over the {FILE_CAP}-line cap and grew ({what}{credit}): "
                            f"split it, pay for the growth with cuts in over-cap files, or "
                            f"grant it with a commit message line `File-size: {n} <reason>`")

    touched_old = {o for o, _ in rows if o}
    touched_new = {n for _, n in rows if n}
    renamed = {n: o for o, n in rows if o and n}
    renamed_to = {o: n for n, o in renamed.items()}
    for label, type_re, accessor in DEPENDENCIES:
        if not any(accessor in t for t in (*old.values(), *new.values())):
            continue
        # Both sides count the classes that received the dependency at the fork
        # point: a class that starts receiving it on this branch is mid-migration,
        # and is held to the ratchet from the next branch on.
        fork_classes, _ = receivers_at(root, fork, type_re)
        before = global_access_counts(root, fork, type_re, accessor, fork_classes, touched_old)
        after = global_access_counts(root, "HEAD", type_re, accessor, fork_classes, touched_new)
        for path, n in sorted(after.items()):
            was = before.get(renamed.get(path, path), 0)
            if n > was:
                failures.append(f"{path}: {accessor}() inside a class that receives "
                                f"{label} grew {was} -> {n}: use the parameter the class "
                                f"already has")

    def active(blobs):
        return Counter(f"{p}: {name}" for p, t in blobs.items() if not p.startswith("tests/")
                       for name in active_instances(t))
    act_before = active({renamed_to.get(p, p): t for p, t in old.items()})
    act_after = active(new)
    grown = sum(act_after.values()) - sum(act_before.values())
    if grown > 0:
        names = "; ".join(sorted((act_after - act_before).elements()))
        failures.append(f"static active-instance pointers grew by {grown} ({names}): route "
                        f"the XML callback through the widget's user data or an existing "
                        f"owner instead of a new static")

    if failures:
        for f in failures:
            print(f"❌ {f}")
        print(f"   Measured {scope}. Ratchets: prestonbrown/helixscreen#1945.")
        return 1
    print(f"✅ Growth ratchets ({scope}): {len(rows)} C++ files checked, active-instance "
          f"pointers {grown:+d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
