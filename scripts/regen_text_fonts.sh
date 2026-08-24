#!/bin/bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate Noto Sans text fonts for LVGL
#
# This script generates LVGL font files for UI text rendering.
# Includes:
# - Latin (ASCII, Western European, Central European)
# - Cyrillic (Russian, Ukrainian, etc.)
# - CJK wizard subset (12 codepoints compiled into .c fonts)
# - CJK runtime (full Chinese + Japanese as .bin files for runtime loading)
#
# Source fonts:
# - assets/fonts/NotoSans-*.ttf (Latin/Cyrillic)
# - assets/fonts/NotoSansCJKsc-Regular.otf (Chinese)
# - assets/fonts/NotoSansCJKjp-Regular.otf (Japanese)

set -e
cd "$(dirname "$0")/.."

# Add node_modules/.bin to PATH for lv_font_conv
export PATH="$PWD/node_modules/.bin:$PATH"

# Font source files
FONT_REGULAR=assets/fonts/NotoSans-Regular.ttf
FONT_LIGHT=assets/fonts/NotoSans-Light.ttf
FONT_BOLD=assets/fonts/NotoSans-Bold.ttf
FONT_CJK_SC=assets/fonts/NotoSansCJKsc-Regular.otf
FONT_CJK_JP=assets/fonts/NotoSansCJKjp-Regular.otf

# Check Latin fonts exist
for FONT in "$FONT_REGULAR" "$FONT_LIGHT" "$FONT_BOLD"; do
    if [ ! -f "$FONT" ]; then
        echo "ERROR: Font not found: $FONT"
        echo "Download Noto Sans from: https://fonts.google.com/noto/specimen/Noto+Sans"
        exit 1
    fi
done

# CJK font download URLs (Google Noto CJK releases)
CJK_SC_URL="https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"
CJK_JP_URL="https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf"

# Auto-download CJK fonts if missing
download_cjk_font() {
    local url="$1"
    local dest="$2"
    local name
    name=$(basename "$dest")

    if [ -f "$dest" ]; then
        return 0
    fi

    echo "Downloading $name..."
    if command -v curl >/dev/null 2>&1; then
        curl -fSL --progress-bar -o "$dest" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -q --show-progress -O "$dest" "$url"
    else
        echo "ERROR: Neither curl nor wget found - cannot download $name"
        return 1
    fi

    if [ ! -f "$dest" ] || [ ! -s "$dest" ]; then
        echo "ERROR: Download failed for $name"
        rm -f "$dest"
        return 1
    fi

    echo "Downloaded $name ($(du -h "$dest" | cut -f1))"
}

if [ ! -f "$FONT_CJK_SC" ] || [ ! -f "$FONT_CJK_JP" ]; then
    echo "CJK fonts not found - downloading from GitHub notofonts/noto-cjk..."
    download_cjk_font "$CJK_SC_URL" "$FONT_CJK_SC" || { echo "ERROR: Failed to download CJK SC font. CJK support is REQUIRED."; exit 1; }
    download_cjk_font "$CJK_JP_URL" "$FONT_CJK_JP" || { echo "ERROR: Failed to download CJK JP font. CJK support is REQUIRED."; exit 1; }
fi

echo "CJK fonts found - will include Chinese and Japanese support"

# Wizard welcome page CJK codepoints — always compiled into .c fonts
# 欢迎！中文ようこそ！日本語
WIZARD_CJK="0x3046,0x3053,0x305d,0x3088,0x4e2d,0x6587,0x65e5,0x672c,0x6b22,0x8a9e,0x8fce,0xff01"

# Unicode ranges for Latin/Cyrillic
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

# Extract ALL CJK characters from translations and C++ sources
echo "Extracting CJK characters from translations and C++ sources..."
ALL_CJKCHARS=$(python3 << 'EOF'
import glob
import re

chars = set()

# CJK Unicode ranges to scan for
CJK_RANGES = [
    r'[\u3000-\u303f]',   # CJK Symbols and Punctuation
    r'[\u3040-\u309f]',   # Hiragana
    r'[\u30a0-\u30ff]',   # Katakana
    r'[\u3400-\u4dbf]',   # CJK Unified Ideographs Extension A
    r'[\u4e00-\u9fff]',   # CJK Unified Ideographs
    r'[\uff00-\uffef]',   # Halfwidth and Fullwidth Forms
]

def extract_cjk(content):
    """Extract all CJK characters from a string."""
    found = set()
    for pattern in CJK_RANGES:
        found.update(re.findall(pattern, content))
    return found

# Translation files
for path in ['translations/zh.yml', 'translations/ja.yml']:
    try:
        with open(path, 'r') as f:
            chars.update(extract_cjk(f.read()))
    except FileNotFoundError:
        pass

# C++ source files (catches hardcoded CJK strings like welcome text)
for pattern in ['src/**/*.cpp', 'src/**/*.h', 'include/**/*.h']:
    for path in glob.glob(pattern, recursive=True):
        try:
            with open(path, 'r') as f:
                chars.update(extract_cjk(f.read()))
        except (FileNotFoundError, UnicodeDecodeError):
            pass

if chars:
    print(','.join(f'0x{ord(c):04x}' for c in sorted(chars)))
EOF
)
if [ -n "$ALL_CJKCHARS" ]; then
    ALL_CJK_COUNT=$(echo "$ALL_CJKCHARS" | tr ',' '\n' | wc -l | tr -d ' ')
    echo "Found $ALL_CJK_COUNT unique CJK characters total"
