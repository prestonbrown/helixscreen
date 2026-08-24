#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Stage the ESP32 "storage" partition source tree (packed asset container).

Assembles a minified copy of ui_xml/ (all languages), the config JSON assets,
and (if present) Task 3's printer image renditions into a staging directory,
then prints a per-class raw-byte size table.

Usage:
    python3 scripts/esp32_stage_assets.py [--out DIR]

This script only assembles the staging tree — it does not gate on partition
size and does not pack or flash anything. `scripts/esp32_pack_assets.py`
consumes this staging tree, deflates the text assets into a single packed
frogfs container image, and is where the real size gate lives (packed image
size vs the 3.75MB `storage` partition — see that script's docstring).

Historical note: this staging tree used to be mounted directly as a LittleFS
image, which allocates a whole 4KB erase block per file regardless of size —
308 small ui_xml component files cost 1.76MB on-flash for 1.04MB of text (69%
overhead), leaving no room for all 9 languages or any printer images. The
packed container (Task 3) eliminates that per-file block tax and adds ~3-4:1
deflate on text, which is why this script no longer trims languages or
gates on a block-rounded budget: every raw byte counted here ends up smaller,
not larger, once packed.
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = REPO_ROOT / "firmware" / "helixscreen-esp32" / "build" / "littlefs_staging"

# Firmware-local ui_xml overrides copied OVER the shared staged tree. Lets the
# ESP32 build ship a variant of a shared component (e.g. a home-only app_layout
# that defers the other five panels' instantiation) without forking the shared
# ui_xml/ or touching the desktop build. Each file's path mirrors its location
# under ui_xml/ (so ui_xml_overrides/app_layout.xml replaces ui_xml/app_layout.xml).
UI_XML_OVERRIDES_DIR = REPO_ROOT / "firmware" / "helixscreen-esp32" / "ui_xml_overrides"

# ui_xml subtrees/files excluded from staging.
EXCLUDED_XML_DIRS = ("micro",)
EXCLUDED_XML_FILES = ("translations.xml",)  # merged file; per-language files ship instead


def strip_xml_comments(text: str) -> str:
    """Remove <!-- ... --> comments (including multiline)."""
    return re.sub(r"<!--.*?-->", "", text, flags=re.DOTALL)


def _tokenize_xml(text: str) -> list[tuple[str, str]]:
    """Split text into ("tag", str) / ("text", str) tokens.

    A "tag" token spans exactly from a real '<' to its true matching '>' —
    attribute-value quoting, CDATA sections, and comments are all tracked so a
    literal '>' inside any of them can't be mistaken for the tag's close.
    Everything else is a verbatim "text" token. This makes the tag/text split
    boundary-aware in a way a blind '>...<' regex can't be: e.g. a literal '>'
    in text content (legal, unescaped XML) followed by whitespace and then the
    next tag's '<' must NOT be read as "whitespace between two tags".
    """
    tokens: list[tuple[str, str]] = []
    i = 0
    n = len(text)
    text_start = 0

    while i < n:
        if text[i] != "<":
            i += 1
            continue

        if i > text_start:
            tokens.append(("text", text[text_start:i]))

        tag_start = i
        if text.startswith("<![CDATA[", i):
            end = text.find("]]>", i + 9)
            end = end + 3 if end != -1 else n
        elif text.startswith("<!--", i):
            end = text.find("-->", i + 4)
            end = end + 3 if end != -1 else n
        else:
            j = i + 1
            quote = None
            while j < n:
                c = text[j]
                if quote:
                    if c == quote:
                        quote = None
                elif c in ("'", '"'):
                    quote = c
                elif c == ">":
                    j += 1
                    break
                j += 1
            end = j
        tokens.append(("tag", text[tag_start:end]))
        i = end
        text_start = i

    if text_start < n:
        tokens.append(("text", text[text_start:n]))

    return tokens


def collapse_inter_tag_whitespace(text: str) -> str:
    """Remove whitespace-only text nodes that sit directly between two tags.

    Tag-boundary aware (see `_tokenize_xml`): a text token is dropped only if
    it is both (a) entirely whitespace and (b) immediately flanked by real tag
    tokens on both sides. Real text content — including a text node that's
    non-whitespace only because it contains a bare '>' — is never touched, and
    neither are attribute values, CDATA sections, or comments.
    """
    tokens = _tokenize_xml(text)
    out = []
    for idx, (kind, content) in enumerate(tokens):
        if kind == "text" and content.strip() == "":
            prev_is_tag = idx > 0 and tokens[idx - 1][0] == "tag"
            next_is_tag = idx < len(tokens) - 1 and tokens[idx + 1][0] == "tag"
            if prev_is_tag and next_is_tag:
                continue
        out.append(content)
    return "".join(out)


def minify_xml(text: str) -> str:
    """Conservative XML minifier: strip comments, collapse inter-tag whitespace.

    Byte-preserves text content and attribute values. When a transform can't
    prove it's formatting-only whitespace, it doesn't touch the bytes.
    """
    return collapse_inter_tag_whitespace(strip_xml_comments(text))


def format_bytes(n: int) -> str:
    return f"{n:,} B ({n / 1024:.1f} KiB)"


def stage_ui_xml(ui_xml_dir: Path, out_dir: Path) -> int:
    """Minify and copy ui_xml/, excluding micro/ and the merged translations.xml.
    Returns total raw bytes written (translations/ handled separately by
    stage_translations, not counted here)."""
    total = 0
    for src in sorted(ui_xml_dir.rglob("*.xml")):
        rel = src.relative_to(ui_xml_dir)
        if rel.parts and rel.parts[0] in EXCLUDED_XML_DIRS:
            continue
        if rel.parts and rel.parts[0] == "translations":
            continue  # handled by stage_translations
        if src.name in EXCLUDED_XML_FILES:
            continue
        text = src.read_text(encoding="utf-8")
        minified = minify_xml(text)
        dest = out_dir / "ui_xml" / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(minified, encoding="utf-8")
        total += len(minified.encode("utf-8"))
    return total


def stage_ui_xml_overrides(overrides_dir: Path, out_dir: Path) -> int:
    """Overwrite staged ui_xml files with firmware-local overrides (minified).
    Runs AFTER stage_ui_xml so the override wins. Returns the net byte delta
    (override size minus the shared file it replaced) so the staging summary
    stays accurate."""
    if not overrides_dir.is_dir():
        return 0
    delta = 0
    for src in sorted(overrides_dir.rglob("*.xml")):
        rel = src.relative_to(overrides_dir)
        dest = out_dir / "ui_xml" / rel
        prev = dest.stat().st_size if dest.exists() else 0
        minified = minify_xml(src.read_text(encoding="utf-8"))
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(minified, encoding="utf-8")
        new = len(minified.encode("utf-8"))
        print(f"  ui_xml override: {rel} ({format_bytes(new)}, replaces {format_bytes(prev)})")
        delta += new - prev
    return delta


def stage_translations(ui_xml_dir: Path, out_dir: Path,
                       remaining_budget: int = sys.maxsize) -> tuple[int, list[str]]:
    """Minify and stage every per-language translation file. `en` is a hard
    requirement (fails the build if missing or, given an explicit
    remaining_budget, if it doesn't fit); every other language ships
    unconditionally — the packed container (see module docstring) has ample
    room for all 9 languages, so there is no per-language trimming here.
    remaining_budget only exists for the en-fits invariant; callers packing
    into a real container don't need to (and by default don't) constrain it.
    Returns (raw bytes written, included language codes)."""
    translations_dir = ui_xml_dir / "translations"
    if not translations_dir.is_dir():
        return 0, []

    candidates = {}
    for src in sorted(translations_dir.glob("*.xml")):
        if src.name in EXCLUDED_XML_FILES:
            continue
        text = src.read_text(encoding="utf-8")
        minified = minify_xml(text)
        candidates[src.stem] = minified

    included: list[str] = []
    total = 0

    def add(lang: str) -> bool:
        nonlocal total
        minified = candidates[lang]
        size = len(minified.encode("utf-8"))
        if total + size > remaining_budget:
            return False
        dest = out_dir / "ui_xml" / "translations" / f"{lang}.xml"
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(minified, encoding="utf-8")
        total += size
        included.append(lang)
        return True

    if "en" not in candidates:
        print("FAIL: no en.xml found in ui_xml/translations/ — en must always ship.",
              file=sys.stderr)
        sys.exit(1)
    if not add("en"):
        print(f"FAIL: en.xml ({len(candidates['en'].encode('utf-8'))} B minified) does not fit "
              f"in the remaining budget ({remaining_budget} B) — en must always ship.",
              file=sys.stderr)
        sys.exit(1)

    for lang in sorted(lang for lang in candidates if lang != "en"):
        add(lang)

    return total, included


def stage_config(assets_dir: Path, out_dir: Path) -> int:
    """Copy assets/config/{printer_database.json, printing_tips.json, themes/}.
    Returns total raw bytes."""
    total = 0
    config_dir = assets_dir / "config"
    dest_config = out_dir / "assets" / "config"

    for name in ("printer_database.json", "printing_tips.json"):
        src = config_dir / name
        if not src.is_file():
            continue
        dest_config.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest_config / name)
        total += src.stat().st_size

    themes_src = config_dir / "themes"
    if themes_src.is_dir():
        themes_dest = dest_config / "themes"
        shutil.copytree(themes_src, themes_dest, dirs_exist_ok=True)
        total += sum(f.stat().st_size for f in themes_dest.rglob("*") if f.is_file())

    return total


