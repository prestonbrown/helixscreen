#!/usr/bin/env bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate RLE-COMPRESSED, firmware-local twins of the 11 LVGL font faces
# used by the ESP32 build of HelixScreen.
#
# These .c files are the COMPRESSED counterparts of the (uncompressed on the
# desktop side) faces in assets/fonts/. They are regenerated from the EXACT
# same TTF/OTF sources, sizes, Unicode ranges, bpp, and format as
# scripts/regen_text_fonts.sh and scripts/regen_mdi_fonts.sh, with ONE
# difference: lv_font_conv's default RLE compression is left ENABLED (the
# desktop scripts pass --no-compress). Compression trades a small runtime
# decode cost for a large flash saving, which matters on the ESP32's limited
# flash budget but not on desktop.
#
# Output: firmware/helixscreen-esp32/components/helixcore/fonts/<face>.c
#   The firmware build points HELIX_FONT_SRCS
#   (firmware/helixscreen-esp32/components/helixcore/CMakeLists.txt) at these
#   files instead of the desktop assets/fonts/*.c originals.
#
# Symbol names, glyph sets (bar the ranges noted below), sizes, and bpp match the desktop
# originals so the firmware externs/aliases resolve unchanged. Const-ness of
# the top-level `lv_font_t <name>` symbol is also matched to the desktop
# originals AND to the firmware externs in
# firmware/helixscreen-esp32/components/helixcore/lv_conf.h. All 11 faces are
# MOVED (runtime .bin + writable shim), so all of them are const-stripped:
#   - noto_sans_* faces  -> `lv_font_t <name>`  (const stripped, always were)
#   - source_code_pro_14 -> `lv_font_t <name>`  (const stripped: moved face)
#   - mdi_icons_*  faces -> `lv_font_t <name>`  (const stripped: moved faces)
# The .bin twins (frogfs faces loaded at boot) are emitted for all 11 faces:
# noto_sans_18, noto_sans_26, noto_sans_bold_28, noto_sans_light_16,
# noto_sans_light_12, source_code_pro_14, and mdi_icons_16/24/32/48/64. No
# Helix face compiles into the app image; LVGL's built-in montserrat_14
# (LV_FONT_DEFAULT) is the load-failure fallback.
#
# This script does NOT touch anything under assets/fonts/.

set -euo pipefail
cd "$(dirname "$0")/.."

# --- lv_font_conv (v1.5.3) on PATH -------------------------------------------
# Use the project-local npm install, matching regen_text_fonts.sh /
# regen_mdi_fonts.sh — not a personal nvm path.
export PATH="$PWD/node_modules/.bin:$PATH"
if ! command -v lv_font_conv >/dev/null 2>&1; then
    echo "ERROR: lv_font_conv not found on PATH" >&2
    exit 1
fi

OUT_DIR="firmware/helixscreen-esp32/components/helixcore/fonts"
mkdir -p "$OUT_DIR"

# --- Font source files -------------------------------------------------------
FONT_REGULAR=assets/fonts/NotoSans-Regular.ttf
FONT_LIGHT=assets/fonts/NotoSans-Light.ttf
FONT_BOLD=assets/fonts/NotoSans-Bold.ttf
FONT_MONO=assets/fonts/SourceCodePro-Regular.ttf
FONT_CJK_SC=assets/fonts/NotoSansCJKsc-Regular.otf
FONT_CJK_JP=assets/fonts/NotoSansCJKjp-Regular.otf
FONT_MDI=assets/fonts/materialdesignicons-webfont.ttf
FONT_CUSTOM=assets/fonts/helix-custom-icons.ttf

# CJK font download URLs (Google Noto CJK releases) - matches regen_text_fonts.sh
CJK_SC_URL="https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"
CJK_JP_URL="https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf"

download_cjk_font() {
    local url="$1" dest="$2" name
    name=$(basename "$dest")
    [ -f "$dest" ] && return 0
    echo "Downloading $name..."
    if command -v curl >/dev/null 2>&1; then
        curl -fSL --progress-bar -o "$dest" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -q --show-progress -O "$dest" "$url"
    else
        echo "ERROR: Neither curl nor wget found - cannot download $name" >&2
        return 1
    fi
    [ -s "$dest" ] || { echo "ERROR: Download failed for $name" >&2; rm -f "$dest"; return 1; }
}

