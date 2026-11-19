#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generated files under merge=helix-generated (.gitattributes): a merge of two
# branches that both regenerated them keeps one side's copy instead of
# conflicting, and the merge hooks (.githooks/lib/regen-generated.sh) rebuild
# the translation packs from the merged YAML. The fixture's generator is a
# stand-in that writes the sorted YAML lines into one pack, and the real hooks
# run against it.

load helpers

HOOKS="$BATS_TEST_DIRNAME/../../.githooks"

setup() {
    REPO="$BATS_TEST_TMPDIR/repo"
    git init -q -b main "$REPO"
    git -C "$REPO" config user.email t@t
    git -C "$REPO" config user.name t
    git -C "$REPO" config core.hooksPath "$HOOKS"
    git -C "$REPO" config merge.helix-generated.driver true
    mkdir -p "$REPO/scripts" "$REPO/translations" "$REPO/ui_xml/translations" "$REPO/assets/fonts/cjk"
    grep 'merge=helix-generated' "$BATS_TEST_DIRNAME/../../.gitattributes" >"$REPO/.gitattributes"
    printf '#!/bin/sh\nexit 0\n' >"$REPO/scripts/quality-checks.sh"
    chmod +x "$REPO/scripts/quality-checks.sh"
    printf 'import pathlib\nlines = sorted(l for f in sorted(pathlib.Path("translations").glob("*.yml")) for l in f.read_text().splitlines())\npathlib.Path("ui_xml/translations/en.xml").write_text("\\n".join(lines) + "\\n")\n' \
        >"$REPO/scripts/generate_translations.py"
    # The bake is binary, as the real .bin faces are, and stale when it is not
    # the sorted YAML, as a real one lacking a glyph is.
    printf 'import pathlib, sys\nlines = sorted(l for f in sorted(pathlib.Path("translations").glob("*.yml")) for l in f.read_text().splitlines())\nsys.exit(pathlib.Path("assets/fonts/cjk/bake.bin").read_bytes() != b"\\0" + "\\n".join(lines).encode())\n' \
        >"$REPO/scripts/bake_check.py"
    printf '#!/bin/sh\nexec python3 scripts/bake_check.py\n' >"$REPO/scripts/check_cjk_font_staleness.sh"
    printf 'a: one\nm: middle\nz: last\n' >"$REPO/translations/en.yml"
    regen
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm base
}

# What make translations and make regen-text-fonts write, in the fixture.
regen() {
    (cd "$REPO" && python3 scripts/generate_translations.py &&
        python3 -c 'import pathlib, sys; lines = sorted(l for f in sorted(pathlib.Path("translations").glob("*.yml")) for l in f.read_text().splitlines()); pathlib.Path("assets/fonts/cjk/bake.bin").write_bytes(b"\0" + "\n".join(lines).encode())')
}

branch_adding() { # <branch> <yaml line> <line it follows>
    git -C "$REPO" switch -q -c "$1" main
    sed -i "/^$3/a $2" "$REPO/translations/en.yml"
    regen
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm "$1"
}

@test "a clean merge keeps one side's generated files and commits regenerated packs after it" {
    branch_adding left "b: added left" "a:"
    branch_adding right "n: added right" "m:"
    run git -C "$REPO" merge --no-edit left
    [ "$status" -eq 0 ] || fail "the merge stopped: $output"
    lacks "CONFLICT" "$output"
    contains "committed ui_xml/translations/ regenerated" "$output"
    contains "the CJK font bake lacks glyphs" "$output"
    run git -C "$REPO" show HEAD:ui_xml/translations/en.xml
    contains "b: added left" "$output"
    contains "n: added right" "$output"
    [ "$(git -C "$REPO" log -1 --format=%s)" = "chore(i18n): regenerate translation packs after a merge" ]
    git -C "$REPO" rev-parse -q --verify HEAD^1^2 >/dev/null || fail "the pack commit does not follow the merge"
    [ -z "$(git -C "$REPO" status --porcelain)" ] || fail "the merge left the tree dirty"
}

@test "without the driver the same merge conflicts on the generated files" {
    git -C "$REPO" config --unset merge.helix-generated.driver
    branch_adding left "b: added left" "a:"
    branch_adding right "n: added right" "m:"
    run git -C "$REPO" merge --no-edit left
    [ "$status" -ne 0 ]
    contains "CONFLICT (content): Merge conflict in assets/fonts/cjk/bake.bin" "$output"
}

@test "a merge that stopped on a YAML conflict regenerates the packs into the merge commit" {
    branch_adding left "b: added left" "a:"
    branch_adding right "b: added right" "a:"
    run git -C "$REPO" merge --no-edit left
    [ "$status" -ne 0 ]
    [ "$(git -C "$REPO" diff --name-only --diff-filter=U)" = "translations/en.yml" ] ||
        fail "more than the YAML conflicted: $(git -C "$REPO" diff --name-only --diff-filter=U)"
    sed -i '/^<<<<<<< \|^=======$\|^>>>>>>> /d' "$REPO/translations/en.yml"
    git -C "$REPO" add translations/en.yml
    run git -C "$REPO" commit --no-edit
    [ "$status" -eq 0 ]
    contains "regenerated ui_xml/translations/" "$output"
    run git -C "$REPO" show HEAD:ui_xml/translations/en.xml
    contains "b: added left" "$output"
    contains "b: added right" "$output"
    git -C "$REPO" rev-parse -q --verify HEAD^2 >/dev/null || fail "HEAD is not the merge commit"
}

@test "a fast-forward runs no regeneration" {
    branch_adding left "b: added left" "a:"
    git -C "$REPO" switch -q main
    run git -C "$REPO" merge --no-edit left
    [ "$status" -eq 0 ]
    lacks "merge:" "$output"
    [ "$(git -C "$REPO" log -1 --format=%s)" = left ]
}
