#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail if production code indexes series_meta[] by a series *id*.

`ui_temp_graph_t::series_meta` is indexed by SLOT: `ui_temp_graph_add_series`
takes the first free entry of the fixed 16-slot array. `meta->id` is something
else entirely, a monotonically increasing handle (`next_series_id++`) that is
never reused. The two agree only until a series is removed:
`ui_temp_graph_remove_series` frees a slot without lowering `next_series_id`,
so after one remove-then-add cycle the same number means two different things.

`TempGraphHit::series_id` carries the handle. Indexing with it renders the
wrong series' name and colour, and past 16 add/remove cycles reads off the end
of the array. That shipped once in `temp_graph_tooltip_draw_cb` and was caught
in review rather than by a test, because the only observable symptom is drawn
pixels and this repo has no draw-pass readback.

Resolve a handle with `find_meta_by_id()` (declared in temp_graph_internal.h),
which scans for a matching `id` among populated slots. Iterating all slots by
index (`series_meta[i]`, `series_meta[s]`) is fine and is what the gate stays
quiet about.

Scope is src/ and include/ only. Tests legitimately name the bug in comments
and construct id/slot divergence on purpose, and comments are stripped before
matching so prose about the pattern never trips the gate.
"""

import re
import sys
from pathlib import Path

SCAN_DIRS = ("src", "include")
SUFFIXES = (".cpp", ".h", ".hpp", ".cc")

# series_meta[ ... series_id ... ]
PATTERN = re.compile(r"series_meta\s*\[[^\]]*\bseries_id\b[^\]]*\]")

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT = re.compile(r"//[^\n]*")


def strip_comments(text: str) -> str:
    """Blank out comments, preserving line structure so numbers stay accurate."""
    text = BLOCK_COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)
    return LINE_COMMENT.sub(lambda m: " " * len(m.group(0)), text)


def scan(root: Path):
    hits = []
    for d in SCAN_DIRS:
        base = root / d
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            try:
                source = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if "series_meta" not in source:
                continue
            for n, line in enumerate(strip_comments(source).splitlines(), 1):
                if PATTERN.search(line):
                    hits.append((path.relative_to(root), n, line.strip()))
    return hits


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    if len(sys.argv) > 1 and sys.argv[1] == "--scan-dir":
        root = Path(sys.argv[2]).resolve()

    hits = scan(root)
    if hits:
        print(f"ERROR: {len(hits)} site(s) index series_meta[] by a series id.")
        print("series_meta is SLOT-indexed; series_id is a handle that is never reused,")
        print("so the two diverge after any remove-then-add. Use find_meta_by_id().")
        for rel, n, text in hits:
            print(f"  {rel}:{n}: {text}")
        return 1

    print("OK: no series_meta[] indexed by a series id")
    return 0


if __name__ == "__main__":
    sys.exit(main())
