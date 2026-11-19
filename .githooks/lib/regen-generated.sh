#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: regen-generated.sh --staged | --merged
#
# The translation packs (ui_xml/translations/*.xml) and the CJK font bake
# (assets/fonts/cjk/) are generated from translations/*.yml. Two branches that
# both regenerated them would conflict on output nobody can resolve by hand, so
# .gitattributes merges them with the helix-generated driver, which keeps this
# side's copy. This rebuilds what that left stale, run from the repo root:
#
#   --staged  pre-commit, concluding a merge that stopped on a conflict: the
#             packs are regenerated from the staged YAML and staged, so the
#             merge commit carries them.
#   --merged  post-merge, after a merge that committed cleanly. Git writes that
#             commit from the tree it merged, not from anything a hook stages,
#             so packs that changed go into one follow-up commit of their own.
#
# Either way the CJK font bake is checked against the merged strings and, when
# it lacks a glyph, the rebake is named. A bake takes minutes and needs
# lv_font_conv and the CJK source fonts, so the merge does not wait for it;
# qc_cjk_fonts in make land-gate refuses a stale bake before it lands.
#
# A merge that touches none of these inputs does nothing here, and nothing
# here fails the merge.
set -u

INPUTS=(translations/ ui_xml/translations/ assets/fonts/cjk/ scripts/generate_translations.py)
case "${1:-}" in
    --staged) changed=$(git diff --cached --name-only HEAD -- "${INPUTS[@]}" 2>/dev/null) ;;
    --merged)
        # A fast-forward or a squash made no merge commit, so nothing was kept
        # from one side.
        git rev-parse -q --verify HEAD^2 >/dev/null || exit 0
        changed=$(git diff --name-only HEAD^1 HEAD -- "${INPUTS[@]}" 2>/dev/null) ;;
    *) echo "usage: regen-generated.sh --staged | --merged" >&2; exit 2 ;;
esac
[ -n "$changed" ] || exit 0

py=.venv/bin/python3
[ -x "$py" ] || py=python3
if "$py" -c 'import yaml' 2>/dev/null &&
    "$py" scripts/generate_translations.py >/dev/null 2>&1; then
    if ! git diff --quiet -- ui_xml/translations/; then
        if [ "$1" = --staged ]; then
            git add -- ui_xml/translations/
            echo "🌐 merge: regenerated ui_xml/translations/ from the merged translations/*.yml"
        elif HELIX_QC_MERGE_REGEN=1 git commit -q \
            -m "chore(i18n): regenerate translation packs after a merge" \
            -- ui_xml/translations/; then
            echo "🌐 merge: committed ui_xml/translations/ regenerated from the merged translations/*.yml"
        else
            echo "⚠️  merge: ui_xml/translations/ is regenerated in the working tree but its"
            echo "   commit failed (above); commit it before this lands."
        fi
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
