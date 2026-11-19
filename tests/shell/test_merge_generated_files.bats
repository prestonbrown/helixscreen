#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generated files under merge=helix-generated (.gitattributes): a merge of two
# branches that both regenerated them keeps one side's copy instead of
# conflicting, and scripts/check_translation_packs_fresh.py fails packs that no
# longer match the YAML in the index, after a merge, a rebase or a cherry-pick,
# until they are regenerated. No hook makes a commit. The fixture's generator
# is a stand-in that writes the sorted YAML lines into one pack, and the real
# hooks and checker run against it.

load helpers

HOOKS="$BATS_TEST_DIRNAME/../../.githooks"
CHECKER="$BATS_TEST_DIRNAME/../../scripts/check_translation_packs_fresh.py"

setup() {
    REPO="$BATS_TEST_TMPDIR/repo"
    git init -q -b main "$REPO"
    git -C "$REPO" config user.email t@t
    git -C "$REPO" config user.name t
    git -C "$REPO" config core.hooksPath "$HOOKS"
    git -C "$REPO" config merge.helix-generated.driver true
    mkdir -p "$REPO/scripts" "$REPO/translations" "$REPO/ui_xml/translations" "$REPO/assets/fonts/cjk"
    grep 'merge=helix-generated' "$BATS_TEST_DIRNAME/../../.gitattributes" >"$REPO/.gitattributes"
    cp "$CHECKER" "$REPO/scripts/"
    printf '#!/bin/sh\nexit 0\n' >"$REPO/scripts/quality-checks.sh"
    chmod +x "$REPO/scripts/quality-checks.sh"
    cat >"$REPO/scripts/generate_translations.py" <<'PY'
import argparse, pathlib
ap = argparse.ArgumentParser()
ap.add_argument("--yaml-dir", default="translations")
ap.add_argument("--xml-dir", default="ui_xml/translations")
a = ap.parse_args()
lines = sorted(l for f in sorted(pathlib.Path(a.yaml_dir).glob("*.yml")) for l in f.read_text().splitlines())
(pathlib.Path(a.xml_dir) / "en.xml").write_text("\n".join(lines) + "\n")
PY
    printf 'a: one\nm: middle\nz: last\n' >"$REPO/translations/en.yml"
    regen
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm base
}

# What make translations and make regen-text-fonts write, in the fixture. The bake
# is binary, as the real .bin faces are.
regen() {
    (cd "$REPO" && python3 scripts/generate_translations.py &&
        python3 -c 'import pathlib; p = pathlib.Path("ui_xml/translations/en.xml"); pathlib.Path("assets/fonts/cjk/bake.bin").write_bytes(b"\0" + p.read_bytes())')
}

branch_adding() { # <branch> <yaml line> <line it follows>
    git -C "$REPO" switch -q -c "$1" main
    sed -i "/^$3/a $2" "$REPO/translations/en.yml"
    regen
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm "$1"
}

fresh() { run bash -c 'cd "$1" && python3 scripts/check_translation_packs_fresh.py' _ "$REPO"; }

regen_and_commit() { # the fix the checker names, with what the merge or pick left to conclude
    regen
    git -C "$REPO" add ui_xml/translations assets/fonts/cjk
    git -C "$REPO" commit -q --no-edit -m "regenerate packs"
}

@test "a clean merge conflicts on no generated file and stops before its commit with the fix" {
    branch_adding left "b: added left" "a:"
    branch_adding right "n: added right" "m:"
    run git -C "$REPO" merge --no-edit left
    [ "$status" -ne 0 ]
    lacks "CONFLICT" "$output"
    contains "does not match translations/*.yml: en.xml (differs)" "$output"
    contains "make translations && git add ui_xml/translations && git commit" "$output"
    [ "$(git -C "$REPO" log -1 --format=%s)" = right ] || fail "the stale merge was committed"
    git -C "$REPO" rev-parse -q --verify MERGE_HEAD >/dev/null || fail "the merge state was dropped"
    regen_and_commit
    git -C "$REPO" rev-parse -q --verify HEAD^2 >/dev/null || fail "HEAD is not the merge commit"
    run git -C "$REPO" show HEAD:ui_xml/translations/en.xml
    contains "b: added left" "$output"
    contains "n: added right" "$output"
    fresh
    [ "$status" -eq 0 ]
}