for FONT in "$FONT_REGULAR" "$FONT_LIGHT" "$FONT_BOLD" "$FONT_MONO" "$FONT_MDI"; do
    [ -f "$FONT" ] || { echo "ERROR: Font not found: $FONT" >&2; exit 1; }
done
if [ ! -f "$FONT_CJK_SC" ] || [ ! -f "$FONT_CJK_JP" ]; then
    echo "CJK fonts not found - downloading from GitHub notofonts/noto-cjk..."
    download_cjk_font "$CJK_SC_URL" "$FONT_CJK_SC" || { echo "ERROR: CJK SC font is REQUIRED." >&2; exit 1; }
    download_cjk_font "$CJK_JP_URL" "$FONT_CJK_JP" || { echo "ERROR: CJK JP font is REQUIRED." >&2; exit 1; }
fi

# --- Ranges (verbatim from regen_text_fonts.sh / regen_mdi_fonts.sh) ---------
# Except the eighth fractions (0x215B-0x215E): only the Belt Tension panel
# draws them, and the firmware does not build it.

# Wizard welcome page CJK codepoints - always compiled into the .c fonts.
# 欢迎！中文ようこそ！日本語
WIZARD_CJK="0x3046,0x3053,0x305d,0x3088,0x4e2d,0x6587,0x65e5,0x672c,0x6b22,0x8a9e,0x8fce,0xff01"

# Unicode ranges for Latin/Cyrillic (Noto text faces + Source Code Pro)
UNICODE_RANGES=""
UNICODE_RANGES+="0x20-0x7F"      # Basic Latin (ASCII)
UNICODE_RANGES+=",0xA0-0xFF"     # Latin-1 Supplement (Western European)
UNICODE_RANGES+=",0x100-0x17F"   # Latin Extended-A (Central European)
UNICODE_RANGES+=",0x400-0x4FF"   # Cyrillic (Russian, Ukrainian, etc.)
UNICODE_RANGES+=",0x2013-0x2014" # En/Em dashes
UNICODE_RANGES+=",0x2018-0x201D" # Smart quotes
UNICODE_RANGES+=",0x2022"        # Bullet
UNICODE_RANGES+=",0x2026"        # Ellipsis
UNICODE_RANGES+=",0x20AC"        # Euro sign
UNICODE_RANGES+=",0x2122"        # Trademark

# Icon codepoints: the same list the desktop faces carry (regen_mdi_fonts.sh),
# computed from include/ui_icon_codepoints.h and the ICON_* macros in
# include/ui_fonts.h, so a glyph added there reaches the firmware faces on the
# next regen. The generator fails rather than print an empty list.
MDI_ICONS="$(python3 scripts/mdi_icon_codepoints.py --mdi)"
CUSTOM_ICONS="$(python3 scripts/mdi_icon_codepoints.py --custom)"

# --- Const-strip helper (Linux sed) ------------------------------------------
# Applied ONLY to the Noto text faces, matching regen_text_fonts.sh and the
# `extern lv_font_t noto_sans_*` (non-const) declarations in lv_conf.h. The
# CjkFontManager sets fallback pointers on these at runtime, so they must not
# be const. source_code_pro_14 and mdi_icons_* stay const (extern const in
# lv_conf.h; the desktop scripts do not strip them).
strip_const() { sed -i 's/^const lv_font_t /lv_font_t /' "$1"; }

echo "Generating COMPRESSED firmware-local font twins -> $OUT_DIR"
echo ""

# --- Noto text faces (Latin/Cyrillic + 12-codepoint CJK wizard subset) -------
# Same invocation as regen_text_fonts.sh minus --no-compress; then strip const.

