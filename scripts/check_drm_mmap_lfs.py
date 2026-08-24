#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: LVGL's DRM driver must mmap dumb buffers with a 64-bit file offset.
#
# Background: DRM_IOCTL_MODE_MAP_DUMB returns a fake mmap offset allocated from
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
#   ./scripts/check_drm_mmap_lfs.py
#   ./scripts/check_drm_mmap_lfs.py --repo-root /path/to/checkout
#   ./scripts/check_drm_mmap_lfs.py --drm-source path/to/lv_linux_drm.c

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

PATCH_NAME = "lvgl-drm-mmap64.patch"
DRM_SOURCE_REL = Path("lib/lvgl/src/drivers/display/drm/lv_linux_drm.c")

INCLUDE_RE = re.compile(r"^\s*#\s*include\b")
DEFINE_RE = re.compile(r"^\s*#\s*define\s+_FILE_OFFSET_BITS\s+64\b")
STATIC_ASSERT_RE = re.compile(r"_Static_assert\s*\(\s*sizeof\s*\(\s*off_t\s*\)\s*==\s*8")


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

    if not any(STATIC_ASSERT_RE.search(ln) for ln in added):
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

    if not any(STATIC_ASSERT_RE.search(ln) for ln in lines):
        problems.append(
            f"{path}: the `_Static_assert(sizeof(off_t) == 8, ...)` guarding the dumb-buffer "
            f"mmap is gone - a 32-bit build can silently truncate the offset again"
        )

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description="Check the LVGL DRM mmap large-file gate.")
    parser.add_argument("--repo-root", default=".", help="repository root (default: cwd)")
    parser.add_argument("--drm-source", help=f"path to lv_linux_drm.c (default: <root>/{DRM_SOURCE_REL})")
    args = parser.parse_args()

    root = Path(args.repo_root).resolve()
    problems = check_patch(root)

    drm_source = Path(args.drm_source) if args.drm_source else root / DRM_SOURCE_REL
    if drm_source.is_file():
        problems += check_drm_source(drm_source)
    else:
        print(f"ℹ {drm_source} not present - skipping source checks (submodule not checked out)")

    if problems:
        print()
        for problem in problems:
            print(f"❌ {problem}")
        print()
        print("   DRM hands out dumb-buffer mmap offsets from DRM_FILE_PAGE_OFFSET_START")
        print("   (4 GiB), so every offset is above 2^32. A 32-bit off_t truncates it and the")
        print("   mapping fails - HelixScreen then falls back to fbdev and the KMS path is")
        print("   dead on that device with only an 'mmap fail' line in the log.")
        print()
        print(f"   Fix: restore patches/{PATCH_NAME} and its apply block in mk/patches.mk,")
        print("        then `make reapply-patches`.")
        return 1

    print("✓ DRM dumb-buffer mmap uses a 64-bit file offset")
    return 0


if __name__ == "__main__":
    sys.exit(main())
