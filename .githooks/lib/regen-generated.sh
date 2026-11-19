#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: regen-generated.sh --staged | --merged
#
# The translation packs (ui_xml/translations/*.xml) and the CJK font bake
# (assets/fonts/cjk/) are generated from translations/*.yml. Two branches that
# both regenerated them would conflict on output nobody can resolve by hand, so
# .gitattributes merges them with the helix-generated driver, which keeps this
# side's copy. This rebuilds the packs that left stale, run from the repo root:
#
#   --staged  pre-commit, concluding a merge that stopped on a conflict. The
#             packs are generated from the YAML and generator in the INDEX and
#             written into the index, so the merge commit carries them.
#   --merged  post-merge, after a merge that committed cleanly, in a linked
#             worktree only. Git writes that commit from the tree it merged, not
#             from anything a hook stages, so the packs go into one follow-up
#             commit, generated from HEAD's YAML and generator and committed
#             with plumbing (git still counts the merge as in progress while
#             post-merge runs, and refuses a commit of named paths). The main
#             working tree is shared by every session, so it gets no automatic
#             commit: the hook names the command instead.
#
# Nothing is read from the working tree, so a session's uncommitted YAML never
# reaches a pack, and nothing but ui_xml/translations/ entries is written to any
# index. A pack file in the working tree is replaced only while it still holds
# exactly what the index held before; one with edits of its own is left alone
# and named.
#
# Either way the CJK font bake is checked against the merged strings and, when
# it lacks a glyph, the rebake is named. A bake takes minutes and needs
# lv_font_conv and the CJK source fonts, so the merge does not wait for it;
# qc_cjk_fonts in make land-gate refuses a stale bake before it lands.
#
# A merge that touches none of these inputs does nothing here, and nothing
# here fails the merge.
set -u

PACKS=ui_xml/translations
INPUTS=(translations/ "$PACKS/" assets/fonts/cjk/ scripts/generate_translations.py scripts/translations/)
mode=${1:-}
case "$mode" in
    --staged) changed=$(git diff --cached --name-only HEAD -- "${INPUTS[@]}" 2>/dev/null) ;;
    --merged)
        # A fast-forward or a squash made no merge commit, so nothing was kept
        # from one side.
        git rev-parse -q --verify HEAD^2 >/dev/null || exit 0
        changed=$(git diff --name-only HEAD^1 HEAD -- "${INPUTS[@]}" 2>/dev/null) ;;
    *) echo "usage: regen-generated.sh --staged | --merged" >&2; exit 2 ;;
esac
[ -n "$changed" ] || exit 0

main_tree() { [ "$(git rev-parse --git-dir)" = "$(git rev-parse --git-common-dir)" ]; }

stale_note() {
    echo "⚠️  merge: $PACKS/ does not match the merged translations/*.yml. Before this"
    echo "   lands, run 'make translations' and commit $PACKS/."
}

# generate SRC_DIR OUT_DIR: the packs SRC_DIR's translations/ and scripts/ make.
generate() {
    local py=.venv/bin/python3
    [ -x "$py" ] || py=python3
    "$py" -c 'import yaml' 2>/dev/null || return 1
    "$py" "$1/scripts/generate_translations.py" --yaml-dir "$1/translations" \
        --xml-dir "$2" >/dev/null 2>&1
}

# set_pack IDX PATH FILE OLD: point PATH at FILE's content in index IDX, and in
# the working tree when it still holds blob OLD. Prints PATH when its working
# copy has edits of its own and was left alone.
set_pack() {
    local idx=$1 path=$2 file=$3 old=$4 blob
    blob=$(git hash-object -w "$file") || return 1
    GIT_INDEX_FILE=$idx git update-index --add --cacheinfo "100644,$blob,$path" || return 1
    if [ ! -e "$path" ] || [ "$(git hash-object "$path")" = "$old" ]; then
        cp "$file" "$path"
    else
        printf '%s\n' "$path"
    fi
}

work=$(mktemp -d) || exit 0
# shellcheck disable=SC2064  # expand now: the path is fixed
trap "rm -rf '$work'" EXIT
mkdir -p "$work/src" "$work/out"

# The main working tree gets no automatic commit: only the note, when the
# packs HEAD carries are not the ones its YAML makes.
report_only=""
if [ "$mode" = --merged ] && main_tree; then report_only=1; fi

{
    if [ "$mode" = --staged ]; then
        git ls-files -z -- translations/ scripts/generate_translations.py scripts/translations/ |
            git checkout-index -z --stdin --prefix="$work/src/"
        base_index=$(git rev-parse --git-path index)
        [ -n "${GIT_INDEX_FILE:-}" ] && base_index=$GIT_INDEX_FILE
        rev=":"
    else
        # shellcheck disable=SC2046  # one tracked path per word
        git archive HEAD -- $(git ls-tree -r --name-only HEAD -- translations/ \
            scripts/generate_translations.py scripts/translations/) | tar -x -C "$work/src"
        rev="HEAD:"
    fi
    if ! generate "$work/src" "$work/out"; then
        stale_note
    else
        updated="" held="" idx=""
        if [ "$mode" = --merged ]; then
            idx="$work/index"
            GIT_INDEX_FILE=$idx git read-tree HEAD
        else
            idx=$base_index
        fi
        for f in "$work/out"/*.xml; do
            [ -f "$f" ] || continue
            path="$PACKS/$(basename "$f")"
            old=$(git rev-parse -q --verify "$rev$path" 2>/dev/null || true)
            [ -n "$old" ] && [ "$(git hash-object "$f")" = "$old" ] && continue
            if [ -n "$report_only" ]; then
                stale_note
                break
            fi
            if out=$(set_pack "$idx" "$path" "$f" "$old"); then
                updated+=" $path"
                [ -z "$out" ] || held+=" $out"
            fi
        done
        if [ -n "$updated" ] && [ "$mode" = --merged ]; then
            head=$(git rev-parse HEAD)
            tree=$(GIT_INDEX_FILE=$idx git write-tree) &&
                commit=$(git commit-tree "$tree" -p "$head" \
                    -m "chore(i18n): regenerate translation packs after a merge") &&
                git update-ref -m "post-merge: regenerate translation packs" HEAD "$commit" "$head" &&
                for p in $updated; do
                    git update-index --add --cacheinfo "100644,$(git rev-parse "HEAD:$p"),$p"
                done &&
                echo "🌐 merge: committed $PACKS/ regenerated from the merged translations/*.yml"
        elif [ -n "$updated" ]; then
            echo "🌐 merge: regenerated $PACKS/ into the merge commit from the staged translations/*.yml"
        fi
        if [ -n "$held" ]; then
            echo "⚠️  merge: left these working copies alone, they hold edits of their own:$held"
        fi
    fi
}

if [ -f scripts/check_cjk_font_staleness.sh ] &&
    ! bash scripts/check_cjk_font_staleness.sh >/dev/null 2>&1; then
    echo "⚠️  merge: the CJK font bake lacks glyphs the merged strings use. Before this"
    echo "   lands, run 'make regen-text-fonts' and commit assets/fonts/cjk/; make"
    echo "   land-gate refuses a stale bake."
fi
exit 0
