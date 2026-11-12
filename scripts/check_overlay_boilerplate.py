#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gate: hand-rolled overlay boilerplate in src/ui only shrinks.

OverlayBase::show() owns the create-once lifecycle and helix::lazy_global<T>
owns the process-wide instance. Two patterns mean someone wrote either by hand:

  already_exists   a log line containing "already exists": the guard log every
                   hand-rolled create() carries ("create() called but overlay
                   already exists"). User-facing strings do not count.
  static_unique    a `static std::unique_ptr<T> name` variable: a hand-rolled
                   global accessor slot instead of lazy_global<T>. A function
                   returning a unique_ptr does not count.

Counts are taken from code only (comments are ignored). A RATCHET: the baseline
holds today's counts, the gate fails when either one grows and says when the
baseline can drop.

Usage:
  check_overlay_boilerplate.py [--baseline FILE [--write-baseline]] [--repo-root DIR]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from source_strip import blank_cxx
import ratchet  # noqa: E402

PATTERNS = {
    "already_exists": re.compile(r"spdlog::\w+\([^;]*already exists"),
    "static_unique": re.compile(r"\bstatic\s+std::unique_ptr\s*<[^;()]*>\s*\w+\s*[;={]"),
}


def counts(root: Path) -> dict[str, int]:
    out = dict.fromkeys(PATTERNS, 0)
    base = root / "src" / "ui"
    for f in sorted(list(base.rglob("*.cpp")) + list(base.rglob("*.h"))):
        code = blank_cxx(f.read_text(errors="replace"), strings="keep")
        for key, pat in PATTERNS.items():
            out[key] += len(pat.findall(code))
    return out


def read_baseline(path: Path) -> dict[str, int]:
    out = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            key, _, val = line.partition("=")
            out[key.strip()] = int(val)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", type=Path)
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args()

    now = counts(args.repo_root)
    if args.write_baseline:
        if not args.baseline:
            print("--write-baseline needs --baseline PATH")
            return 2
        args.baseline.write_text(
            "# Hand-rolled overlay boilerplate in src/ui (scripts/check_overlay_boilerplate.py).\n"
            "# Shrink-only: convert the site to OverlayBase::show / lazy_global<T>, then\n"
            "# re-run with --write-baseline.\n"
            + "".join(f"{k}={v}\n" for k, v in now.items())
        )
        print(f"wrote {args.baseline}: {now}")
        return 0
    if not args.baseline:
        print(now)
        return 0

    base = read_baseline(args.baseline)
    grew = {k: (base.get(k, 0), v) for k, v in now.items() if v > base.get(k, 0)}
    if grew:
        for k, (was, is_) in grew.items():
            print(f"overlay boilerplate grew: {k} {was} -> {is_}")
        print("   Use OverlayBase::show() and lazy_global<T>() instead of hand-rolling them.")
        return 1
    shrank = [k for k, v in now.items() if v < base.get(k, 0)]
    if shrank:
        return ratchet.under_baseline(
            "Overlay boilerplate", f"{args.baseline} (re-run with --write-baseline)",
            [ratchet.lowered(k, base.get(k, 0), now.get(k, 0)) for k in shrank],
            f"✅ Overlay boilerplate: {now}, within {args.baseline}; re-run with "
            f"--write-baseline to hold the gain ({', '.join(shrank)})")
    print(f"✅ Overlay boilerplate: {now}, within {args.baseline}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