@test "without the driver the same merge conflicts on the generated files" {
    git -C "$REPO" config --unset merge.helix-generated.driver
    branch_adding left "b: added left" "a:"
    branch_adding right "n: added right" "m:"
    run git -C "$REPO" merge --no-edit left
    [ "$status" -ne 0 ]
    contains "CONFLICT (content): Merge conflict in assets/fonts/cjk/bake.bin" "$output"
}

@test "a rebase that keeps one side's packs fails the check until they are regenerated" {
    branch_adding left "b: added left" "a:"
    branch_adding right "n: added right" "m:"
    run git -C "$REPO" rebase -q left
    [ "$status" -eq 0 ] || fail "$output"
    fresh
    [ "$status" -eq 1 ]
    contains "en.xml (differs)" "$output"
    regen_and_commit
    fresh
    [ "$status" -eq 0 ]
}

@test "a cherry-pick that keeps one side's packs fails the check until they are regenerated" {
    branch_adding left "b: added left" "a:"
    branch_adding right "n: added right" "m:"
    run git -C "$REPO" cherry-pick left
    [ "$status" -eq 0 ] || fail "$output"
    fresh
    [ "$status" -eq 1 ]
    regen_and_commit
    fresh
    [ "$status" -eq 0 ]
}

@test "the check reads the index, never the working tree" {
    echo "q: uncommitted" >>"$REPO/translations/en.yml"
    fresh
    [ "$status" -eq 0 ] || fail "uncommitted YAML failed it: $output"
    git -C "$REPO" add translations/en.yml
    fresh
    [ "$status" -eq 1 ]
    regen
    fresh
    [ "$status" -eq 1 ] || fail "an unstaged regenerated pack passed it"
    git -C "$REPO" add ui_xml/translations
    fresh
    [ "$status" -eq 0 ]
}

@test "a pack no locale generates is stale too" {
    echo "<x/>" >"$REPO/ui_xml/translations/xx.xml"
    git -C "$REPO" add ui_xml/translations/xx.xml
    fresh
    [ "$status" -eq 1 ]
    contains "xx.xml (no locale generates it)" "$output"
}

@test "no hook makes a commit: a merge leaves HEAD where git put it" {
    branch_adding left "b: added left" "a:"
    git -C "$REPO" switch -q main
    run git -C "$REPO" merge --no-edit left
    [ "$status" -eq 0 ]
    [ "$(git -C "$REPO" log -1 --format=%s)" = left ]
    [ -z "$(git -C "$REPO" status --porcelain)" ]
}

@test "the qc gate fails stale packs and triggers on the YAML, the packs and the generator" {
    echo "q: staged" >>"$REPO/translations/en.yml"
    git -C "$REPO" add translations/en.yml
    run bash -c '
        STAGED_ONLY=true; QC_TMP="$(mktemp -d)"
        . "$1/scripts/qc/_lib.sh"
        . "$1/scripts/qc/translation_packs_fresh.sh"
        cd "$2" && qc_translation_packs_fresh' _ "$BATS_TEST_DIRNAME/../.." "$REPO"
    [ "$status" -eq 1 ]
    contains "en.xml (differs)" "$output"
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    re="$(qc_trigger translation_packs_fresh)"
    for p in translations/de.yml ui_xml/translations/de.xml scripts/generate_translations.py \
        scripts/translations/locale_variants.py scripts/check_translation_packs_fresh.py; do
        echo "$p" | grep -qE "$re" || fail "$p should trigger"
    done
    if echo ui_xml/home_panel.xml | grep -qE "$re"; then fail "a layout should not trigger"; fi
}
