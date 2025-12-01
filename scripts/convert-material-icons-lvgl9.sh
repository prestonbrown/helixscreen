#!/bin/bash

# Copyright 2025 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later

set -e

SVG_SOURCE="/Users/pbrown/Code/Printing/helixscreen/assets/images/material"
TEMP_PNG_DIR="/tmp/material_png"
OUTPUT_DIR="assets/images/material"
VENV_PYTHON=".venv/bin/python3"

echo "=== LVGL 9 Material Icon Conversion ==="
echo "Source SVGs: $SVG_SOURCE"
echo "Output C arrays: $OUTPUT_DIR"
echo ""

# Create temp directory for PNGs
mkdir -p "$TEMP_PNG_DIR"
mkdir -p "$OUTPUT_DIR"

# Step 1: Convert SVGs to PNGs (64x64)
echo "Step 1: Converting SVGs to PNGs..."
count=0
for svg_file in "$SVG_SOURCE"/*.svg; do
    filename=$(basename "$svg_file" .svg)
    png_file="$TEMP_PNG_DIR/${filename}.png"

    # Convert SVG to 64x64 RGBA PNG with alpha transparency
    # Use Inkscape for proper alpha channel preservation (ImageMagick loses transparency)
    inkscape "$svg_file" --export-type=png --export-filename="$png_file" -w 64 -h 64 >/dev/null 2>&1
    ((count++))
done
echo "Converted $count SVGs to PNG"
echo ""

# Step 2: Convert PNGs to LVGL 9 C arrays
echo "Step 2: Converting PNGs to LVGL 9 C arrays..."
# Use RGB565A8: 16-bit RGB565 + 8-bit alpha channel
# Works with C++ recoloring via lv_obj_set_style_img_recolor()
$VENV_PYTHON scripts/LVGLImage.py \
    --ofmt C \
    --cf RGB565A8 \
    --compress NONE \
    -o "$OUTPUT_DIR" \
    "$TEMP_PNG_DIR"

# Clean up temp directory
rm -rf "$TEMP_PNG_DIR"

echo ""
echo "=== Conversion Complete ==="
echo "Generated $(ls "$OUTPUT_DIR"/*.c | wc -l | tr -d ' ') LVGL 9 C files"
echo ""
echo "Next steps:"
echo "  1. Run: make clean && make"
echo "  2. Test: ./build/bin/helix-ui-proto"
