#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail if grid_edit_mode.cpp grows a second cell-geometry computation.

GridEditMode::current_metrics() is the one place allowed to ask GridLayout for
the grid's dimensions; every other path takes a CellMetrics from it instead of
calling GridLayout directly, so gutter handling and int-vs-float rounding can't
drift between call sites.

Counting is per LINE, not per call: two matches on one source line register as
one hit. This is exactly why current_metrics() puts get_cols() and get_rows()
on separate lines — folding them onto one line would consume only one of
LIMIT's two slots and leave a free slot for a future duplicate elsewhere in
the file. Keep them on separate lines.

The pattern matches static `GridLayout::get_*` calls only. It does not see the
instance accessors (`GridLayout temp_grid(bp); temp_grid.cols();`) — five
`GridLayout temp_grid(...)` sites already exist in this file and are outside
this gate's reach. That's a known gap, not a bug; widening the pattern to
cover instance calls is future work, not this gate's job today.
"""

import re
import sys
from pathlib import Path

TARGET = Path("src/ui/home/grid_edit_mode.cpp")
PATTERN = re.compile(r"GridLayout::get_(cols|rows|dimensions)\b")
LIMIT = 2  # one get_cols + one get_rows, both inside current_metrics()


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    path = root / TARGET
    if not path.is_file():
        print(f"ERROR: {TARGET} not found", file=sys.stderr)
        return 1

    hits = [
        (n, line.strip())
        for n, line in enumerate(path.read_text().splitlines(), 1)
        if PATTERN.search(line)
    ]

    if len(hits) > LIMIT:
        print(f"ERROR: {TARGET} has {len(hits)} grid-dimension call sites (max {LIMIT}).")
        print("Cell geometry belongs in GridEditMode::current_metrics(); take a")
        print("CellMetrics from it instead of recomputing.")
        for n, text in hits:
            print(f"  {TARGET}:{n}: {text}")
        return 1

    print(f"OK: {TARGET} has {len(hits)} grid-dimension call site(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
