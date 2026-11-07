#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/apply_submodule_patch.sh - the single apply verdict
# shared by every stanza in mk/patches.mk.
#
# The verdict must be three-way and honest: a patch either applies, is
# recognized as already applied by a reverse check, or is refused. An
# else-branch that prints "already applied" whenever `git apply --check`
# fails turns a dead patch into a green line, because the same non-zero exit
# covers "already applied" and "will never apply again". Only a from-clean
# run (HELIX_PATCHES_FROM_CLEAN=1, set by `make reapply-patches`) can judge
# the third case: there nothing is applied yet, so every patch must take its
# apply branch. Incremental runs warn instead of failing, because a sibling
# patch earlier in the same recipe may legitimately have moved the context a
# shared-file patch needs.
#
# The flag is a claim about the tree, and the claim is verified, not assumed:
# on a non-pristine checkout a healthy shared-file patch reads "neither" too,
# so there the fatal downgrades to the warn branch.

load helpers

HELPER="scripts/apply_submodule_patch.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1

    # make exports HELIX_FROM_CLEAN_SENTINEL into its recipe shells, and bats
    # runs inside one, so the real tree's build/.patches-from-clean would
    # otherwise leak into these fixture verdicts. A test that exercises the
    # sentinel sets its own explicitly.
    unset HELIX_FROM_CLEAN_SENTINEL
    # CI exports the from-clean flag at the workflow level, reaching bats the
    # same way: without this unset, every warn-branch test below would take the
    # fatal branch there. Tests that want the flag set pass it per run.
    unset HELIX_PATCHES_FROM_CLEAN

    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    SUB="$ROOT/lib/fake"
    mkdir -p "$ROOT/patches" "$SUB"

    git -C "$SUB" init -q
    git -C "$SUB" config user.email t@example.invalid
    git -C "$SUB" config user.name "Fixture"
    printf 'one\n' > "$SUB/one.txt"
    git -C "$SUB" add one.txt
    git -C "$SUB" commit -qm pristine

    # good.patch matches the pristine file.
    printf 'one\nALPHA\n' > "$SUB/one.txt"
    git -C "$SUB" diff -- one.txt > "$ROOT/patches/good.patch"
    git -C "$SUB" checkout -- one.txt

    # dead.patch matches no state the submodule can ever reach: neither a
    # forward nor a reverse check can pass.
    cat > "$ROOT/patches/dead.patch" <<'EOF'
diff --git a/one.txt b/one.txt
--- a/one.txt
+++ b/one.txt
@@ -1 +1 @@
-drifted-base
+drifted-new
EOF
}

@test "applies a patch that matches the submodule" {
    run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch applied' <<<"$output"
    grep -q '^ALPHA$' "$SUB/one.txt"
}

@test "recognizes an already-applied patch through the reverse check" {
    git -C "$SUB" apply "$ROOT/patches/good.patch"
    run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch already applied' <<<"$output"
    # The reverse-apply branch must leave the tree alone.
    [ "$(git -C "$SUB" diff -- one.txt | grep -c '^+ALPHA$')" -eq 1 ]
}

@test "a dead patch warns but does not fail an incremental run" {
    run "$HELPER" "$SUB" "$ROOT/patches/dead.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'not verifiable in place' <<<"$output"
    # The warning must not wear a success checkmark: a green glyph here is
    # how a dead patch hides.
    ! grep -q '✓.*fixture patch' <<<"$output"
}

@test "a neither-branch patch whose marker is present reads as applied" {
    # A sibling moved the context git compares, but the line the patch adds
    # is in the file: this is the routine healthy state of a shared-file
    # patch on a patched tree, and it must not print the warn a dead patch
    # prints - operators would learn to skip it.
    printf 'sha256\tpatch\tkind\tdir\tfile\tmarker\tlabel\tnote\n' > "$ROOT/markers.tsv"
    printf 'x\tgood.patch\t+\tLVGL_DIR\tone.txt\tALPHA\tfixture patch\t\n' >> "$ROOT/markers.tsv"
    printf 'one-moved-by-sibling\nALPHA\n' > "$SUB/one.txt"
    HELIX_PATCH_MARKERS_TSV="$ROOT/markers.tsv" HELIX_MARKER_LVGL_DIR="$SUB" \
        run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'already applied' <<<"$output"
    grep -q 'marker present' <<<"$output"
    ! grep -q '⚠' <<<"$output"
}

@test "a neither-branch patch whose marker is absent says the effect is missing" {
    # Same unreadable context, but the patch's line is not in the file: the
    # warn must name what is wrong rather than hedge.
    printf 'sha256\tpatch\tkind\tdir\tfile\tmarker\tlabel\tnote\n' > "$ROOT/markers.tsv"
    printf 'x\tgood.patch\t+\tLVGL_DIR\tone.txt\tALPHA\tfixture patch\t\n' >> "$ROOT/markers.tsv"
    printf 'one-moved-by-sibling\n' > "$SUB/one.txt"
    HELIX_PATCH_MARKERS_TSV="$ROOT/markers.tsv" HELIX_MARKER_LVGL_DIR="$SUB" \
        run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q "marker is absent" <<<"$output"
    grep -q 'reapply-patches' <<<"$output"
    ! grep -q '✓' <<<"$output"
}