def stage_filaments(assets_dir: Path, out_dir: Path) -> int:
    src = assets_dir / "filaments.json"
    if not src.is_file():
        return 0
    dest = out_dir / "assets" / "filaments.json"
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)
    return src.stat().st_size


def stage_printer_images(repo_root: Path, out_dir: Path) -> tuple[int, bool]:
    """Copy build/esp32_printer_images/ (Task 3's downscaled renditions) if
    present, to assets/images/printers/ — the path PrinterImageWidget's
    fallback resolution expects relative to the asset root (see
    src/system/prerendered_images.cpp get_prerendered_printer_path /
    get_best_printer_image callers). Returns (raw bytes copied, whether the
    source dir existed)."""
    src = repo_root / "build" / "esp32_printer_images"
    if not src.is_dir():
        return 0, False
    dest = out_dir / "assets" / "images" / "printers"
    shutil.copytree(src, dest, dirs_exist_ok=True)
    total = sum(f.stat().st_size for f in dest.rglob("*") if f.is_file())
    return total, True


# The "cold" font faces moved out of the compiled app image into the frogfs
# `storage` partition (Stage B fonts->frogfs enabler). Loaded at boot via
# lv_binfont_create("A:/assets/assets/fonts/<face>.bin") in
# firmware/helixscreen-esp32/main/font_registration.c. Generated as .bin twins
# by scripts/esp32_regen_compressed_fonts.sh (committed alongside the .c).
FONT_BIN_DIR = REPO_ROOT / "firmware" / "helixscreen-esp32" / "components" / "helixcore" / "fonts"


