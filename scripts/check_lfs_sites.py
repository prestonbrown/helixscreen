#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: every site that needs a 64-bit off_t on a 32-bit target still gets one.
#
# On our 32-bit targets (pi32, ad5m, cc1, k1) off_t is 32 bits unless the translation
# unit asks for large-file support, and the request only takes effect if it arrives
# before glibc's features.h is first included. Each site below carries the request in
# the one place that works for it, plus a sizeof(off_t) assertion that fails only on
# a 32-bit build, which PR CI never runs. This gate checks the shape of each fix.
#
# --- gcode: the gcode reader (`--only gcode`) ---------------------------------
#
# FileDataSource addresses gcode by uint64_t but seeks with fseeko/ftello, whose
# off_t is 32 bits on our 32-bit targets (pi32, ad5m, cc1, k1) unless large-file
# support is requested. Without it ftello() returns -1 on a file past 2 GB and
# read_range()'s cast to off_t truncates.
#
# The request cannot live in the source. mk/rules.mk compiles every app TU with
# $(FORCED_INCLUDE), which force-includes include/lvgl_pch.h ahead of the file, and
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
# --- drm: LVGL's DRM dumb-buffer mmap (`--only drm`) --------------------------
#
# DRM_IOCTL_MODE_MAP_DUMB returns a fake mmap offset allocated from
# DRM_FILE_PAGE_OFFSET_START, which is 4 GiB, so every offset it hands back is
# above 2^32. mmap() takes an off_t, and on 32-bit userspace off_t is 32 bits
# wide unless the translation unit asks for large-file support - the offset is
# truncated at the call site and the mapping fails with EINVAL. Measured on the
# pi32 toolchain: without the define the object references `mmap` and off_t is
# 4 bytes; with it the object references `mmap64` and off_t is 8 bytes.
#
# The symptom is silent. lv_linux_drm.c logs "mmap fail", lv_linux_drm_set_file
# returns an error, and HelixScreen falls back to fbdev - so a 32-bit device
# runs the slower backend forever and nobody notices the KMS path never worked.
# That is why this needs a gate and not a bug report.
#
# What is checked:
#
#   1. patches/lvgl-drm-mmap64.patch exists, mk/patches.mk applies it, and the
#      patch still adds both `#define _FILE_OFFSET_BITS 64` and the sizeof(off_t)
#      static assertion. A dropped, unwired, or thinned-out patch means the fix
#      is gone on the next `make reapply-patches`.
#
#   2. If lib/lvgl is checked out AND already patched, the define must sit above
#      the first #include in lv_linux_drm.c. features.h latches the value on
#      first inclusion, so a define that drifts below an include still compiles
#      and does nothing - the exact way this regresses on an LVGL version bump.
#
# A pristine (unpatched) submodule is a legitimate state - CI's quality job runs
# before anything applies patches - so check 2 reports and skips there.
#
# Usage:
#   ./scripts/check_lfs_sites.py                    # every site
#   ./scripts/check_lfs_sites.py --repo-root /path/to/checkout
#   ./scripts/check_lfs_sites.py --only gcode --rules mk/rules.mk --source src/rendering/gcode_data_source.cpp
#   ./scripts/check_lfs_sites.py --only drm --drm-source path/to/lv_linux_drm.c

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# --- gcode ---------------------------------------------------------------------

RULES_REL = Path("mk/rules.mk")
SOURCE_REL = Path("src/rendering/gcode_data_source.cpp")

OBJECT_STEM = "rendering/gcode_data_source.o"
LFS_DEFINE = "-D_FILE_OFFSET_BITS=64"

# A make comment is not a build rule. Only a real target line counts.
OVERRIDE_RE = re.compile(
    r"^[^#\n]*" + re.escape(OBJECT_STEM) + r"\s*:.*" + re.escape(LFS_DEFINE)
)
GCODE_STATIC_ASSERT_RE = re.compile(r"static_assert\s*\(\s*sizeof\s*\(\s*off_t\s*\)\s*==\s*8")
SOURCE_DEFINE_RE = re.compile(r"^\s*#\s*define\s+_FILE_OFFSET_BITS\b")


def check_gcode_rules(path: Path) -> list[str]:
    if not path.is_file():
        return [f"{path} not found - cannot confirm the gcode large-file override"]

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if OVERRIDE_RE.match(line):
            return []

    return [
        f"{path}: no target-specific `{LFS_DEFINE}` for {OBJECT_STEM}. Without it off_t "
        f"is 32-bit on pi32/ad5m/cc1/k1 and gcode past 2 GB seeks to the wrong place"
    ]


def check_gcode_source(path: Path) -> list[str]:
    if not path.is_file():
        return [f"{path} not found"]

    problems: list[str] = []
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()

    if not any(GCODE_STATIC_ASSERT_RE.search(ln) for ln in lines):
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


# --- drm -----------------------------------------------------------------------

PATCH_NAME = "lvgl-drm-mmap64.patch"
DRM_SOURCE_REL = Path("lib/lvgl/src/drivers/display/drm/lv_linux_drm.c")

INCLUDE_RE = re.compile(r"^\s*#\s*include\b")
DEFINE_RE = re.compile(r"^\s*#\s*define\s+_FILE_OFFSET_BITS\s+64\b")
DRM_STATIC_ASSERT_RE = re.compile(r"_Static_assert\s*\(\s*sizeof\s*\(\s*off_t\s*\)\s*==\s*8")


