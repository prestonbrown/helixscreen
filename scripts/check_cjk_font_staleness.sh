#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Check if CJK text fonts need regeneration after translation changes.
# Called automatically after translation-sync.
#
# Compares the set of CJK characters in current translations against
# what's compiled into the font files. If new characters are found,
# warns the user to run 'make regen-text-fonts'.

set -e
cd "$(dirname "$0")/.."

CJK_FONT_SC=assets/fonts/NotoSansCJKsc-Regular.otf
CJK_FONT_JP=assets/fonts/NotoSansCJKjp-Regular.otf

# Skip check if CJK source fonts aren't available
if [ ! -f "$CJK_FONT_SC" ] || [ ! -f "$CJK_FONT_JP" ]; then
    exit 0
fi

# Extract current CJK characters needed from translations + sources
NEEDED=$(python3 << 'PYEOF'
import glob
import re

chars = set()
CJK_RANGES = [
    r'[\u3000-\u303f]', r'[\u3040-\u309f]', r'[\u30a0-\u30ff]',
    r'[\u3400-\u4dbf]', r'[\u4e00-\u9fff]', r'[\uff00-\uffef]',
]

def extract_cjk(content):
    found = set()
    for pattern in CJK_RANGES:
        found.update(re.findall(pattern, content))
    return found

for path in ['translations/zh.yml', 'translations/ja.yml']:
    try:
        with open(path, 'r') as f:
            chars.update(extract_cjk(f.read()))
    except FileNotFoundError:
        pass

for pattern in ['src/**/*.cpp', 'src/**/*.h', 'include/**/*.h']:
    for path in glob.glob(pattern, recursive=True):
        try:
            with open(path, 'r') as f:
                chars.update(extract_cjk(f.read()))
        except (FileNotFoundError, UnicodeDecodeError):
            pass

for c in sorted(chars):
    print(f'0x{ord(c):04x}')
PYEOF
)

# Compare against the codepoint manifest written by regen_text_fonts.sh, which
# records exactly the glyph set baked into the runtime CJK .bin files
# (assets/fonts/cjk/*.bin). The CJK runtime is those .bin files — NOT the .c
# fonts, which only carry the 12-codepoint wizard subset. Checking the .c here
# (the old behavior) reported the entire CJK set as perpetually "missing".
MANIFEST=assets/fonts/cjk/.cjk_codepoints.manifest
if [ ! -f "$MANIFEST" ]; then
    echo "⚠ CJK font manifest not found ($MANIFEST) - run 'make regen-text-fonts'"
    exit 0
fi

# Normalize manifest to the same "0xXXXX" form NEEDED uses (lowercase, no blanks)
COMPILED=$(grep -oE '0x[0-9a-fA-F]+' "$MANIFEST" | tr 'A-F' 'a-f' | sort -u)

# Find characters needed but not compiled
MISSING=$(comm -23 <(echo "$NEEDED" | sort) <(echo "$COMPILED" | sort))
MISSING_COUNT=0
if [ -n "$MISSING" ]; then
    MISSING_COUNT=$(echo "$MISSING" | wc -l | tr -d ' ')
fi

if [ "$MISSING_COUNT" -gt 0 ]; then
    echo ""
    echo "⚠ $MISSING_COUNT new CJK characters found in translations that aren't in fonts."
    echo "  Run 'make regen-text-fonts' to include them, then rebuild."
    echo ""
fi
