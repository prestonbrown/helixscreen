#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Generate full-screen 3D splash images for HelixScreen.

Composites the 1024x1024 3D logo onto a full-screen canvas at each target
resolution. The background color is sampled from the source image's edges
so the logo blends seamlessly into a filled screen.

Output: LVGL ARGB8888 .bin files for each size/mode combination.

Usage:
    python scripts/gen_splash_3d.py                      # Generate all
    python scripts/gen_splash_3d.py --modes dark          # Dark only
    python scripts/gen_splash_3d.py --sizes medium        # AD5M only
    python scripts/gen_splash_3d.py --sizes medium --modes dark # AD5M dark only
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow not found. Run: pip install Pillow", file=sys.stderr)
    sys.exit(1)


# Screen size definitions matching regen_images.sh and prerendered_images.cpp
# Format: (name, width, height, logo_size)
# Classes are the UiBreakpoint tiers (include/ui_breakpoint.h), selected from the
# narrow axis by helix::get_splash_3d_size_name(). Logo is 50% of width below
# 500px tall, 60% at or above.
SCREEN_SIZES = [
    ("micro", 480, 272, 240),       # CC1
    ("tiny", 480, 320, 240),        # Snapmaker U1
    ("small", 480, 400, 240),
    ("medium", 800, 480, 400),      # K1, K2, AD5M, AD5X
    ("large", 1024, 600, 614),
    ("xlarge", 1280, 720, 768),
    ("ultrawide", 1920, 440, 384),  # Wide, short bar display
]

# Source images (relative to project root)
SOURCE_IMAGES = {
    "dark": "assets/images/helixscreen-logo-3d-dark.png",
    "light": "assets/images/helixscreen-logo-3d-light.png",
}


def sample_edge_color(img: Image.Image) -> tuple:
    """Sample the canonical background color from image corners.

    Uses the median of the four corner pixels for a clean, representative color.
    The source images have slightly noisy edges, so averaging would give a
    color that doesn't match any actual pixel.
    """
    rgb = img.convert("RGB")
    w, h = rgb.size

    corners = [
        rgb.getpixel((0, 0)),
        rgb.getpixel((w - 1, 0)),
        rgb.getpixel((0, h - 1)),
        rgb.getpixel((w - 1, h - 1)),
    ]

    r = sorted(c[0] for c in corners)[1]  # Median of 4 = avg of middle 2, take lower
    g = sorted(c[1] for c in corners)[1]
    b = sorted(c[2] for c in corners)[1]

    return (r, g, b)


def find_content_bounds(img: Image.Image, bg_color: tuple, tolerance: int = 20) -> tuple:
    """Find the vertical bounds of non-background content in the image.

    Returns (top_y, bottom_y) of the first and last rows containing
    pixels that differ from the background color.
    """
    rgb = img.convert("RGB")
    w, h = rgb.size
    bg_r, bg_g, bg_b = bg_color[:3]

    top = 0
    bottom = h

    # Scan from top
    for y in range(h):
        for x in range(0, w, 4):  # Sample every 4th pixel for speed
            p = rgb.getpixel((x, y))
            if (abs(p[0] - bg_r) > tolerance or
                abs(p[1] - bg_g) > tolerance or
                abs(p[2] - bg_b) > tolerance):
                top = y
                break
        else:
            continue
        break

    # Scan from bottom
    for y in range(h - 1, -1, -1):
        for x in range(0, w, 4):
            p = rgb.getpixel((x, y))
            if (abs(p[0] - bg_r) > tolerance or
                abs(p[1] - bg_g) > tolerance or
                abs(p[2] - bg_b) > tolerance):
                bottom = y + 1
                break
        else:
            continue
        break

    return (top, bottom)


