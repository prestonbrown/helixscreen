#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Pack the ESP32 staging tree (scripts/esp32_stage_assets.py's output) into a
single frogfs (jkent/frogfs) compressed container image for the `storage`
partition, and gate on the real partition size.

Usage (call the venv's interpreter by path — do NOT `activate` it):
    python3 -m venv /tmp/esp32pack-venv
    /tmp/esp32pack-venv/bin/pip install pyyaml
    /tmp/esp32pack-venv/bin/python scripts/esp32_pack_assets.py

`activate` prepends the venv to PATH, which shadows the ESP-IDF python that
`export.sh` put there — mkfrogfs.py is then run by the wrong interpreter. CI
does it the by-path way for exactly this reason (see .github/workflows/
esp32-build.yml).

Requires `firmware/helixscreen-esp32/managed_components/jkent__frogfs/` to
already exist (fetched by ESP-IDF's component manager the first time
`idf.py reconfigure` or `idf.py build` runs against main/idf_component.yml,
which declares `jkent/frogfs`) — run that once first if it's missing.

Why this doesn't use frogfs's own CMake macros (target_add_frogfs /
declare_frogfs_bin): those create a build-tree Python venv and pip-install
into it from *inside* CMake's configure/build step. That conflicts with this
repo's build discipline of keeping asset generation as an explicit,
inspectable pre-step ahead of `idf.py build` (see esp32_stage_assets.py's
docstring for the same rationale re: littlefs_create_partition_image). This
script instead invokes managed_components/jkent__frogfs/tools/mkfrogfs.py
directly from our own venv — mkfrogfs.py is a self-contained script once its
`format`/`frogfs` sibling modules are importable (they are: running it
in-place makes Python add its own directory to sys.path automatically).

Size gate: unlike the old per-file LittleFS block-rounding budget, a packed
frogfs image has no per-file flash-block tax — each entry costs its own
(compressed) byte count plus a small fixed header (~8-20 bytes) padded to a
4-byte boundary. The gate here is simply the packed image's total byte size
against the `storage` partition (0x2c0000 = 2,883,584 bytes, partitions.csv),
minus a small safety margin for headroom (future growth without touching the
partition table, and any esptool_py write-size rounding).
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
FIRMWARE_DIR = REPO_ROOT / "firmware" / "helixscreen-esp32"
DEFAULT_STAGING_DIR = FIRMWARE_DIR / "build" / "littlefs_staging"
DEFAULT_CONFIG = FIRMWARE_DIR / "frogfs.yaml"
DEFAULT_CACHE_DIR = FIRMWARE_DIR / "build" / "frogfs_cache"
# In build/ even though it is a build INPUT (main/CMakeLists.txt FATAL_ERRORs
# when it is missing), and so an `idf.py fullclean` deletes it. That is
# deliberate, not an oversight: its own input — build/littlefs_staging, written
# by esp32_stage_assets.py — lives in build/ too, so a fullclean invalidates
# both halves together and re-staging is required regardless. The missing case
# is loud, not silent: CMake stops at configure time naming both re-run
# commands. Moving the image outside build/ would need main/CMakeLists.txt's
# STORAGE_IMAGE to follow, and would leave a multi-MB untracked artifact in the
# firmware source dir.
DEFAULT_OUTPUT = FIRMWARE_DIR / "build" / "storage_frogfs.bin"
MKFROGFS = FIRMWARE_DIR / "managed_components" / "jkent__frogfs" / "tools" / "mkfrogfs.py"

# `storage` partition size (partitions.csv: storage, data, spiffs, 0xd20000, 0x2c0000).
# 0x2c0000 = 2,883,584. (The Stage B enabler brief cited 2,949,120 = 0x2d0000
# for this constant, but that exceeds the actual partition by 64KB and would let
# the packer green-light a container past the partition end; the partition table
# is geometry-locked to 0x2c0000 to end the flash exactly at 0x1000000.)
STORAGE_PARTITION_BYTES = 2_883_584

# Small fixed safety margin: frogfs itself has no per-file block tax, but we
# keep headroom for incremental content growth between spec revisions and
# any esptool_py write-granularity rounding. 64KB, same magnitude as the old
# LittleFS metadata reserve this replaces.
SAFETY_MARGIN_BYTES = 64 * 1024
EFFECTIVE_BUDGET_BYTES = STORAGE_PARTITION_BYTES - SAFETY_MARGIN_BYTES


def format_bytes(n: int) -> str:
    return f"{n:,} B ({n / 1024:.1f} KiB)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--staging-dir", type=Path, default=DEFAULT_STAGING_DIR)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    if not args.staging_dir.is_dir():
        print(f"FAIL: staging dir missing: {args.staging_dir}\n"
              "Run 'python3 scripts/esp32_stage_assets.py' first.", file=sys.stderr)
        return 1

    if not MKFROGFS.is_file():
        print(f"FAIL: {MKFROGFS} not found.\n"
              "Run 'idf.py reconfigure' (or 'idf.py build') in "
              f"{FIRMWARE_DIR} first so the component manager fetches "
              "jkent/frogfs (declared in main/idf_component.yml) into "
              "managed_components/.", file=sys.stderr)
        return 1

    try:
        import yaml  # noqa: F401
    except ImportError:
        print("FAIL: pyyaml not installed. Use a venv, called BY PATH — do not\n"
              "'activate' it, that shadows the ESP-IDF python:\n"
              "  python3 -m venv /tmp/esp32pack-venv\n"
              "  /tmp/esp32pack-venv/bin/pip install pyyaml\n"
              "  /tmp/esp32pack-venv/bin/python scripts/esp32_pack_assets.py",
              file=sys.stderr)
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    DEFAULT_CACHE_DIR.mkdir(parents=True, exist_ok=True)

    env = dict(os.environ)
    env["FROGFS_STAGING_DIR"] = str(args.staging_dir.resolve())

    cmd = [sys.executable, str(MKFROGFS), "-C", str(REPO_ROOT),
           str(args.config), str(DEFAULT_CACHE_DIR), str(args.out)]
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, env=env)
    if result.returncode != 0:
        print(f"FAIL: mkfrogfs.py exited {result.returncode}", file=sys.stderr)
        return 1

    packed_size = args.out.stat().st_size
    pct = 100.0 * packed_size / STORAGE_PARTITION_BYTES
    headroom = STORAGE_PARTITION_BYTES - packed_size

    print()
    print("ESP32 packed asset container:")
    print(f"  output: {args.out}")
    print(f"  packed size: {format_bytes(packed_size)} ({pct:.1f}% of partition)")
    print(f"  storage partition: {format_bytes(STORAGE_PARTITION_BYTES)}")
    print(f"  headroom: {format_bytes(headroom)}")
    print(f"  effective budget (partition minus {format_bytes(SAFETY_MARGIN_BYTES)} safety "
          f"margin): {format_bytes(EFFECTIVE_BUDGET_BYTES)}")

    if packed_size > EFFECTIVE_BUDGET_BYTES:
        print(f"FAIL: packed size {packed_size} exceeds effective budget "
              f"{EFFECTIVE_BUDGET_BYTES} ({format_bytes(SAFETY_MARGIN_BYTES)} safety margin "
              f"reserved out of the {format_bytes(STORAGE_PARTITION_BYTES)} partition)",
              file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
