#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Downscale + palette-quantize assets/images/printers/*.png for the ESP32
packed asset container (Plan 4 Task 3).

Rendition width matches what PrinterImageWidget actually requests at runtime:
get_printer_image_size(screen_width) in src/system/prerendered_images.cpp
returns 300px for screen_width >= 600 (the K-Touch panel is 800px wide), so
every printer image is re-rendered at width=300 (aspect-preserved height).

Quality is favored over squeeze (headroom is ample post-container): each
image is palette-quantized to up to 256 colors via Pillow's FASTOCTREE
method, the only quantize method that quantizes the alpha channel jointly
with color instead of reducing to binary transparency. Falls back to fewer
colors only if 256 doesn't help, and to a plain resized RGBA PNG (no
quantization) if quantizing would produce a LARGER file than the resize
alone (rare, but happens on some near-flat/low-color source art).

Usage:
    python3 -m venv /tmp/esp32imgs-venv && . /tmp/esp32imgs-venv/bin/activate
    pip install Pillow
    python3 scripts/esp32_printer_images.py [--out DIR]

All source images ship — never silently dropped. If any image fails to
process, this script exits non-zero rather than shipping a partial set.
"""

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("FAIL: Pillow not installed. Use a venv:\n"
          "  python3 -m venv /tmp/esp32imgs-venv && "
          ". /tmp/esp32imgs-venv/bin/activate && pip install Pillow",
          file=sys.stderr)
    sys.exit(1)

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_DIR = REPO_ROOT / "assets" / "images" / "printers"
DEFAULT_OUT = REPO_ROOT / "build" / "esp32_printer_images"

# Matches get_printer_image_size(800) in src/system/prerendered_images.cpp
# (screen_width >= 600 -> 300px). The K-Touch panel is 800px wide.
TARGET_WIDTH = 300

# Excluded from the image set (documentation, not an image).
EXCLUDED_FILES = ("README.md",)


def format_bytes(n: int) -> str:
    return f"{n:,} B ({n / 1024:.1f} KiB)"


def render_one(src: Path, dest: Path) -> tuple[int, int]:
    """Downscale + quantize a single PNG. Returns (orig_bytes, out_bytes)."""
    orig_bytes = src.stat().st_size

    im = Image.open(src).convert("RGBA")
    if im.width != TARGET_WIDTH:
        target_h = round(im.height * TARGET_WIDTH / im.width)
        resized = im.resize((TARGET_WIDTH, target_h), Image.LANCZOS)
    else:
        resized = im

    import io

    def encode(image: Image.Image) -> bytes:
        buf = io.BytesIO()
        image.save(buf, "PNG", optimize=True)
        return buf.getvalue()

    resized_bytes = encode(resized)
    best = resized_bytes

    # Try 256 colors first ("favoring quality"); only drop to fewer colors
    # if 256 didn't already beat the plain resized RGBA PNG.
    for colors in (256, 128, 64):
        quantized = resized.quantize(colors=colors, method=Image.Quantize.FASTOCTREE,
                                     dither=Image.Dither.FLOYDSTEINBERG)
        encoded = encode(quantized)
        if len(encoded) < len(best):
            best = encoded
        if len(best) < len(resized_bytes):
            break

    dest.parent.mkdir(parents=True, exist_ok=True)
    with open(dest, "wb") as f:
        f.write(best)

    return orig_bytes, len(best)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help=f"output directory (default: {DEFAULT_OUT})")
    args = parser.parse_args()

    out_dir: Path = args.out
    out_dir.mkdir(parents=True, exist_ok=True)

    sources = sorted(
        p for p in SOURCE_DIR.glob("*.png") if p.name not in EXCLUDED_FILES
    )
    if not sources:
        print(f"FAIL: no PNG files found in {SOURCE_DIR}", file=sys.stderr)
        return 1

    rows = []
    total_orig = 0
    total_out = 0
    failures = []

    for src in sources:
        dest = out_dir / src.name
        try:
            orig_bytes, out_bytes = render_one(src, dest)
        except Exception as exc:  # noqa: BLE001 - report and keep going, but fail the run
            failures.append((src.name, str(exc)))
            continue
        rows.append((src.name, orig_bytes, out_bytes))
        total_orig += orig_bytes
        total_out += out_bytes

    print(f"ESP32 printer image pipeline: {SOURCE_DIR} -> {out_dir}")
    print(f"  Target width: {TARGET_WIDTH}px (matches get_printer_image_size(800))")
    print()
    print(f"  {'file':<38} {'orig':>14} {'packed':>14} {'ratio':>8}")
    for name, orig_bytes, out_bytes in rows:
        ratio = f"{100.0 * out_bytes / orig_bytes:.0f}%" if orig_bytes else "n/a"
        print(f"  {name:<38} {orig_bytes:>10,} B {out_bytes:>10,} B {ratio:>8}")
    print()
    print(f"  TOTAL: {len(rows)} images, {format_bytes(total_orig)} -> {format_bytes(total_out)}"
          f" ({100.0 * total_out / total_orig:.1f}%)" if total_orig else "  TOTAL: 0 images")

    if failures:
        print(file=sys.stderr)
        print(f"FAIL: {len(failures)} image(s) failed to process (all images must ship):",
              file=sys.stderr)
        for name, error in failures:
            print(f"  {name}: {error}", file=sys.stderr)
        return 1

    if len(rows) != len(sources):
        print(f"FAIL: only {len(rows)}/{len(sources)} images processed", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