def uniformize_background(img: Image.Image, bg_color: tuple, tolerance: int = 8) -> Image.Image:
    """Replace near-background pixels in the outer margin with exact bg color.

    Only processes pixels that are reachable from the edges via flood-fill
    (i.e., contiguous background regions touching the border). This avoids
    damaging the drop shadow or any interior detail of the logo.
    """
    pixels = img.load()
    w, h = img.size
    has_alpha = img.mode == "RGBA"
    bg_r, bg_g, bg_b = bg_color[:3]

    def is_bg(x: int, y: int) -> bool:
        p = pixels[x, y]
        return (abs(p[0] - bg_r) <= tolerance and
                abs(p[1] - bg_g) <= tolerance and
                abs(p[2] - bg_b) <= tolerance)

    # Flood-fill from all border pixels that match the bg color.
    # This finds only the contiguous background region around the logo,
    # not the drop shadow or interior areas.
    visited = set()
    queue = []

    # Seed from all four edges
    for x in range(w):
        if is_bg(x, 0):
            queue.append((x, 0))
        if is_bg(x, h - 1):
            queue.append((x, h - 1))
    for y in range(1, h - 1):
        if is_bg(0, y):
            queue.append((0, y))
        if is_bg(w - 1, y):
            queue.append((w - 1, y))

    # BFS flood fill
    while queue:
        cx, cy = queue.pop()
        if (cx, cy) in visited:
            continue
        if cx < 0 or cx >= w or cy < 0 or cy >= h:
            continue
        if not is_bg(cx, cy):
            continue

        visited.add((cx, cy))
        # Replace with exact bg color
        if has_alpha:
            pixels[cx, cy] = (bg_r, bg_g, bg_b, 255)
        else:
            pixels[cx, cy] = (bg_r, bg_g, bg_b)

        # Add 4-connected neighbors
        for dx, dy in [(-1, 0), (1, 0), (0, -1), (0, 1)]:
            nx, ny = cx + dx, cy + dy
            if 0 <= nx < w and 0 <= ny < h and (nx, ny) not in visited:
                queue.append((nx, ny))

    return img