else
    echo "WARNING: No CJK characters found in translations or source files"
fi

# Runtime CJK = full extracted set (for .bin files)
RUNTIME_CJKCHARS="$ALL_CJKCHARS"
echo "Wizard CJK: 12 codepoints (compiled into .c fonts)"
if [ -n "$RUNTIME_CJKCHARS" ]; then
    RUNTIME_CJK_COUNT=$(echo "$RUNTIME_CJKCHARS" | tr ',' '\n' | wc -l | tr -d ' ')
    echo "Runtime CJK: $RUNTIME_CJK_COUNT codepoints (generated as .bin files)"
fi

# Font sizes
SIZES_REGULAR="8 10 11 12 14 16 18 20 24 26 28 32 40"
SIZES_LIGHT="10 11 12 14 16 18 20 26"
SIZES_BOLD="14 16 18 20 24 28 32 40"

echo ""
echo "Regenerating Noto Sans text fonts for LVGL..."

# Generate Regular weight
echo ""
echo "Regular weight:"
for SIZE in $SIZES_REGULAR; do
    OUTPUT="assets/fonts/noto_sans_${SIZE}.c"
    echo "  Generating noto_sans_${SIZE} -> $OUTPUT"

    lv_font_conv \
        --font "$FONT_REGULAR" --size "$SIZE" --range "$UNICODE_RANGES" \
        --font "$FONT_CJK_SC" --size "$SIZE" --range "$WIZARD_CJK" \
        --font "$FONT_CJK_JP" --size "$SIZE" --range "$WIZARD_CJK" \
        --bpp 4 --format lvgl \
        --no-compress \
        -o "$OUTPUT"
    # Strip const so CjkFontManager can set fallback pointers at runtime
    if [[ "$OSTYPE" == "darwin"* ]]; then
        sed -i '' 's/^const lv_font_t /lv_font_t /' "$OUTPUT"
    else
        sed -i 's/^const lv_font_t /lv_font_t /' "$OUTPUT"
    fi
done

# Generate Light weight
echo ""
echo "Light weight:"
for SIZE in $SIZES_LIGHT; do
    OUTPUT="assets/fonts/noto_sans_light_${SIZE}.c"
    echo "  Generating noto_sans_light_${SIZE} -> $OUTPUT"

    lv_font_conv \
        --font "$FONT_LIGHT" --size "$SIZE" --range "$UNICODE_RANGES" \
        --font "$FONT_CJK_SC" --size "$SIZE" --range "$WIZARD_CJK" \
        --font "$FONT_CJK_JP" --size "$SIZE" --range "$WIZARD_CJK" \
        --bpp 4 --format lvgl \
        --no-compress \
        -o "$OUTPUT"
    # Strip const so CjkFontManager can set fallback pointers at runtime
    if [[ "$OSTYPE" == "darwin"* ]]; then
        sed -i '' 's/^const lv_font_t /lv_font_t /' "$OUTPUT"
    else
        sed -i 's/^const lv_font_t /lv_font_t /' "$OUTPUT"
    fi
done

# Generate Bold weight
echo ""
echo "Bold weight:"
for SIZE in $SIZES_BOLD; do
    OUTPUT="assets/fonts/noto_sans_bold_${SIZE}.c"
    echo "  Generating noto_sans_bold_${SIZE} -> $OUTPUT"

    lv_font_conv \
        --font "$FONT_BOLD" --size "$SIZE" --range "$UNICODE_RANGES" \
        --font "$FONT_CJK_SC" --size "$SIZE" --range "$WIZARD_CJK" \
        --font "$FONT_CJK_JP" --size "$SIZE" --range "$WIZARD_CJK" \
        --bpp 4 --format lvgl \
        --no-compress \
        -o "$OUTPUT"
    # Strip const so CjkFontManager can set fallback pointers at runtime
    if [[ "$OSTYPE" == "darwin"* ]]; then
        sed -i '' 's/^const lv_font_t /lv_font_t /' "$OUTPUT"
    else
        sed -i 's/^const lv_font_t /lv_font_t /' "$OUTPUT"
    fi
done

# Generate Source Code Pro Monospace (for console/terminal)
FONT_MONO=assets/fonts/SourceCodePro-Regular.ttf
SIZES_MONO="8 10 12 14 16 18 20 24"