@test "a dead patch fails a from-clean run" {
    HELIX_PATCHES_FROM_CLEAN=1 run "$HELPER" "$SUB" "$ROOT/patches/dead.patch" "fixture patch"
    [ "$status" -eq 1 ]
    grep -q 'does not apply to a clean checkout' <<<"$output"
    grep -q 'Regenerate it' <<<"$output"
    # The fatal must name the other outcome a from-clean failure hides: a
    # patch every file of which a later patch owns is deleted, not regenerated.
    grep -q 'superseded' <<<"$output"
}

@test "a matching patch passes a from-clean run" {
    HELIX_PATCHES_FROM_CLEAN=1 run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch applied' <<<"$output"
}

@test "a dead patch on a non-pristine tree only warns, even under the flag" {
    # The flag asserts the run started from pristine submodules; a checkout
    # that cannot back that claim does not get the fatal. This is the state
    # `make clean` leaves behind: stamp gone, submodules still patched.
    git -C "$SUB" apply "$ROOT/patches/good.patch"
    HELIX_PATCHES_FROM_CLEAN=1 run "$HELPER" "$SUB" "$ROOT/patches/dead.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'not verifiable in place' <<<"$output"
}

@test "dirt outside the patch's own files does not downgrade the fatal" {
    # The standalone from-clean check is scoped to the files named by the
    # patch's diff headers, the way the recipe guard scopes to its explicit
    # file list: an untracked file elsewhere in the submodule is not evidence
    # about this patch, and must not buy a dead patch the warn branch.
    printf 'scratch\n' > "$SUB/unrelated.txt"
    HELIX_PATCHES_FROM_CLEAN=1 run "$HELPER" "$SUB" "$ROOT/patches/dead.patch" "fixture patch"
    [ "$status" -eq 1 ]
    grep -q 'does not apply to a clean checkout' <<<"$output"
}

@test "a make run's sentinel settles the claim, not the tree's current state" {
    # mk/patches.mk judges the claim once at recipe start - before any stanza
    # has dirtied a shared file - and writes the answer where every stanza
    # reads it back. The sentinel is authoritative in both directions: dirt
    # from earlier stanzas must not read as "not pristine", and a "0" must
    # keep the fatal holstered even on a tree that looks pristine now.
    git -C "$SUB" apply "$ROOT/patches/good.patch"
    printf 1 > "$ROOT/sentinel"
    HELIX_PATCHES_FROM_CLEAN=1 HELIX_FROM_CLEAN_SENTINEL="$ROOT/sentinel" \
        run "$HELPER" "$SUB" "$ROOT/patches/dead.patch" "fixture patch"
    [ "$status" -eq 1 ]
    grep -q 'does not apply to a clean checkout' <<<"$output"

    printf 0 > "$ROOT/sentinel"
    HELIX_PATCHES_FROM_CLEAN=1 HELIX_FROM_CLEAN_SENTINEL="$ROOT/sentinel" \
        run "$HELPER" "$SUB" "$ROOT/patches/dead.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'not verifiable in place' <<<"$output"
}

@test "mk/patches.mk writes the sentinel from the tree's actual state" {
    # The sentinel is only honest if the recipe guard writes it before the
    # first stanza runs: the variable, the export, and the pristine check
    # over the patched files.
    grep -qF 'HELIX_FROM_CLEAN_SENTINEL := $(BUILD_DIR)/.patches-from-clean' mk/patches.mk
    grep -qF 'export HELIX_FROM_CLEAN_SENTINEL' mk/patches.mk
    grep -qF 'status --porcelain -- $$files' mk/patches.mk
}

@test "a hostile git environment cannot bend the verdict" {
    # A pre-commit hook exports GIT_DIR/GIT_WORK_TREE/GIT_INDEX_FILE for the
    # superproject; if those leak into the helper's git calls, every question
    # about the submodule is answered by the wrong repository. The decoy is
    # kept dirty so the from-clean check would read "not pristine" from it
    # and wrongly holster the fatal on a pristine submodule.
    git init -q "$ROOT/decoy"
    printf 'decoy\n' > "$ROOT/decoy/one.txt"
    git -C "$ROOT/decoy" add one.txt
    git -C "$ROOT/decoy" -c user.email=t@example.invalid -c user.name=Fixture commit -qm decoy
    printf 'dirty\n' > "$ROOT/decoy/one.txt"

    GIT_DIR="$ROOT/decoy/.git" GIT_WORK_TREE="$ROOT/decoy" \
        GIT_INDEX_FILE="$ROOT/decoy/.git/index" HELIX_PATCHES_FROM_CLEAN=1 \
        run "$HELPER" "$SUB" "$ROOT/patches/dead.patch" "fixture patch"
    [ "$status" -eq 1 ]
    grep -q 'does not apply to a clean checkout' <<<"$output"

    GIT_DIR="$ROOT/decoy/.git" GIT_WORK_TREE="$ROOT/decoy" \
        GIT_INDEX_FILE="$ROOT/decoy/.git/index" HELIX_PATCHES_FROM_CLEAN=1 \
        run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch applied' <<<"$output"
    # The patch landed in the submodule; the decoy's dirty file is untouched.
    grep -q '^ALPHA$' "$SUB/one.txt"
    [ "$(cat "$ROOT/decoy/one.txt")" = "dirty" ]
}

