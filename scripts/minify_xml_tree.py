#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Minify a STAGED tree of XML files in place, for release packaging.

Why this exists: ui_xml/ is loaded at runtime, and lv_xml_component.c keeps a
verbatim copy of every component's <view> source text alive for the whole
session (it is re-parsed on each lv_xml_create, so it cannot be freed). Comments
and indentation in the shipped XML are therefore permanently resident heap on
every device. On a 114 MB CC1 that is ~300 KB of the ~5 MB heap, allocated
during boot and never returned -- it sits at the bottom of the arena and pins
the brk, so transients above it can never be released to the OS either.

The minifier itself is NOT reimplemented here. It is imported from
esp32_stage_assets.py, which has shipped and been exercised by the ESP32
packaging path (and is covered by tests/python/test_esp32_stage_assets.py), so
both packaging paths stay on one tokenizer-aware implementation.

SAFETY: this rewrites files in place and is only ever meant to run against a
staged COPY of ui_xml/, never the source tree. It refuses to run on a directory
inside the repo's own ui_xml/ unless --force is given.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from esp32_stage_assets import minify_xml  # noqa: E402


def human(n: int) -> str:
    return f"{n:,} B" if n < 1024 else f"{n / 1024:.1f} KB"


def source_ui_xml_root() -> Path:
    """The one tree this script must never rewrite: the repo's own ui_xml/."""
    return (Path(__file__).resolve().parent.parent / "ui_xml").resolve()


def minify_tree(root: Path, force: bool = False, protected: Path | None = None) -> tuple[int, int, int]:
    """Rewrite every *.xml under `root`. Returns (files, before, after).

    `protected` is the tree the guard refuses to touch; it defaults to the repo's
    real ui_xml/ and exists so tests can point the guard at a fixture. Verifying
    a destructive guard by disabling it only works if the guard's target is
    injectable — otherwise the test that proves the guard fires is itself the
    thing that does the damage when the guard is mutated away. That is not
    hypothetical: it rewrote all 343 files of this repo's ui_xml/ once.
    """
    repo_ui_xml = (protected or source_ui_xml_root()).resolve()
    resolved = root.resolve()
    if not force and (resolved == repo_ui_xml or repo_ui_xml in resolved.parents):
        raise SystemExit(
            f"refusing to minify the source tree in place: {resolved}\n"
            "This is meant for a staged copy. Pass --force only if you truly mean it."
        )

    files = before = after = 0
    for path in sorted(resolved.rglob("*.xml")):
        original = path.read_text(encoding="utf-8")
        minified = minify_xml(original)
        if minified == original:
            before += len(original.encode())
            after += len(original.encode())
            continue
        # Only rewrite when it is strictly smaller; a minifier that grew a file
        # would mean the transform did something unexpected to that file.
        if len(minified) > len(original):
            print(f"  !! skipped (grew): {path}", file=sys.stderr)
            before += len(original.encode())
            after += len(original.encode())
            continue
        path.write_text(minified, encoding="utf-8")
        files += 1
        before += len(original.encode())
        after += len(minified.encode())
    return files, before, after


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("directory", type=Path, help="staged directory to minify in place")
    ap.add_argument("--force", action="store_true", help="allow running on the source tree")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if not args.directory.is_dir():
        print(f"not a directory: {args.directory}", file=sys.stderr)
        return 1

    files, before, after = minify_tree(args.directory, args.force)
    if not args.quiet:
        saved = before - after
        pct = (saved / before * 100) if before else 0.0
        print(
            f"  Minified {files} XML file(s): {human(before)} -> {human(after)} "
            f"(saved {human(saved)}, {pct:.1f}%)"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