def generate_splash(
    source_path: Path,
    output_dir: Path,
    output_name: str,
    screen_width: int,
    screen_height: int,
    logo_size: int,
    lvgl_image_py: Path,
    python_exe: str,
) -> bool:
    """Generate a full-screen splash image using crop-and-extend.

    Instead of placing a small logo on a large canvas, we:
    1. Crop the source image to the target aspect ratio (removing excess top/bottom)
    2. Scale up to fill the screen width
    3. Extend edges if the cropped result is shorter than the screen

    This makes the logo fill much more of the widescreen display.
    The logo_size parameter is used as a safety limit — the logo graphic
    must not be cropped.
    """
    try:
        img = Image.open(source_path).convert("RGBA")
    except Exception as e:
        print(f"  Error opening {source_path}: {e}", file=sys.stderr)
        return False

    src_w, src_h = img.size  # 1024x1024

    # Sample background color from corners
    bg_color = sample_edge_color(img)

    # Detect logo content bounds (rows with non-background pixels)
    content_top, content_bottom = find_content_bounds(img, bg_color)
    content_h = content_bottom - content_top

    # Scale factor: make the logo fill as much of the screen as possible
    # while keeping the content fully visible.
    #
    # Two constraints:
    #   1. Content must fit vertically: scale <= screen_height / content_h
    #   2. Image must fill screen width: scale = screen_width / src_w
    #
    # Use the smaller of the two to ensure both constraints are met.
    # Add a small margin (5% of screen height) so the logo doesn't touch edges.
    margin_px = int(screen_height * 0.12)
    scale_by_height = (screen_height - margin_px) / content_h
    scale_by_width = screen_width / src_w
    scale_factor = min(scale_by_height, scale_by_width)

    # Scale the full source image
    scaled_w = int(src_w * scale_factor)
    scaled_h = int(src_h * scale_factor)
    scaled = img.resize((scaled_w, scaled_h), Image.LANCZOS)

    # Clean up noisy background pixels so they blend seamlessly
    scaled = uniformize_background(scaled, bg_color)

    # Create full-screen canvas and paste scaled image centered
    canvas = Image.new("RGBA", (screen_width, screen_height), bg_color + (255,))
    x_offset = (screen_width - scaled_w) // 2
    y_offset = (screen_height - scaled_h) // 2
    canvas.paste(scaled, (x_offset, y_offset), scaled)

    # Save to temp PNG, then convert with LVGLImage.py
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
        tmp_path = tmp.name
        canvas.save(tmp_path)

    try:
        result = subprocess.run(
            [
                python_exe,
                str(lvgl_image_py),
                "--cf", "ARGB8888",
                "--ofmt", "BIN",
                "--compress", "LZ4",
                "-o", str(output_dir),
                "--name", output_name,
                tmp_path,
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            print(f"  LVGLImage.py error: {result.stderr}", file=sys.stderr)
            return False
        return True
    finally:
        os.unlink(tmp_path)


def main():
    parser = argparse.ArgumentParser(description="Generate full-screen 3D splash images")
    parser.add_argument(
        "--sizes",
        nargs="+",
        default=None,
        help="Size names to generate (default: all). Values: "
             + ", ".join(n for n, _w, _h, _l in SCREEN_SIZES),
    )
    parser.add_argument(
        "--modes",
        nargs="+",
        default=None,
        help="Modes to generate (default: all). Values: dark, light",
    )
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Output directory (default: build/assets/images/prerendered)",
    )
    parser.add_argument(
        "--project-dir",
        default=None,
        help="Project root directory (default: auto-detect from script location)",
    )
    args = parser.parse_args()

    # Find project root
    if args.project_dir:
        project_dir = Path(args.project_dir)
    else:
        project_dir = Path(__file__).resolve().parent.parent

    # Output directory
    output_dir = Path(args.output_dir) if args.output_dir else project_dir / "build" / "assets" / "images" / "prerendered"
    output_dir.mkdir(parents=True, exist_ok=True)

    # LVGLImage.py path
    lvgl_image_py = project_dir / "scripts" / "LVGLImage.py"
    if not lvgl_image_py.exists():
        print(f"Error: LVGLImage.py not found at {lvgl_image_py}", file=sys.stderr)
        sys.exit(1)

    # Python executable (prefer venv)
    venv_python = project_dir / ".venv" / "bin" / "python"
    python_exe = str(venv_python) if venv_python.exists() else "python3"

    # Filter sizes
    valid_size_names = {s[0] for s in SCREEN_SIZES}
    if args.sizes:
        for s in args.sizes:
            if s not in valid_size_names:
                print(f"Error: Unknown size '{s}'. Valid: {', '.join(sorted(valid_size_names))}", file=sys.stderr)
                sys.exit(1)
        sizes = [s for s in SCREEN_SIZES if s[0] in args.sizes]
    else:
        sizes = SCREEN_SIZES

    # Filter modes
    valid_modes = {"dark", "light"}
    if args.modes:
        for m in args.modes:
            if m not in valid_modes:
                print(f"Error: Unknown mode '{m}'. Valid: dark, light", file=sys.stderr)
                sys.exit(1)
        modes = args.modes
    else:
        modes = list(SOURCE_IMAGES.keys())

    # Generate
    print(f"Generating 3D splash images ({len(modes)} modes x {len(sizes)} sizes)...")
    success = 0
    failed = 0

    for mode in modes:
        source_rel = SOURCE_IMAGES[mode]
        source_path = project_dir / source_rel
        if not source_path.exists():
            print(f"  Error: Source not found: {source_path}", file=sys.stderr)
            failed += len(sizes)
            continue

        print(f"\n  {mode}: {source_rel}")
        for name, width, height, logo_size in sizes:
            output_name = f"splash-3d-{mode}-{name}"
            sys.stdout.write(f"    {name} ({width}x{height}, logo {logo_size}px)... ")
            sys.stdout.flush()

            if generate_splash(
                source_path, output_dir, output_name,
                width, height, logo_size,
                lvgl_image_py, python_exe,
            ):
                # Show file size
                out_file = output_dir / f"{output_name}.bin"
                size_kb = out_file.stat().st_size / 1024
                print(f"OK ({size_kb:.0f} KB)")
                success += 1
            else:
                print("FAILED")
                failed += 1

    print(f"\nDone: {success} generated, {failed} failed")
    if failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