@test "redirected output carries no ANSI escapes" {
    run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    ! grep -q $'\033' <<<"$output"
}

@test "mk/patches.mk routes every submodule stanza through the helper" {
    # The helper owns the apply verdict; a stanza that runs `git apply`
    # directly restores the per-stanza else-branch the helper replaced, where
    # an unchanged grep marker blesses hunks the patch gained since. Holds
    # for both submodules. The pattern is $'...'-quoted: grep reads a bare
    # \t as the letter t, which would make this assertion unable to fail.
    [ "$(grep -cE 'APPLY_PATCH\) \$\((LVGL|LIBHV|LUA)_DIR\)' mk/patches.mk)" -gt 50 ]
    ! grep -qE $'^\t+git -C \$\(LVGL_DIR\) apply' mk/patches.mk
    ! grep -qE $'^\t+git -C \$\(LIBHV_DIR\) apply' mk/patches.mk
    ! grep -qE $'^\t+git -C \$\(LUA_DIR\) apply' mk/patches.mk
}

@test "reapply-patches judges from clean" {
    # The from-clean verdict only binds when the flag actually reaches the
    # recipe shells: the sub-make must pass it and the variable must be
    # exported.
    grep -q 'force-apply-patches HELIX_PATCHES_FROM_CLEAN=1' mk/patches.mk
    grep -q '^export HELIX_PATCHES_FROM_CLEAN' mk/patches.mk
}

# A source tarball unpacks the submodules as plain directories. The fixture
# submodule loses its .git, and the superproject keeps or loses its own.
untar_fixture() {
    rm -rf "$SUB/.git"
    if [ "${1:-}" = "in-superproject" ]; then
        git -C "$ROOT" init -q
    fi
}

@test "a submodule with no .git inside a git superproject is patched, not skipped" {
    # git answers from the nearest enclosing repository, where every path in
    # the patch is outside the submodule directory: `git apply` then skips
    # them all and exits 0, which would print "applied" over an untouched file.
    untar_fixture in-superproject
    run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch applied' <<<"$output"
    [ "$(grep -c '^ALPHA$' "$SUB/one.txt")" -eq 1 ]

    run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch already applied' <<<"$output"
    [ "$(grep -c '^ALPHA$' "$SUB/one.txt")" -eq 1 ]
}

@test "a submodule with no repository anywhere is patched once" {
    untar_fixture
    GIT_CEILING_DIRECTORIES="$(dirname "$ROOT")" \
        run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch applied' <<<"$output"
    GIT_CEILING_DIRECTORIES="$(dirname "$ROOT")" \
        run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch already applied' <<<"$output"
    [ "$(grep -c '^ALPHA$' "$SUB/one.txt")" -eq 1 ]
}

# A PATH holding every tool the helper uses except git.
path_without_git() {
    local bin="$BATS_TEST_TMPDIR/nogit-bin" tool
    mkdir -p "$bin"
    for tool in bash env sed sort grep cat dirname basename python3 patch; do
        ln -sf "$(command -v "$tool")" "$bin/$tool"
    done
    printf '%s' "$bin"
}

@test "without git installed the patch still applies, once" {
    untar_fixture
    local nogit
    nogit="$(path_without_git)"
    PATH="$nogit" run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch applied' <<<"$output"
    PATH="$nogit" run "$HELPER" "$SUB" "$ROOT/patches/good.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'fixture patch already applied' <<<"$output"
    [ "$(grep -c '^ALPHA$' "$SUB/one.txt")" -eq 1 ]
    # No backup or reject files beside the patched one.
    [ "$(ls "$SUB")" = "one.txt" ]
}

@test "without git a dead patch still reads as neither, never applied" {
    untar_fixture
    PATH="$(path_without_git)" run "$HELPER" "$SUB" "$ROOT/patches/dead.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'not verifiable in place' <<<"$output"
    ! grep -q '✓.*fixture patch' <<<"$output"
    [ "$(cat "$SUB/one.txt")" = "one" ]
}

@test "a submodule with no .git cannot back the from-clean claim" {
    # Nothing can show such a tree is pristine, so the fatal stays holstered
    # even where the enclosing repository would call the files clean.
    untar_fixture in-superproject
    git -C "$ROOT" add lib/fake/one.txt
    git -C "$ROOT" -c user.email=t@example.invalid -c user.name=Fixture commit -qm vendored
    HELIX_PATCHES_FROM_CLEAN=1 run "$HELPER" "$SUB" "$ROOT/patches/dead.patch" "fixture patch"
    [ "$status" -eq 0 ]
    grep -q 'not verifiable in place' <<<"$output"
}