# Passing "bin" as the 4th arg ALSO emits a runtime-loadable .bin twin (same
# glyphs/ranges/bpp, lv_font_conv --format bin) alongside the compiled .c. The
# .bin twins are the faces moved out of the app image into the frogfs `storage`
# partition (loaded at boot via lv_binfont_create); the .c stays generated so
# the face can be moved back into the compile without a regen.
gen_noto() { # <src_ttf> <size> <outname> [bin]
    local src="$1" size="$2" name="$3" emit_bin="${4:-}" out="$OUT_DIR/$3.c"
    echo "  $name (compressed) -> $out"
    lv_font_conv \
        --font "$src"        --size "$size" --range "$UNICODE_RANGES" \
        --font "$FONT_CJK_SC" --size "$size" --range "$WIZARD_CJK" \
        --font "$FONT_CJK_JP" --size "$size" --range "$WIZARD_CJK" \
        --bpp 4 --format lvgl \
        -o "$out"
    strip_const "$out"
    if [ "$emit_bin" = "bin" ]; then
        echo "  $name (compressed) -> $OUT_DIR/$name.bin  [frogfs twin]"
        lv_font_conv \
            --font "$src"        --size "$size" --range "$UNICODE_RANGES" \
            --font "$FONT_CJK_SC" --size "$size" --range "$WIZARD_CJK" \
            --font "$FONT_CJK_JP" --size "$size" --range "$WIZARD_CJK" \
            --bpp 4 --format bin \
            -o "$OUT_DIR/$name.bin"
    fi
}

gen_noto "$FONT_REGULAR" 26 noto_sans_26 bin
gen_noto "$FONT_BOLD"    28 noto_sans_bold_28 bin
gen_noto "$FONT_REGULAR" 18 noto_sans_18 bin
gen_noto "$FONT_LIGHT"   16 noto_sans_light_16 bin
gen_noto "$FONT_LIGHT"   12 noto_sans_light_12 bin

# --- Source Code Pro monospace ----------------------------------------------
# regen_text_fonts.sh omits --no-compress for this face (so the desktop asset is
# itself compressed). source_code_pro_14 is now a MOVED face: its glyph data
# lives in a runtime .bin (frogfs twin) and its symbol is the writable shim in
# moved_fonts_shim.c, so ui_fonts.h / lv_conf.h declare it non-const. Strip
# const on the .c twin to match (so a move-back into the compile stays
# qualifier-clean) and emit the .bin twin alongside.
echo "  source_code_pro_14 (compressed) -> $OUT_DIR/source_code_pro_14.c"
lv_font_conv \
    --font "$FONT_MONO" --size 14 --bpp 4 --format lvgl \
    --range "$UNICODE_RANGES" \
    -o "$OUT_DIR/source_code_pro_14.c"
strip_const "$OUT_DIR/source_code_pro_14.c"
echo "  source_code_pro_14 (compressed) -> $OUT_DIR/source_code_pro_14.bin  [frogfs twin]"
lv_font_conv \
    --font "$FONT_MONO" --size 14 --bpp 4 --format bin \
    --range "$UNICODE_RANGES" \
    -o "$OUT_DIR/source_code_pro_14.bin"

# --- MDI icon faces ----------------------------------------------------------
# Same invocation as regen_mdi_fonts.sh minus --no-compress. All mdi sizes this
# script generates (16/24/32/48/64) are now MOVED faces: their symbols are
# non-const runtime-populated shims in ui_fonts.h / lv_conf.h (glyph data in a
# frogfs .bin), so every compiled twin must be de-const'd or moving the face
# back into the compile hits conflicting-qualifiers. Strip const on all of them.
gen_mdi() { # <size> [bin]
    local size="$1" emit_bin="${2:-}" out="$OUT_DIR/mdi_icons_$1.c"
    echo "  mdi_icons_$size (compressed) -> $out"
    lv_font_conv \
        --font "$FONT_MDI" --size "$size" --bpp 4 --format lvgl \
        --range "$MDI_ICONS" \
        --font "$FONT_CUSTOM" --range "$CUSTOM_ICONS" \
        -o "$out"
    sed -i 's/^const lv_font_t /lv_font_t /' "$out"
    if [ "$emit_bin" = "bin" ]; then
        echo "  mdi_icons_$size (compressed) -> $OUT_DIR/mdi_icons_$size.bin  [frogfs twin]"
        lv_font_conv \
            --font "$FONT_MDI" --size "$size" --bpp 4 --format bin \
            --range "$MDI_ICONS" \
            --font "$FONT_CUSTOM" --range "$CUSTOM_ICONS" \
            -o "$OUT_DIR/mdi_icons_$size.bin"
    fi
}

gen_mdi 16 bin
gen_mdi 24 bin
gen_mdi 32 bin
gen_mdi 48 bin
gen_mdi 64 bin

echo ""
echo "Done. 11 compressed .c font twins + 11 .bin frogfs twins written to $OUT_DIR"
