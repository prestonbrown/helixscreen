#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fail if the translations need a CJK codepoint the runtime font doesn't bake.
# Called after translation-sync (mk/translations.mk) and from quality-checks.
#
# Compares the CJK characters needed by the translations and C++ sources
# against the codepoint manifest regen_text_fonts.sh records for the runtime
# .bin fonts. A codepoint on the left and not the right renders as tofu in
# zh/ja, with no build error and no runtime warning — this gate is the only
# thing that says so.
#
# Both inputs are git-tracked files, so this runs in every clone and CI
# runner. The CJK source .otf fonts are deliberately NOT required: they are
# gitignored downloads needed only to RE-bake, and gating on them made every
# fresh checkout silently skip this check.

set -e
cd "$(dirname "$0")/.."

ROOT="."
if [ "$1" = "--root" ] && [ -n "$2" ]; then
    ROOT="$2"
fi

MANIFEST="$ROOT/assets/fonts/cjk/.cjk_codepoints.manifest"

if [ ! -f "$MANIFEST" ]; then
    echo "✗ CJK font manifest not found ($MANIFEST)"
    echo "  Run 'make regen-text-fonts' and commit assets/fonts/cjk/."
    exit 1
fi

# One 0xXXXX per line, sorted — same extractor the bake uses, so the two
# sides cannot drift (scripts/translations/cjk_charset.py).
NEEDED=$(python3 scripts/translations/cjk_charset.py --root "$ROOT")

if [ -z "$NEEDED" ] || ! grep -qE '0x[0-9a-f]+' "$MANIFEST"; then
    # An empty side here is a broken scan or a wiped manifest, not "no CJK".
    echo "✗ CJK needed-set or manifest is empty — refusing to pass vacuously."
    exit 1
fi

# Normalize manifest to the same "0xXXXX" form NEEDED uses (lowercase, no blanks)
COMPILED=$(grep -oE '0x[0-9a-fA-F]+' "$MANIFEST" | tr 'A-F' 'a-f' | sort -u)

MISSING=$(comm -23 <(echo "$NEEDED" | sort) <(echo "$COMPILED" | sort))
MISSING_COUNT=0
if [ -n "$MISSING" ]; then
    MISSING_COUNT=$(echo "$MISSING" | wc -l | tr -d ' ')
fi

if [ "$MISSING_COUNT" -gt 0 ]; then
    echo "✗ $MISSING_COUNT CJK codepoint(s) needed by translations/sources but missing from the baked font:"
    echo "$MISSING" | while read -r cp; do
        printf '  %s %s\n' "$cp" "$(python3 -c "import sys; print(chr(int(sys.argv[1], 16)))" "$cp")"
    done | head -30
    if [ "$MISSING_COUNT" -gt 30 ]; then
        echo "  ... and $((MISSING_COUNT - 30)) more"
    fi
    echo "Run 'make regen-text-fonts' to bake them, then rebuild and commit assets/fonts/cjk/."
    exit 1
fi

exit 0
