#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lint gate: DisplayManager must cache the display resolution only AFTER the
backend has settled rotation.

The DRM backend may take over rotation itself (a scanout plane advertising
90/270) and clear LVGL's rotation, which un-swaps the resolution. A cache
read before set_display_rotation() therefore records a value the display no
longer has. DisplayManager::init() learned this the hard way
(prestonbrown/helixscreen#1275); apply_rotation() repeated the same order
(prestonbrown/helixscreen#1587), and the probe path does it correctly - this
gate pins all three, and any fourth call site the file grows.

Why a lint and not a unit test: apply_rotation's body lives behind
`#ifndef HELIX_DISPLAY_SDL`, and the test binary compiles with
HELIX_DISPLAY_SDL, so the path cannot run headless. The dormant path goes
live the moment a plane owns rotation on i915/amdgpu (x86 targets).
"""
#
# Usage:
#   ./scripts/check_rotation_cache_order.py
#   ./scripts/check_rotation_cache_order.py --file /path/to/display_manager.cpp

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

DEFAULT_FILE = "src/application/display_manager.cpp"

# A new function definition at column 0 naming DisplayManager (methods; the
# file's free helpers and namespace scope are not call sites of the members).
# Two spellings: the usual `void DisplayManager::name(`, and a long return
# type wrapped onto its own line leaving `DisplayManager::name(` at column 0.
FUNCTION_START = re.compile(r"^(?:[A-Za-z_][\w:<>,&*\s]*)?DisplayManager::\w+\s*\(")

# The cache write under protection.
CACHE_WRITE = re.compile(r"m_width\s*=\s*lv_display_get_horizontal_resolution")

# The call that must precede it within the same function. Comment lines are
# stripped before matching, so prose naming the call cannot satisfy the order
# (or defeat the no-settle exemption for an external-resize refresh).
BACKEND_SETTLE = re.compile(r"set_display_rotation\s*\(")


def strip_comment(line: str) -> str:
    idx = line.find("//")
    return line[:idx] if idx >= 0 else line


def check_source(text: str, source_name: str) -> list[str]:
    lines = text.splitlines()
    blocks: list[tuple[int, int]] = []  # (start_line_index, end_line_index_exclusive)
    current_start = None
    for i, line in enumerate(lines):
        if FUNCTION_START.match(line):
            if current_start is not None:
                blocks.append((current_start, i))
            current_start = i
    if current_start is not None:
        blocks.append((current_start, len(lines)))

    violations: list[str] = []
    for start, end in blocks:
        block = [strip_comment(l) for l in lines[start:end]]
        cache_idx = [i for i, l in enumerate(block) if CACHE_WRITE.search(l)]
        settle_idx = [i for i, l in enumerate(block) if BACKEND_SETTLE.search(l)]
        if not settle_idx:
            # A cache refresh with no rotation settle in the function is the
            # external-resize catch-up (resize_timer_cb: Android fold/unfold
            # changes the resolution out from under us). Nothing to order.
            continue
        for c in cache_idx:
            if not any(s < c for s in settle_idx):
                violations.append(
                    f"{source_name}:{start + c + 1}: resolution cached at line "
                    f"{start + c + 1} before set_display_rotation() in "
                    f"{lines[start].split('DisplayManager::')[-1].split('(')[0].strip()}()"
                )
    return violations


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--file", default=DEFAULT_FILE,
                    help="display_manager.cpp to check (default: %(default)s)")
    args = ap.parse_args()

    path = Path(args.file)
    if not path.is_file():
        print(f"{args.file}: not found", file=sys.stderr)
        return 2

    violations = check_source(path.read_text(), str(path))
    for v in violations:
        print(v)
    if violations:
        print("The backend may change LVGL's rotation (a plane owning 90/270 "
              "un-swaps the resolution); cache m_width/m_height only after "
              "set_display_rotation() returns.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
