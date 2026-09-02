#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: the display backends' stored-touch-range gate must ask the DISPLAY
# for its rotation, never the `/display/rotate` config key.
#
# A touch range solved on a rotated panel folds the rotation into (min,max,swap)
# and double-applies it at runtime (prestonbrown/helixscreen#1394). Both display
# backends therefore refuse a stored range when the panel is rotated, and the
# calibration panel refuses to solve a new one. All three must answer the same
# question from the same source.
#
# The config key is only the REQUEST. It differs from the applied rotation in
# both directions:
#
#   * CLI `--rotate` or HELIX_DISPLAY_ROTATION with no `/display/rotate` key:
#     the display is rotated, the key reads 0, and a backend trusting the key
#     programs the stored range from the broken window - #1394 stays live.
#   * A DRM->fbdev rotation fallback that fails: display_manager.cpp logs
#     "Continuing without rotation" and never calls lv_display_set_rotation(),
#     so the display is unrotated while the key still reads 90/270. A backend
#     trusting the key discards a legitimate stored range on every boot.
#
# Why a lint and not a unit test: the gates live in create_input_pointer(),
# which needs a real fbdev or DRM device and cannot run headless. Mutation
# testing confirmed the gap - reverting either backend to the config-key read
# kills no test. This gate is what makes that revert fail.
#
# What is checked, per backend source:
#
#   1. It calls display_rotation_degrees() or display_is_rotated().
#   2. It performs no Config read of "/display/rotate". Prose mentions of the
#      key in comments are fine and expected - the check targets the read form
#      (`get<...>("/display/rotate"`), not the string.
#
# Usage:
#   ./scripts/check_touch_rotation_source.py
#   ./scripts/check_touch_rotation_source.py --repo-root /path/to/checkout

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The backends whose stored-range gate must read the display.
GUARDED_SOURCES = (
    "src/api/display_backend_fbdev.cpp",
    "src/api/display_backend_drm.cpp",
)

# Either helper answers the question; both are defined in include/display_backend.h.
REQUIRED_HELPERS = ("display_rotation_degrees", "display_is_rotated")

# A Config read of the rotation key, in any of the spellings this tree uses.
# Deliberately matches the READ, not the bare string, so comments naming the
# key stay legal.
FORBIDDEN_READ = re.compile(r"""get\s*<[^>]*>\s*\(\s*["']/display/rotate["']""")


def check_source(root: Path, rel: str) -> list[str]:
    path = root / rel
    if not path.is_file():
        return [f"{rel}: missing - update GUARDED_SOURCES if the backend was renamed"]

    text = path.read_text(encoding="utf-8")
    failures: list[str] = []

    if not any(helper in text for helper in REQUIRED_HELPERS):
        failures.append(
            f"{rel}: no call to {' or '.join(REQUIRED_HELPERS)}.\n"
            f"      The stored-range gate must ask the display for its rotation."
        )

    for lineno, line in enumerate(text.splitlines(), start=1):
        if FORBIDDEN_READ.search(line):
            failures.append(
                f"{rel}:{lineno}: reads /display/rotate.\n"
                f"      {line.strip()}\n"
                f"      That key is the requested rotation, not the applied one. Use\n"
                f"      display_rotation_degrees() so this gate and the calibration\n"
                f"      panel's gate cannot disagree (prestonbrown/helixscreen#1394)."
            )

    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root (default: parent of scripts/)",
    )
    args = parser.parse_args()

    failures: list[str] = []
    for rel in GUARDED_SOURCES:
        failures.extend(check_source(args.repo_root, rel))

    if failures:
        print("FAIL: the touch-range rotation gate must read the display, not the config key.\n")
        for failure in failures:
            print(f"  {failure}")
        print()
        return 1

    print(f"OK: {len(GUARDED_SOURCES)} display backend(s) gate the stored touch range on the "
          f"applied display rotation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
