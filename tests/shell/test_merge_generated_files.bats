#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generated files under merge=helix-generated (.gitattributes): a merge of two
# branches that both regenerated them keeps one side's copy instead of
# conflicting, and the merge hooks (.githooks/lib/regen-generated.sh) rebuild
# the translation packs from the merged YAML: never from the working tree, and
# never as an automatic commit in the main working tree. The fixture's
# generator is a stand-in that writes the sorted YAML lines into one pack, and
# the real hooks run against it.

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
    cat >"$REPO/scripts/generate_translations.py" <<'PY'
import argparse, pathlib
ap = argparse.ArgumentParser()
ap.add_argument("--yaml-dir", default="translations")
ap.add_argument("--xml-dir", default="ui_xml/translations")
a = ap.parse_args()
lines = sorted(l for f in sorted(pathlib.Path(a.yaml_dir).glob("*.yml")) for l in f.read_text().splitlines())
out = pathlib.Path(a.xml_dir)
out.mkdir(parents=True, exist_ok=True)
(out / "en.xml").write_text("\n".join(lines) + "\n")
PY
    # The bake is binary, as the real .bin faces are, and stale when it is not
    # the sorted YAML, as a real one lacking a glyph is.
    cat >"$REPO/scripts/bake.py" <<'PY'
import pathlib, sys
lines = sorted(l for f in sorted(pathlib.Path("translations").glob("*.yml")) for l in f.read_text().splitlines())
want = b"\0" + "\n".join(lines).encode()
bake = pathlib.Path("assets/fonts/cjk/bake.bin")
if sys.argv[1:] == ["--write"]:
    bake.write_bytes(want)
sys.exit(bake.read_bytes() != want)
PY
    printf '#!/bin/sh\nexec python3 scripts/bake.py\n' >"$REPO/scripts/check_cjk_font_staleness.sh"
    printf 'a: one\nm: middle\nz: last\n' >"$REPO/translations/en.yml"
    printf 'c: de\n' >"$REPO/translations/de.yml"
    echo notes >"$REPO/notes.txt"
    regen "$REPO"
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm base
    TREE="$BATS_TEST_TMPDIR/linked"
}

# What make translations and make regen-text-fonts write, in the fixture.
regen() { # <tree>
    (cd "$1" && python3 scripts/generate_translations.py && python3 scripts/bake.py --write) || true
}

branch_adding() { # <tree> <branch> <yaml line> <line it follows>
    git -C "$1" switch -q -c "$2" main
    sed -i "/^$4/a $3" "$1/translations/en.yml"
    regen "$1"
    git -C "$1" add -A
    git -C "$1" commit -qm "$2"
}

two_branches_in_linked_tree() {
    git -C "$REPO" worktree add -q --detach "$TREE" main
    branch_adding "$TREE" left "b: added left" "a:"
    branch_adding "$TREE" right "n: added right" "m:"
}

@test "a clean merge in a linked worktree keeps one side's files and commits regenerated packs after it" {
    two_branches_in_linked_tree
    run git -C "$TREE" merge --no-edit left
    [ "$status" -eq 0 ] || fail "the merge stopped: $output"
    lacks "CONFLICT" "$output"
    contains "committed ui_xml/translations/ regenerated" "$output"
    contains "the CJK font bake lacks glyphs" "$output"
    run git -C "$TREE" show HEAD:ui_xml/translations/en.xml
    contains "b: added left" "$output"
    contains "n: added right" "$output"
    [ "$(git -C "$TREE" log -1 --format=%s)" = "chore(i18n): regenerate translation packs after a merge" ]
    git -C "$TREE" rev-parse -q --verify HEAD^1^2 >/dev/null || fail "the pack commit does not follow the merge"
    [ -z "$(git -C "$TREE" status --porcelain)" ] || fail "the merge left the tree dirty: $(git -C "$TREE" status --porcelain)"
}