def check_patch(root: Path) -> list[str]:
    """The patch must exist, be wired into the build, and still carry the fix."""
    problems: list[str] = []

    patch = root / "patches" / PATCH_NAME
    if not patch.is_file():
        return [f"patches/{PATCH_NAME} is missing - the DRM 64-bit mmap fix has been dropped"]

    patches_mk = root / "mk" / "patches.mk"
    if not patches_mk.is_file():
        problems.append("mk/patches.mk not found")
    elif PATCH_NAME not in patches_mk.read_text(encoding="utf-8", errors="replace"):
        problems.append(
            f"mk/patches.mk has no apply block for {PATCH_NAME} - it would never be applied"
        )

    # Only the lines the patch ADDS count. A define that merely appears as
    # context is one that some other patch owns.
    added = [ln[1:] for ln in patch.read_text(encoding="utf-8", errors="replace").splitlines()
             if ln.startswith("+") and not ln.startswith("+++")]

    if not any(DEFINE_RE.match(ln) for ln in added):
        problems.append(
            f"patches/{PATCH_NAME} no longer adds `#define _FILE_OFFSET_BITS 64` - "
            f"mmap() will truncate the DRM dumb-buffer offset on 32-bit targets"
        )

    if not any(DRM_STATIC_ASSERT_RE.search(ln) for ln in added):
        problems.append(
            f"patches/{PATCH_NAME} no longer adds the `_Static_assert(sizeof(off_t) == 8, ...)` "
            f"backstop - a toolchain where the define does not take would fail silently"
        )

    return problems


def check_drm_source(path: Path) -> list[str]:
    """In an already-patched tree, the define must precede every #include."""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()

    define_at = next((i for i, ln in enumerate(lines) if DEFINE_RE.match(ln)), None)
    if define_at is None:
        print(f"ℹ {path} is unpatched - skipping source ordering check")
        return []

    problems: list[str] = []

    first_include = next((i for i, ln in enumerate(lines) if INCLUDE_RE.match(ln)), None)
    if first_include is not None and define_at > first_include:
        problems.append(
            f"{path}:{define_at + 1}: `#define _FILE_OFFSET_BITS 64` sits below the first "
            f"#include (line {first_include + 1}). features.h has already latched the value, "
            f"so the define compiles but does nothing"
        )

    if not any(DRM_STATIC_ASSERT_RE.search(ln) for ln in lines):
        problems.append(
            f"{path}: the `_Static_assert(sizeof(off_t) == 8, ...)` guarding the dumb-buffer "
            f"mmap is gone - a 32-bit build can silently truncate the offset again"
        )

    return problems


def gcode_problems(args, root: Path) -> list[str]:
    rules = Path(args.rules) if args.rules else root / RULES_REL
    source = Path(args.source) if args.source else root / SOURCE_REL
    return check_gcode_rules(rules) + check_gcode_source(source)


def drm_problems(args, root: Path) -> list[str]:
    problems = check_patch(root)
    drm_source = Path(args.drm_source) if args.drm_source else root / DRM_SOURCE_REL
    if drm_source.is_file():
        problems += check_drm_source(drm_source)
    else:
        print(f"ℹ {drm_source} not present - skipping source checks (submodule not checked out)")
    return problems


GCODE_WHY = (
    "   FileDataSource addresses gcode by uint64_t but seeks with fseeko/ftello.",
    "   On a 32-bit target off_t is 32 bits without large-file support, so ftello",
    "   returns -1 past 2 GB and the seek cast truncates. The static_assert only",
    "   fires on 32-bit builds, which PR CI never runs - hence this gate.",
    "",
    "   Fix: restore in mk/rules.mk",
    f"        $(OBJ_DIR)/{OBJECT_STEM}: CXXFLAGS += {LFS_DEFINE}",
)
DRM_WHY = (
    "   DRM hands out dumb-buffer mmap offsets from DRM_FILE_PAGE_OFFSET_START",
    "   (4 GiB), so every offset is above 2^32. A 32-bit off_t truncates it and the",
    "   mapping fails - HelixScreen then falls back to fbdev and the KMS path is",
    "   dead on that device with only an 'mmap fail' line in the log.",
    "",
    f"   Fix: restore patches/{PATCH_NAME} and its apply block in mk/patches.mk,",
    "        then `make reapply-patches`.",
)

# (name, problems, why, success line)
SITES = (
    ("gcode", gcode_problems, GCODE_WHY, "✓ gcode reader builds with a 64-bit off_t"),
    ("drm", drm_problems, DRM_WHY, "✓ DRM dumb-buffer mmap uses a 64-bit file offset"),
)


def main() -> int:
    parser = argparse.ArgumentParser(description="Check every large-file (64-bit off_t) site.")
    parser.add_argument("--repo-root", default=".", help="repository root (default: cwd)")
    parser.add_argument("--only", choices=[name for name, *_ in SITES],
                        help="check one site (default: every site)")
    parser.add_argument("--rules", help=f"gcode: the makefile fragment (default: <root>/{RULES_REL})")
    parser.add_argument("--source", help=f"gcode: the reader's source (default: <root>/{SOURCE_REL})")
    parser.add_argument("--drm-source",
                        help=f"drm: lv_linux_drm.c (default: <root>/{DRM_SOURCE_REL})")
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()

    failed = False
    for name, problems_of, why, ok_line in SITES:
        if args.only and name != args.only:
            continue
        problems = problems_of(args, root)
        if not problems:
            print(ok_line)
            continue
        failed = True
        print()
        for problem in problems:
            print(f"❌ {problem}")
        print()
        print("\n".join(why))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
