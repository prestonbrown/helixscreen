#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail when printer_database.json names a printer image that does not exist.

A missing image is silent at runtime: get_prerendered_printer_path() falls through to
generic-corexy, so the user just sees a CoreXY frame for their bed-slinger and nothing
is logged above debug. Twenty entries had drifted that way before anyone noticed.

KNOWN_MISSING below is a ratchet, not a permanent exemption. Entries in it still need
art; the gate's job is to stop NEW ones appearing and to notice when an old one is
finally satisfied so the list shrinks.

  ./scripts/check_printer_images.py           # check
  ./scripts/check_printer_images.py --list    # just print what is unresolved
"""
import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DB = REPO_ROOT / "assets" / "config" / "printer_database.json"
IMG_DIR = REPO_ROOT / "assets" / "images" / "printers"

# Empty, and worth keeping that way. Every database entry now names an image that
# exists: art was drawn for the Artillery Genius Pro, Kingroon KLP1 and Sovol SV07,
# the Sidewinder X2 shares the Genius Pro frame, the five community-config entries
# (KAMP, Klippain Shake&Tune, ERCF/Happy Hare, Klicky, Ellis) use generic-corexy,
# and the Bambu and two unillustrated Kingroon entries were dropped. A new entry
# without art belongs here WITH a reason, not silently falling through to the
# generic frame - which is what hid the previous twenty.
KNOWN_MISSING: set[str] = set()


def unresolved():
    """[(printer_id, image)] for every entry whose image is not on disk."""
    db = json.loads(DB.read_text(encoding="utf-8"))
    out = []
    for entry in db.get("printers", []):
        image = entry.get("image", "")
        if not image:
            continue
        if not (IMG_DIR / image).exists():
            out.append((entry.get("id", "<no id>"), image))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="print unresolved refs and exit 0")
    args = ap.parse_args()

    missing = unresolved()
    missing_images = {img for _, img in missing}

    if args.list:
        for pid, img in sorted(missing, key=lambda r: r[1]):
            mark = " (known)" if img in KNOWN_MISSING else " (NEW)"
            print(f"{img:<30} {pid}{mark}")
        return 0

    new = sorted(missing_images - KNOWN_MISSING)
    # A known-missing image that now exists means someone added the art; drop it from
    # the list so the ratchet only ever tightens.
    satisfied = sorted(img for img in KNOWN_MISSING if (IMG_DIR / img).exists())

    if new:
        print(f"❌ printer_database.json references {len(new)} image(s) that do not exist:")
        for img in new:
            ids = ", ".join(pid for pid, i in missing if i == img)
            print(f"     {img}  <- {ids}")
        print("   These render as generic-corexy with nothing logged above debug.")
        print(f"   Add the art to assets/images/printers/, point the entry at an existing")
        print(f"   image, or add it to KNOWN_MISSING in {Path(__file__).name} with a reason.")
        return 1

    if satisfied:
        print(f"❌ {len(satisfied)} entr(ies) in KNOWN_MISSING now have artwork:")
        for img in satisfied:
            print(f"     {img}")
        print(f"   Remove them from KNOWN_MISSING in {Path(__file__).name} — the list is a")
        print("   ratchet and must shrink when art lands.")
        return 1

    covered = len(missing_images)
    print(f"✅ Printer images: every database reference resolves "
          f"({covered} known-missing awaiting artwork)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