@test "the follow-up commit holds only packs from committed YAML, and leaves other work uncommitted" {
    two_branches_in_linked_tree
    echo "q: uncommitted" >>"$TREE/translations/de.yml"
    echo "wip" >>"$TREE/notes.txt"
    echo "scratch" >"$TREE/scratch.txt"
    run git -C "$TREE" merge --no-edit left
    [ "$status" -eq 0 ] || fail "$output"
    [ "$(git -C "$TREE" show --name-only --format= HEAD)" = "ui_xml/translations/en.xml" ]
    run git -C "$TREE" show HEAD:ui_xml/translations/en.xml
    lacks "q: uncommitted" "$output"
    contains "n: added right" "$output"
    run git -C "$TREE" status --porcelain
    [ "$output" = "$(printf ' M notes.txt\n M translations/de.yml\n?? scratch.txt')" ] || fail "$output"
}

@test "a pack with edits of its own keeps them, and the commit still lands" {
    two_branches_in_linked_tree
    echo "hand edit" >>"$TREE/ui_xml/translations/en.xml"
    run git -C "$TREE" merge --no-edit left
    [ "$status" -eq 0 ] || fail "$output"
    contains "left these working copies alone" "$output"
    grep -q "hand edit" "$TREE/ui_xml/translations/en.xml"
    run git -C "$TREE" show HEAD:ui_xml/translations/en.xml
    lacks "hand edit" "$output"
    contains "n: added right" "$output"
}

@test "the main working tree gets no automatic commit, only the command to run" {
    branch_adding "$REPO" left "b: added left" "a:"
    branch_adding "$REPO" right "n: added right" "m:"
    run git -C "$REPO" merge --no-edit left
    [ "$status" -eq 0 ] || fail "$output"
    contains "run 'make translations' and commit ui_xml/translations/" "$output"
    git -C "$REPO" rev-parse -q --verify HEAD^2 >/dev/null || fail "HEAD is not the merge commit"
    [ -z "$(git -C "$REPO" status --porcelain)" ] || fail "the main tree was touched"
}

@test "without the driver the same merge conflicts on the generated files" {
    git -C "$REPO" config --unset merge.helix-generated.driver
    two_branches_in_linked_tree
    run git -C "$TREE" merge --no-edit left
    [ "$status" -ne 0 ]
    contains "CONFLICT (content): Merge conflict in assets/fonts/cjk/bake.bin" "$output"
}

@test "a merge that stopped on a YAML conflict regenerates the packs into the merge commit from the index" {
    git -C "$REPO" worktree add -q --detach "$TREE" main
    branch_adding "$TREE" left "b: added left" "a:"
    branch_adding "$TREE" right "b: added right" "a:"
    run git -C "$TREE" merge --no-edit left
    [ "$status" -ne 0 ]
    [ "$(git -C "$TREE" diff --name-only --diff-filter=U)" = "translations/en.yml" ] ||
        fail "more than the YAML conflicted: $(git -C "$TREE" diff --name-only --diff-filter=U)"
    sed -i '/^<<<<<<< \|^=======$\|^>>>>>>> /d' "$TREE/translations/en.yml"
    git -C "$TREE" add translations/en.yml
    echo "q: unstaged" >>"$TREE/translations/en.yml"
    run git -C "$TREE" commit --no-edit
    [ "$status" -eq 0 ] || fail "$output"
    contains "regenerated ui_xml/translations/ into the merge commit" "$output"
    run git -C "$TREE" show HEAD:ui_xml/translations/en.xml
    contains "b: added left" "$output"
    contains "b: added right" "$output"
    lacks "q: unstaged" "$output"
    git -C "$TREE" rev-parse -q --verify HEAD^2 >/dev/null || fail "HEAD is not the merge commit"
}

@test "a fast-forward runs no regeneration" {
    git -C "$REPO" worktree add -q --detach "$TREE" main
    branch_adding "$TREE" left "b: added left" "a:"
    git -C "$TREE" switch -q main 2>/dev/null || git -C "$TREE" checkout -q --detach main
    run git -C "$TREE" merge --no-edit left
    [ "$status" -eq 0 ]
    lacks "merge:" "$output"
    [ "$(git -C "$TREE" log -1 --format=%s)" = left ]
}
