#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Fail the ESP32 firmware build if the packed storage asset image
(scripts/esp32_pack_assets.py's output) is older than any file under the
source trees that feed it (ui_xml/, assets/, the printer-image rendition
build dir, or the littlefs staging tree itself).

Invoked from firmware/helixscreen-esp32/main/CMakeLists.txt as a custom
target with no OUTPUT, so it re-runs on every `idf.py build` — not just on
CMake reconfigure. The `if(NOT EXISTS ...)` guard next to it in CMakeLists.txt
only fires at configure time and only catches a MISSING image; this script
is the build-time counterpart that catches a STALE one (image exists but a
source file was edited after the last pack).

Usage:
    esp32_check_asset_staleness.py <packed_image> <source_path>...

Exits 1 (and prints re-run instructions) if any regular file under any
<source_path> has an mtime newer than <packed_image>, or if <packed_image>
is missing. Exits 0 otherwise. Non-existent source paths are skipped
(e.g. build/esp32_printer_images/ before the printer-image script has ever
been run) since stage_printer_images() itself treats that as "nothing to
stage yet", not an error.

Caveat: this is a plain mtime comparison, same granularity limits as any
timestamp-based build system (a source edit and a re-pack within the same
filesystem-timestamp tick can't be told apart) -- it will not produce false
staleness, but a source edit racing a pack in the same instant could in
theory go undetected.
"""

import sys
from pathlib import Path


def newest_mtime(path: Path):
    """(mtime, file) of the most recently modified regular file under path,
    or None if path doesn't exist or contains no regular files."""
    if not path.exists():
        return None
    if path.is_file():
        return path.stat().st_mtime, path
    best = None
    for f in path.rglob("*"):
        if f.is_file():
            mtime = f.stat().st_mtime
            if best is None or mtime > best[0]:
                best = (mtime, f)
    return best


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: esp32_check_asset_staleness.py <packed_image> <source_path>...",
              file=sys.stderr)
        return 2

    image = Path(sys.argv[1])
    sources = [Path(p) for p in sys.argv[2:]]

    rerun_instructions = (
        "Re-run:\n"
        "  python3 scripts/esp32_stage_assets.py\n"
        "  python3 scripts/esp32_pack_assets.py\n"
        "then re-run idf.py build."
    )

    if not image.is_file():
        print(f"FAIL: packed asset image missing: {image}\n\n{rerun_instructions}",
              file=sys.stderr)
        return 1

    image_mtime = image.stat().st_mtime
    stale = []
    for src in sources:
        result = newest_mtime(src)
        if result is None:
            continue
        mtime, f = result
        if mtime > image_mtime:
            stale.append((src, f, mtime))

    if stale:
        print("FAIL: packed asset image is STALE -- a source input changed after "
              "the last pack:", file=sys.stderr)
        print(f"  {image} (packed at {image_mtime:.0f})", file=sys.stderr)
        for src, f, mtime in stale:
            print(f"  newer than image: {f} ({mtime:.0f}, under {src})", file=sys.stderr)
        print(file=sys.stderr)
        print(rerun_instructions, file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
