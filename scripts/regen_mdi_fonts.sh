#!/bin/bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate Material Design Icons fonts for LVGL
#
# This script generates LVGL font files using MDI (Pictogrammers) icons.
# MDI provides 7200+ icons in a single unified font, eliminating the need
# for FontAwesome's split Solid/Brands/Regular fonts.
#
# MDI uses Unicode Plane 15 Private Use Area (0xF0000+)
# UTF-8 encoding: 4 bytes per codepoint (vs FontAwesome's 3-byte 0xF000 range)
#
# Source: https://pictogrammers.com/library/mdi/
# Font:   https://github.com/Templarian/MaterialDesign-Webfont

set -e
cd "$(dirname "$0")/.."

# Add node_modules/.bin to PATH so lv_font_conv is available
# (same as how npm scripts work)
export PATH="$PWD/node_modules/.bin:$PATH"

FONT=assets/fonts/materialdesignicons-webfont.ttf
# Glyphs MDI does not have (brand marks), merged into every face below. Built
# from assets/fonts/custom_icons/*.svg by scripts/build_custom_icon_font.py.
FONT_CUSTOM=assets/fonts/helix-custom-icons.ttf

# Check font exists
if [ ! -f "$FONT" ]; then
    echo "ERROR: MDI font not found at $FONT"
    echo "Download from: https://github.com/Templarian/MaterialDesign-Webfont/raw/master/fonts/materialdesignicons-webfont.ttf"
    exit 1
fi

# The glyphs every face carries: whatever include/ui_icon_codepoints.h and the
# ICON_* macros in include/ui_fonts.h name, computed by mdi_icon_codepoints.py.
# Adding an icon there and running `make regen-fonts` is the whole change.
MDI_ICONS="$(python3 scripts/mdi_icon_codepoints.py --mdi)"
# Custom block (0xFE000 and up), served from FONT_CUSTOM.
CUSTOM_ICONS="$(python3 scripts/mdi_icon_codepoints.py --custom)"

# Sizes to generate - matching text font sizes for consistent UI
SIZES="14 16 24 32 48 64 80 96 128"

echo "Generating Material Design Icons fonts for LVGL..."
echo "  Font: $FONT"
echo ""

for SIZE in $SIZES; do
    OUTPUT="assets/fonts/mdi_icons_${SIZE}.c"
    echo "  Generating mdi_icons_${SIZE} -> $OUTPUT"

    lv_font_conv \
        --font "$FONT" --size "$SIZE" --bpp 4 --format lvgl \
        --range "$MDI_ICONS" \
        --font "$FONT_CUSTOM" --range "$CUSTOM_ICONS" \
        --no-compress \
        -o "$OUTPUT"

    # De-const mdi_icons_16/24/32/48/64: these five faces are runtime-populated
    # on embedded targets (ESP32 loads their glyph data from a .bin and
    # struct-copies into the symbol at boot; Plan A fonts->frogfs move), so
    # ui_fonts.h declares them non-const. The .c definition must match or gcc
    # errors with "conflicting type qualifiers". The remaining sizes (14/80/96/
    # 128) are NOT moved and stay const. Mirrors the strip in
    # scripts/regen_text_fonts.sh.
    if [ "$SIZE" = "16" ] || [ "$SIZE" = "24" ] || [ "$SIZE" = "32" ] || \
       [ "$SIZE" = "48" ] || [ "$SIZE" = "64" ]; then
        if [[ "$OSTYPE" == "darwin"* ]]; then
            sed -i '' 's/^const lv_font_t /lv_font_t /' "$OUTPUT"
        else
            sed -i 's/^const lv_font_t /lv_font_t /' "$OUTPUT"
        fi
    fi
done

echo ""
echo "Done! Generated fonts for sizes: $SIZES"
echo ""
echo "Fonts are ready. If you added new icons, remember to:"
echo "  1. Add mapping to include/ui_icon_codepoints.h"
echo "  2. Rebuild the project: make -j"
