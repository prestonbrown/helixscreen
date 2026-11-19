#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run by pre-merge-commit (a clean merge) and pre-commit (a merge concluded
# after a conflict), from the repo root, before the merge commit is written.
#
# The translation packs (ui_xml/translations/*.xml) and the CJK font bake
# (assets/fonts/cjk/) are generated from translations/*.yml. Two branches that
# both regenerated them would conflict on output nobody can resolve by hand, so
# .gitattributes merges them with the helix-generated driver, which keeps this
# side's copy. This rebuilds what that left stale:
#
#   translation packs  regenerated from the merged YAML (about 2 s) and staged,
#                      so the merge commit carries packs that match it.
#   CJK font bake      checked against the merged strings and, when it lacks a
#                      glyph, named with the command that rebakes it. A bake
#                      takes minutes and needs lv_font_conv and the CJK source
#                      fonts, so the merge does not wait for it; qc_cjk_fonts in
#                      make land-gate refuses a stale bake before it lands.
#
# A merge that touches none of these inputs does nothing here. Never fails the
# merge: a pack that cannot be regenerated is reported, and the push-time sweep
# and make land-gate check the packs against the YAML.
set -u

changed=$(git diff --cached --name-only HEAD -- translations/ ui_xml/translations/ \
    assets/fonts/cjk/ scripts/generate_translations.py 2>/dev/null)
[ -n "$changed" ] || exit 0

py=.venv/bin/python3
[ -x "$py" ] || py=python3
if "$py" -c 'import yaml' 2>/dev/null &&
    "$py" scripts/generate_translations.py >/dev/null 2>&1; then
    if ! git diff --quiet -- ui_xml/translations/; then
        git add -- ui_xml/translations/
        echo "🌐 merge: regenerated ui_xml/translations/ from the merged translations/*.yml"
    fi
else
    echo "⚠️  merge: could not regenerate ui_xml/translations/ (no python with yaml);"
    echo "   run 'make translations' and commit the packs before this lands."
fi

if [ -f scripts/check_cjk_font_staleness.sh ] &&
    ! bash scripts/check_cjk_font_staleness.sh >/dev/null 2>&1; then
    echo "⚠️  merge: the CJK font bake lacks glyphs the merged strings use. Before this"
    echo "   lands, run 'make regen-text-fonts' and commit assets/fonts/cjk/; make"
    echo "   land-gate refuses a stale bake."
fi
exit 0
