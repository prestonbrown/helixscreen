#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gate: a branch does not add an include that points up the layer stack
(prestonbrown/helixscreen#1946).

The layers, low to high, are LAYERS below: every `src/<dir>` belongs to one. A header
belongs to the directory that holds the `.cpp` of the same stem (`include/foo.h` is
src/printer's when `src/printer/foo.cpp` exists); a header under `src/<dir>/` belongs to
that directory. A header with no `.cpp` anywhere belongs to the directory its name or
its include/ subdirectory says it serves (HEADER_ONLY_OWNERS: `include/core/` is
src/core's, `ui_*.h`, `include/ui/` and `include/helix/ui/` are src/ui's, `include/led/`
src/led's, `include/system/` src/system's, `include/state/` src/printer's); one that matches no row belongs to
nobody, and its includes are not counted in either direction until it is moved.

An upward edge is a file in one layer including a header owned by a higher layer. The
ratchet is measured from the branch's fork point (scripts/diff_base.py) to HEAD over
committed work, so there is no baseline file for parallel branches to conflict on:

  per file      A file may lose upward edges; it may not end the branch with more than
                it started with. A rename is compared against its old path. A branch
                commit may grant one file with a message line
                `Layering: <path> <reason>`, for a fix that cannot wait.
  cleared pairs CLEARED_PAIRS names (includer directory, header basename) pairs a
                refactor has emptied; any such include anywhere at HEAD fails, whether
                or not the branch touched it, so a cleared edge cannot come back.

`--report` prints HEAD's directory-to-directory include matrix and every upward pair,
counted the same way the ratchet counts them.

Usage:
  check_layering.py --base auto      # the fork point diff_base.py picks
  check_layering.py --base REV       # REV...HEAD
  check_layering.py --report         # HEAD's matrix and upward edges
"""

from __future__ import annotations

import os
import argparse
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

# Low to high. A directory missing here is unlayered: its edges are reported by
# --report under "unlayered" and never fail the gate, so a new src/ directory is
# added to a layer deliberately rather than judged by accident.
LAYERS = (
    ("core", ("util", "utils", "config", "generated", "core")),
    ("platform", ("network", "bluetooth", "input", "platform")),
    ("moonraker", ("api",)),
    ("state", ("printer", "temperature", "spoolman", "print", "calibration", "sensors",
               "led", "plugin", "system")),
    ("rendering", ("rendering",)),
    ("ui", ("ui",)),
    ("app", ("application", "abort", "remote", "tools", "lvgl-demo")),
)
LAYER_OF = {d: i for i, (_, dirs) in enumerate(LAYERS) for d in dirs}
LAYER_NAME = {i: name for i, (name, _) in enumerate(LAYERS)}

# (includer directory, header basename) pairs no file may include again. A
# refactor that empties a pair adds it here in the same change.
CLEARED_PAIRS: tuple[tuple[str, str], ...] = ()

# A header with no same-stem .cpp takes its owner from the first row whose prefix
# matches its path or its basename. A header matching no row is header-only and
# belongs to nobody. Rows name the layer a header serves, not where it is used.
HEADER_ONLY_OWNERS: tuple[tuple[str, str], ...] = (
    ("include/core/", "core"),
    ("include/ui/", "ui"),
    ("include/helix/ui/", "ui"),
    ("ui_", "ui"),
    ("include/led/", "led"),
    ("include/system/", "system"),
    ("include/state/", "printer"),
)

SOURCES = (":(glob)src/**/*.cpp", ":(glob)src/**/*.h", ":(glob)include/**/*.h")
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.M)
GRANT_RE = re.compile(r"^Layering:[ \t]*(\S+)[ \t]+\S", re.M)
INCLUDE_ROOTS = ("include/", "src/generated/", "")


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


def is_source(path: str) -> bool:
    if path.startswith("src/"):
        return path.endswith((".cpp", ".h"))
    return path.startswith("include/") and path.endswith(".h")


def list_files(root: Path, rev: str) -> list[str]:
    return [p for p in git(root, "ls-tree", "-r", "--name-only", rev).split()
            if is_source(p)]


def src_dir(path: str) -> str | None:
    """The `src/<dir>` a file sits in, or None outside src/."""
    parts = path.split("/")
    return parts[1] if parts[0] == "src" and len(parts) > 2 else None


def owners(files) -> dict[str, str]:
    """Owning directory of every header: the directory of the `.cpp` with the same
    stem (the lowest layer when several share a stem), or the header's own `src/<dir>`.
    A header with neither is absent: header-only."""
    stems: dict[str, str] = {}
    for f in files:
        d = src_dir(f)
        if d and f.endswith(".cpp"):
            stem = Path(f).stem
            if stem not in stems or LAYER_OF.get(d, 99) < LAYER_OF.get(stems[stem], 99):
                stems[stem] = d
    out = {}
    for f in files:
        if not f.endswith(".h"):
            continue
        d = src_dir(f) if f.startswith("src/") else stems.get(Path(f).stem)
        if d is None:
            name = Path(f).name
            d = next((o for p, o in HEADER_ONLY_OWNERS
                      if f.startswith(p) or name.startswith(p)), None)
        if d:
            out[f] = d
    return out


def resolve(inc: str, includer: str, known: set[str]) -> str | None:
    cands = [str(Path(includer).parent / inc)] + [r + inc for r in INCLUDE_ROOTS]
    for c in cands:
        c = os.path.normpath(c)
        if c in known:
            return c
    return None


def upward_edges(blobs: dict[str, str], owner: dict[str, str]):
    """{file: [(header, includer_dir, owner_dir)]} for every upward include at one rev."""
    known = set(blobs)
    out: dict[str, list[tuple[str, str, str]]] = defaultdict(list)
    for f, text in blobs.items():
        d = src_dir(f) if f.startswith("src/") else owner.get(f)
        if d is None or d not in LAYER_OF:
            continue
        for inc in INCLUDE_RE.findall(text):
            h = resolve(inc, f, known)
            if h is None or h == f:
                continue
            o = owner.get(h)
            if o is None or o not in LAYER_OF:
                continue
            if LAYER_OF[o] > LAYER_OF[d]:
                out[f].append((h, d, o))
    return out


def tree(root: Path, rev: str):
    files = list_files(root, rev)
    blobs = read_blobs(root, rev, files)
    owner = owners(files)
    return blobs, owner, upward_edges(blobs, owner)


def changed(root: Path, base: str):
    """(old_path, new_path) for every source the branch touched; '' for absent."""
    rows = []
    for row in git(root, "diff", "--name-status", "-M", "--no-ext-diff",
                   f"{base}...HEAD", "--", *SOURCES).splitlines():
        parts = row.split("\t")
        status = parts[0][0]
        if status in "RC":
            rows.append((parts[1] if status == "R" else "", parts[2]))
        elif status == "A":
            rows.append(("", parts[1]))
        elif status == "D":
            rows.append((parts[1], ""))
        else:
            rows.append((parts[1], parts[1]))
    return rows


def grants(root: Path, fork: str) -> set[str]:
    return set(GRANT_RE.findall(git(root, "log", "--format=%B", f"{fork}..HEAD")))


def cleared_hits(edges) -> list[str]:
    hits = []
    for f, es in sorted(edges.items()):
        for h, d, _ in es:
            if (d, Path(h).name) in CLEARED_PAIRS:
                hits.append(f"{f} includes {h}: src/{d} no longer includes it anywhere")
    return hits


def describe(h: str, d: str, o: str) -> str:
    return f"{h} (src/{o}, {LAYER_NAME[LAYER_OF[o]]}) from {LAYER_NAME[LAYER_OF[d]]}"


def report(root: Path) -> int:
    blobs, owner, edges = tree(root, "HEAD")
    known = set(blobs)
    matrix: dict[str, Counter] = defaultdict(Counter)
    unlayered: Counter = Counter()
    for f, text in blobs.items():
        d = src_dir(f) if f.startswith("src/") else owner.get(f)
        if d is None:
            continue
        for inc in INCLUDE_RE.findall(text):
            h = resolve(inc, f, known)
            o = owner.get(h) if h else None
            if o is None:
                continue
            matrix[d][o] += 1
            if d not in LAYER_OF or o not in LAYER_OF:
                unlayered[d if d not in LAYER_OF else o] += 1
    dirs = sorted(matrix, key=lambda d: (LAYER_OF.get(d, 99), d))
    cols = sorted({o for c in matrix.values() for o in c} | set(dirs),
                  key=lambda d: (LAYER_OF.get(d, 99), d))
    print("Direct include edges, includer src/<dir> (rows) -> owning src/<dir> (columns); "
          "header-only headers not counted")
    print(f"{'':14}" + "".join(f"{c[:7]:>8}" for c in cols))
    for r in dirs:
        print(f"{r:14}" + "".join(f"{matrix[r][c] or '':>8}" for c in cols))
    pairs = Counter((d, o) for es in edges.values() for _, d, o in es)
    files = sum(1 for es in edges.values() if es)
    print(f"\nUpward edges: {sum(pairs.values())} in {files} files")
    for (d, o), n in pairs.most_common():
        heads = Counter(Path(h).name for es in edges.values() for h, x, y in es if (x, y) == (d, o))
        print(f"  src/{d} -> src/{o}: {n}; " +
              ", ".join(f"{h}({c})" for h, c in heads.most_common(6)))
    if unlayered:
        print("\nUnlayered directories (add each to LAYERS): " +
              ", ".join(f"src/{d} ({n} edges)" for d, n in unlayered.most_common()))
    for hit in cleared_hits(edges):
        print(f"  cleared pair present: {hit}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", help="REV, or 'auto' for diff_base.py's fork point")
    ap.add_argument("--report", action="store_true", help="print HEAD's matrix and exit")
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
            print(f"❌ Layering: no fork point to measure from ({why}). A shallow or "
                  f"single-branch checkout has no upstream; fetch one, or pass --base REV")
            return 1
    failures = []
    _, _, head_edges = tree(root, "HEAD")
    failures += cleared_hits(head_edges)
    if base == "HEAD" or git(root, "rev-parse", base) == git(root, "rev-parse", "HEAD"):
        if failures:
            for f in failures:
                print(f"❌ {f}")
            return 1
        print("✅ Layering: no committed work to measure")
        return 0
    fork = git(root, "merge-base", base, "HEAD").strip()
    scope = f"{base[:12]}...HEAD"
    _, _, fork_edges = tree(root, fork)
    rows = changed(root, base)
    renamed = {n: o for o, n in rows if o and n and o != n}
    granted = grants(root, fork)

    # Every file is compared, not only the touched ones: a header's owner can move
    # to a higher layer on this branch and turn an untouched include upward.
    for f in sorted(head_edges):
        if f in granted:
            continue
        was = fork_edges.get(renamed.get(f, f), [])
        now = head_edges[f]
        if len(now) <= len(was):
            continue
        added = Counter((h, d, o) for h, d, o in now) - Counter(was)
        which = "; ".join(describe(h, d, o) for h, d, o in sorted(added))
        failures.append(f"{f}: upward includes grew {len(was)} -> {len(now)}: {which}. "
                        f"Lower layers emit data and events; the UI presents them. Move "
                        f"the shared piece down, or grant this file with a commit message "
                        f"line `Layering: {f} <reason>`")
    if failures:
        for f in failures:
            print(f"❌ {f}")
        print(f"   Measured {scope}. Layer map: docs/devel/ARCHITECTURE.md; gate: "
              f"prestonbrown/helixscreen#1946.")
        return 1
    before = sum(len(v) for v in fork_edges.values())
    after = sum(len(v) for v in head_edges.values())
    print(f"✅ Layering ({scope}): upward includes {before} -> {after}, "
          f"{len(rows)} files touched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
