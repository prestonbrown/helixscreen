#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that UI code invalidates a printer image cache only when it changed.

`invalidate_printer_image_cache(path)` deletes every generated scaled `.bin` for
that source image. Regenerating one is a synchronous decode-and-resize on the
main thread (1.5-2.0 s on a 2-core board) plus a flash write, so a caller that
runs it on a path it is about to set again destroys the cache it is about to
need and stalls the UI on every refresh.

A refresh is not an image change. `PrinterImageWidget::refresh_printer_image()`
and `PrinterManagerOverlay::refresh_printer_info()` both re-resolve the active
image on every activation and usually get the same path back, so they must ask
`invalidate_printer_image_cache_if_changed(current, next)`, which is a no-op
when the two agree.

WHO MAY CALL THE UNCONDITIONAL FORM
  Only `src/system/`. `PrinterImageManager::import_image()` rewrites the pixels
  behind an existing path, so there the caches really are stale and the path
  really is unchanged - that is the one case the guarded form would wrongly
  skip. The definition itself also lives there.

WHY A LINT AND NOT A UNIT TEST
  The helper's own test cannot see a call site that re-inlines the
  unconditional form, and a revert of either UI caller to it survives the
  whole suite. Only reading the call sites catches that.

Exit 0 when no UI caller uses the unconditional form, 1 otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Subdirectories of the scanned tree where the unconditional form is the right
# answer. Anything else must use the _if_changed variant. Named relative to the
# scan root so a fixture tree under a different name still resolves.
ALLOWED_SUBDIRS = ("system",)

# invalidate_printer_image_cache( but NOT invalidate_printer_image_cache_if_changed(
CALL_RE = re.compile(r"\binvalidate_printer_image_cache\s*\(")

# Line comments and the bodies of block comments; enough to keep a doc comment
# that names the function from reading as a call site.
LINE_COMMENT_RE = re.compile(r"//.*$", re.M)
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)


def strip_comments(text: str) -> str:
    """Blank out comments, preserving newlines so line numbers still line up."""

    def blank(match: re.Match[str]) -> str:
        return re.sub(r"[^\n]", " ", match.group(0))

    return LINE_COMMENT_RE.sub(blank, BLOCK_COMMENT_RE.sub(blank, text))


def find_violations(src_root: Path) -> list[tuple[str, int, str]]:
    out: list[tuple[str, int, str]] = []
    for path in sorted(src_root.rglob("*.cpp")) + sorted(src_root.rglob("*.h")):
        inner = path.relative_to(src_root)
        if inner.parts[0] in ALLOWED_SUBDIRS:
            continue
        rel = (Path(src_root.name) / inner).as_posix()
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if "invalidate_printer_image_cache" not in text:
            continue
        lines = strip_comments(text).splitlines()
        for i, line in enumerate(lines, start=1):
            if CALL_RE.search(line):
                out.append((rel, i, line.strip()))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=str(REPO_ROOT / "src"),
                    help="tree to scan (fixture override for the gate's own tests)")
    args = ap.parse_args()

    violations = find_violations(Path(args.src).resolve())
    if not violations:
        print("✓ printer image invalidation: every UI caller is change-guarded")
        return 0

    print("UI code must not unconditionally invalidate a printer image cache.")
    print()
    for rel, lineno, text in violations:
        print(f"  {rel}:{lineno}")
        print(f"    {text}")
    print()
    print(f"{len(violations)} unguarded call(s) outside {chr(44).join(ALLOWED_SUBDIRS)}/.")
    print()
    print("A refresh re-resolves the same image most of the time, and deleting a")
    print("cache that still matches costs a synchronous decode-and-resize on the")
    print("main thread plus a flash write, every time the panel is opened.")
    print()
    print("Use invalidate_printer_image_cache_if_changed(current_path, next_path).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