if [ -f "$FONT_MONO" ]; then
    echo ""
    echo "Source Code Pro (Monospace):"
    for SIZE in $SIZES_MONO; do
        OUTPUT="assets/fonts/source_code_pro_${SIZE}.c"
        echo "  Generating source_code_pro_${SIZE} -> $OUTPUT"

        lv_font_conv \
            --font "$FONT_MONO" --size "$SIZE" --bpp 4 --format lvgl \
            --range "$UNICODE_RANGES" \
            \
            -o "$OUTPUT"
        # De-const only source_code_pro_14: it is a MOVED face (ESP32 loads its
        # glyph data from a runtime .bin and struct-copies into the symbol at
        # boot; Plan A fonts->frogfs move), so ui_fonts.h declares it non-const.
        # The .c definition must match or gcc errors with "conflicting type
        # qualifiers". The other mono sizes are NOT moved and stay const.
        if [ "$SIZE" = "14" ]; then
            if [[ "$OSTYPE" == "darwin"* ]]; then
                sed -i '' 's/^const lv_font_t /lv_font_t /' "$OUTPUT"
            else
                sed -i 's/^const lv_font_t /lv_font_t /' "$OUTPUT"
            fi
        fi
    done
else
    echo ""
    echo "Source Code Pro not found ($FONT_MONO) - skipping monospace fonts"
    echo "Download from: https://github.com/adobe-fonts/source-code-pro/tree/release/TTF"
fi

# Generate CJK runtime .bin files (full character set, loaded at runtime)
if [ -n "$RUNTIME_CJKCHARS" ]; then
    echo ""
    echo "CJK runtime .bin files (full character set):"
    mkdir -p assets/fonts/cjk

    echo "  Regular weight:"
    for SIZE in $SIZES_REGULAR; do
        OUTPUT="assets/fonts/cjk/noto_sans_cjk_${SIZE}.bin"
        echo "    Generating noto_sans_cjk_${SIZE}.bin"
        lv_font_conv \
            --font "$FONT_CJK_SC" --size "$SIZE" --range "$RUNTIME_CJKCHARS" \
            --font "$FONT_CJK_JP" --size "$SIZE" --range "$RUNTIME_CJKCHARS" \
            --bpp 4 --format bin \
            \
            -o "$OUTPUT"
    done

    echo "  Bold weight:"
    for SIZE in $SIZES_BOLD; do
        OUTPUT="assets/fonts/cjk/noto_sans_cjk_bold_${SIZE}.bin"
        echo "    Generating noto_sans_cjk_bold_${SIZE}.bin"
        lv_font_conv \
            --font "$FONT_CJK_SC" --size "$SIZE" --range "$RUNTIME_CJKCHARS" \
            --font "$FONT_CJK_JP" --size "$SIZE" --range "$RUNTIME_CJKCHARS" \
            --bpp 4 --format bin \
            \
            -o "$OUTPUT"
    done

    echo "  Light weight:"
    for SIZE in $SIZES_LIGHT; do
        OUTPUT="assets/fonts/cjk/noto_sans_cjk_light_${SIZE}.bin"
        echo "    Generating noto_sans_cjk_light_${SIZE}.bin"
        lv_font_conv \
            --font "$FONT_CJK_SC" --size "$SIZE" --range "$RUNTIME_CJKCHARS" \
            --font "$FONT_CJK_JP" --size "$SIZE" --range "$RUNTIME_CJKCHARS" \
            --bpp 4 --format bin \
            \
            -o "$OUTPUT"
    done

    BIN_COUNT=$(ls -1 assets/fonts/cjk/*.bin 2>/dev/null | wc -l)
    echo "  Generated $BIN_COUNT .bin files"

    # Record the exact codepoint set baked into the runtime .bin files. This is
    # the source of truth for check_cjk_font_staleness.sh — it must compare a
    # translation's needed glyphs against what's actually in the .bin runtime
    # set, NOT against the .c fonts (which only carry the 12-codepoint wizard
    # subset and would otherwise report the whole CJK set as perpetually
    # "missing"). One "0xXXXX" per line, sorted, matching the format emitted by
    # the staleness check's char extractor.
    MANIFEST="assets/fonts/cjk/.cjk_codepoints.manifest"
    echo "$RUNTIME_CJKCHARS" | tr ',' '\n' | sort > "$MANIFEST"
    echo "  Wrote codepoint manifest ($RUNTIME_CJK_COUNT entries) -> $MANIFEST"
else
    echo ""
    echo "WARNING: No CJK characters extracted - skipping .bin generation"
fi

echo ""
echo "Done! Generated text fonts with extended Unicode support."
echo ""
echo "Supported character sets:"
echo "  - ASCII (0x20-0x7F)"
echo "  - Western European: é, è, ê, ñ, ü, ö, ß, etc."
echo "  - Central European: ą, ę, ł, ő, etc."
echo "  - Cyrillic: А-Яа-я (Russian, Ukrainian, etc.)"
echo "  - CJK wizard (compiled .c): 12 codepoints for welcome page"
echo "  - CJK runtime (.bin): Full Chinese + Japanese character set"
echo ""
echo "Rebuild the project: make -j"