def stage_fonts(out_dir: Path) -> int:
    """Copy the runtime .bin font faces to assets/fonts/. Returns raw bytes."""
    bins = sorted(FONT_BIN_DIR.glob("*.bin"))
    if not bins:
        return 0
    dest = out_dir / "assets" / "fonts"
    dest.mkdir(parents=True, exist_ok=True)
    total = 0
    for src in bins:
        shutil.copy2(src, dest / src.name)
        total += src.stat().st_size
    return total


def stage_images(assets_dir: Path, out_dir: Path) -> int:
    """Rider I1: the general assets/images/ tree was never staged, so runtime
    paths like "A:/assets/assets/images/benchy_thumbnail_white.png" (correct
    for the mount) resolved to a missing file. Stage the referenced non-printer
    images: the benchy print-preview thumbnail and the AMS backend logos.
    Returns raw bytes."""
    dest_images = out_dir / "assets" / "images"
    total = 0
    for name in ("benchy_thumbnail_white.png",):
        src = assets_dir / "images" / name
        if src.is_file():
            dest_images.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest_images / name)
            total += src.stat().st_size
    ams_src = assets_dir / "images" / "ams"
    if ams_src.is_dir():
        ams_dest = dest_images / "ams"
        ams_dest.mkdir(parents=True, exist_ok=True)
        for src in sorted(ams_src.glob("*.png")):
            shutil.copy2(src, ams_dest / src.name)
            total += src.stat().st_size
    return total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help=f"staging output directory (default: {DEFAULT_OUT})")
    args = parser.parse_args()

    out_dir: Path = args.out
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    ui_xml_dir = REPO_ROOT / "ui_xml"
    assets_dir = REPO_ROOT / "assets"

    ui_xml_bytes = stage_ui_xml(ui_xml_dir, out_dir)
    ui_xml_bytes += stage_ui_xml_overrides(UI_XML_OVERRIDES_DIR, out_dir)
    translations_bytes, included_langs = stage_translations(ui_xml_dir, out_dir)
    config_bytes = stage_config(assets_dir, out_dir)
    filaments_bytes = stage_filaments(assets_dir, out_dir)
    printer_images_bytes, printer_images_present = stage_printer_images(REPO_ROOT, out_dir)
    images_bytes = stage_images(assets_dir, out_dir)
    fonts_bytes = stage_fonts(out_dir)

    total = (ui_xml_bytes + translations_bytes + config_bytes + filaments_bytes
             + printer_images_bytes + images_bytes + fonts_bytes)

    print("ESP32 storage staging tree (raw bytes; packed size is computed by "
          "esp32_pack_assets.py):")
    print(f"  ui_xml (minified, excl. micro/ + translations.xml): {format_bytes(ui_xml_bytes)}")
    print(f"  translations ({', '.join(included_langs) if included_langs else 'none'}): "
          f"{format_bytes(translations_bytes)}")
    print(f"  assets/config (printer_database, printing_tips, themes): {format_bytes(config_bytes)}")
    print(f"  assets/filaments.json: {format_bytes(filaments_bytes)}")
    if printer_images_present:
        print(f"  assets/images/printers (build/esp32_printer_images/): "
              f"{format_bytes(printer_images_bytes)}")
    else:
        print("  assets/images/printers: SKIPPED (build/esp32_printer_images/ not present — "
              "run scripts/esp32_printer_images.py first)")
    print(f"  assets/images (benchy thumbnail + AMS logos): {format_bytes(images_bytes)}")
    print(f"  assets/fonts (runtime .bin faces): {format_bytes(fonts_bytes)}")
    print(f"  TOTAL (raw, pre-pack): {format_bytes(total)}")
    print(f"  output: {out_dir}")
    print("  Next: python3 scripts/esp32_pack_assets.py (packs this tree and gates on the "
          "real 2.0MB storage partition budget)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
