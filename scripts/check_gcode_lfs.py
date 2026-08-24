#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: the gcode reader must be compiled with a 64-bit off_t.
#
# FileDataSource addresses gcode by uint64_t but seeks with fseeko/ftello, whose
# off_t is 32 bits on our 32-bit targets (pi32, ad5m, cc1, k1) unless large-file
# support is requested. Without it ftello() returns -1 on a file past 2 GB and
# read_range()'s cast to off_t truncates.
#
# The request cannot live in the source. mk/rules.mk compiles every app TU with
# $(PCH_FLAGS), which force-includes include/lvgl_pch.h ahead of the file, and
# that header pulls in <chrono>/<cstdint>/<mutex>/<string> - so glibc has latched
# _FILE_OFFSET_BITS before line 1 of any .cpp is read. A #define in the .cpp
# compiles clean and does absolutely nothing. It has to arrive on the command
# line, as a target-specific CXXFLAGS override on that one object.
#
# src/rendering/gcode_data_source.cpp carries a static_assert on sizeof(off_t)
# that fails the build if the override is dropped - but ONLY on a 32-bit target,
# and pi32/ad5m/cc1/k1 live in release.yml's matrix, not build.yml's. On x86_64
# PR CI off_t is already 8 bytes and the assertion passes trivially, so a dropped
# override is invisible until release. This gate closes that window.
#
# What is checked:
#
#   1. mk/rules.mk still carries the target-specific -D_FILE_OFFSET_BITS=64 for
#      the gcode_data_source object.
#   2. The source still carries the static_assert backstop.
#   3. The source does NOT define _FILE_OFFSET_BITS itself. That form is the
#      no-op described above; finding one means someone "simplified" the build
#      rule into the source and silently reverted the fix.
#
# Usage:
#   ./scripts/check_gcode_lfs.py
#   ./scripts/check_gcode_lfs.py --repo-root /path/to/checkout
#   ./scripts/check_gcode_lfs.py --rules mk/rules.mk --source src/rendering/gcode_data_source.cpp

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

RULES_REL = Path("mk/rules.mk")
SOURCE_REL = Path("src/rendering/gcode_data_source.cpp")

OBJECT_STEM = "rendering/gcode_data_source.o"
LFS_DEFINE = "-D_FILE_OFFSET_BITS=64"

# A make comment is not a build rule. Only a real target line counts.
OVERRIDE_RE = re.compile(
    r"^[^#\n]*" + re.escape(OBJECT_STEM) + r"\s*:.*" + re.escape(LFS_DEFINE)
)
STATIC_ASSERT_RE = re.compile(r"static_assert\s*\(\s*sizeof\s*\(\s*off_t\s*\)\s*==\s*8")
SOURCE_DEFINE_RE = re.compile(r"^\s*#\s*define\s+_FILE_OFFSET_BITS\b")


def check_rules(path: Path) -> list[str]:
    if not path.is_file():
        return [f"{path} not found - cannot confirm the gcode large-file override"]

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if OVERRIDE_RE.match(line):
            return []

    return [
        f"{path}: no target-specific `{LFS_DEFINE}` for {OBJECT_STEM}. Without it off_t "
        f"is 32-bit on pi32/ad5m/cc1/k1 and gcode past 2 GB seeks to the wrong place"
    ]


def check_source(path: Path) -> list[str]:
    if not path.is_file():
        return [f"{path} not found"]

    problems: list[str] = []
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()

    if not any(STATIC_ASSERT_RE.search(ln) for ln in lines):
        problems.append(
            f"{path}: the `static_assert(sizeof(off_t) == 8, ...)` backstop is gone - "
            f"a 32-bit build would silently go back to truncating past 2 GB"
        )

    for i, ln in enumerate(lines):
        if SOURCE_DEFINE_RE.match(ln):
            problems.append(
                f"{path}:{i + 1}: `#define _FILE_OFFSET_BITS` in the source does nothing here. "
                f"mk/rules.mk force-includes include/lvgl_pch.h ahead of this file, so glibc "
                f"has already latched the value. The define belongs on the compile line "
                f"(target-specific CXXFLAGS), not in the .cpp"
            )
            break

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description="Check the gcode reader's large-file gate.")
    parser.add_argument("--repo-root", default=".", help="repository root (default: cwd)")
    parser.add_argument("--rules", help=f"path to the makefile fragment (default: <root>/{RULES_REL})")
    parser.add_argument("--source", help=f"path to the gcode source (default: <root>/{SOURCE_REL})")
    args = parser.parse_args()

    root = Path(args.repo_root).resolve()
    rules = Path(args.rules) if args.rules else root / RULES_REL
    source = Path(args.source) if args.source else root / SOURCE_REL

    problems = check_rules(rules) + check_source(source)

    if problems:
        print()
        for problem in problems:
            print(f"❌ {problem}")
        print()
        print("   FileDataSource addresses gcode by uint64_t but seeks with fseeko/ftello.")
        print("   On a 32-bit target off_t is 32 bits without large-file support, so ftello")
        print("   returns -1 past 2 GB and the seek cast truncates. The static_assert only")
        print("   fires on 32-bit builds, which PR CI never runs - hence this gate.")
        print()
        print("   Fix: restore in mk/rules.mk")
        print(f"        $(OBJ_DIR)/{OBJECT_STEM}: CXXFLAGS += {LFS_DEFINE}")
        return 1

    print("✓ gcode reader builds with a 64-bit off_t")
    return 0


if __name__ == "__main__":
    sys.exit(main())
