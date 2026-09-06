#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_patch_drift.py - the gate that fails when the
# patches applied into a vendored submodule are not the patches now sitting in
# patches/.
#
# The failure mode it exists for: mk/patches.mk guards every apply with "is this
# file dirty?", never with "is it dirty with THIS version of the patch". So once
# any revision of a patch is applied, editing that patch is a no-op for every
# checkout that already carries the old one. 86560d156 added
# lv_evdev_get_last_raw() to patches/lvgl-evdev-protocol-a.patch, main's
# lib/lvgl kept the previous revision, and every device cross-build failed with
# "not declared in this scope" while the whole desktop suite stayed green -
# `make test` skips patch application and lv_evdev.c is compiled out of desktop
# builds, so no existing gate could see it.
#
# Every test runs against a throwaway fixture repo under BATS_TEST_TMPDIR. None
# of them touch lib/ in this checkout: those are shared submodule checkouts.

GATE="scripts/check_patch_drift.py"

# Build a miniature repo shaped like this one: a Makefile that names a submodule
# directory, an mk/patches.mk whose apply lines bind patches to it, a patches/
# directory, and the submodule itself as a real git repo.
setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1

    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    SUB="$ROOT/lib/fake"
    mkdir -p "$ROOT/patches" "$ROOT/mk" "$SUB"

    git -C "$SUB" init -q
    git -C "$SUB" config user.email t@example.invalid
    git -C "$SUB" config user.name "Fixture"
    printf 'one\n' > "$SUB/one.txt"
    printf 'two\n' > "$SUB/two.txt"
    git -C "$SUB" add one.txt two.txt
    git -C "$SUB" commit -qm pristine

    # alpha.patch / beta.patch modify a tracked file each; gamma.patch CREATES
    # one, which is the case a plain `git diff` dirty check cannot see.
    printf 'one\nALPHA\n' > "$SUB/one.txt"
    git -C "$SUB" diff -- one.txt > "$ROOT/patches/alpha.patch"
    git -C "$SUB" checkout -- one.txt

    printf 'two\nBETA\n' > "$SUB/two.txt"
    git -C "$SUB" diff -- two.txt > "$ROOT/patches/beta.patch"
    git -C "$SUB" checkout -- two.txt

    printf 'three\n' > "$SUB/three.txt"
    git -C "$SUB" add -N three.txt
    git -C "$SUB" diff -- three.txt > "$ROOT/patches/gamma.patch"
    git -C "$SUB" reset -q -- three.txt
    rm -f "$SUB/three.txt"

    # An exempt patch: on the shelf, deliberately applied by nothing.
    printf 'exempt, applied by nothing\n' > "$ROOT/patches/zeta.patch"

    cat > "$ROOT/Makefile" <<'MAKE_EOF'
FAKE_DIR := lib/fake
MAKE_EOF

    write_patches_mk alpha beta gamma
}

# Regenerate mk/patches.mk with an apply line per named patch, in the exact
# shape the real one uses.
write_patches_mk() {
    {
        echo 'PATCH_EXEMPT := zeta.patch'
        echo 'PATCH_DIR := $(abspath patches)'
        for name in "$@"; do
            printf '\t$(Q)if git -C $(FAKE_DIR) apply --check $(PATCH_DIR)/%s.patch 2>/dev/null; then \\\n' "$name"
            printf '\t\tgit -C $(FAKE_DIR) apply $(PATCH_DIR)/%s.patch; \\\n' "$name"
            printf '\tfi\n'
        done
    } > "$ROOT/mk/patches.mk"
}

apply_all() {
    git -C "$SUB" apply "$ROOT/patches/alpha.patch"
    git -C "$SUB" apply "$ROOT/patches/beta.patch"
    git -C "$SUB" apply "$ROOT/patches/gamma.patch"
}

write_stamp() {
    run python3 "$GATE" --repo-root "$ROOT" --write-stamp
    [ "$status" -eq 0 ]
}

run_gate() {
    run python3 "$GATE" --repo-root "$ROOT" "$@"
}

# Put the fixture in the normal steady state: patches applied, stamp written.
in_sync() {
    apply_all
    write_stamp
}

# ---------------------------------------------------------------------------
# The quiet half: states that are NOT drift must stay silent.
# ---------------------------------------------------------------------------

@test "applied and stamped is not drift" {
    in_sync
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"no patch drift"* ]]
}

@test "fresh clone with no stamp and a pristine submodule is not drift" {
    # Nothing applied, nothing stamped. This is a clone that has not built yet;
    # the ordinary build flow applies the patches. Failing here would fail every
    # first build.
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"no patch drift"* ]]
}

@test "an exempt patch is not treated as unapplied" {
    in_sync
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" != *"zeta.patch"* ]]
}

@test "an absent submodule directory is reported, not failed" {
    rm -rf "$SUB"
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"lib/fake"* ]]
}

@test "a submodule that is not a git checkout is reported, not failed" {
    # Docker and rsync'd source trees have no .git; there is nothing to compare
    # against and nothing to repair from.
    in_sync
    rm -rf "$SUB/.git"
    run_gate
    [ "$status" -eq 0 ]
}

@test "rewriting a patch and reapplying it clears the drift" {
    in_sync
    printf 'one\nALPHA\nALPHA2\n' > "$SUB/one.txt"
    git -C "$SUB" diff -- one.txt > "$ROOT/patches/alpha.patch"
    run_gate
    [ "$status" -eq 1 ]
    # The submodule already carries the new content, so a re-stamp is honest here.
    write_stamp
    run_gate
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# The catch half.
# ---------------------------------------------------------------------------

@test "editing an applied patch is drift" {
    in_sync
    # Exactly the 86560d156 shape: the patch grows a hunk, the submodule keeps
    # the old content, and mk/patches.mk's dirty guard says "already applied".
    printf 'one\nALPHA\nALPHA_NEW\n' > "$SUB/one.txt"
    git -C "$SUB" diff -- one.txt > "$ROOT/patches/alpha.patch"
    printf 'one\nALPHA\n' > "$SUB/one.txt"

    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"alpha.patch"* ]]
    [[ "$output" == *"edited since"* ]]
    [[ "$output" == *"make reapply-patches"* ]]
}

@test "a patch added since the last apply is drift" {
    in_sync
    printf 'one\nALPHA\nDELTA\n' > "$SUB/one.txt"
    git -C "$SUB" diff -- one.txt > "$ROOT/patches/delta.patch"
    git -C "$SUB" checkout -- one.txt
    git -C "$SUB" apply "$ROOT/patches/alpha.patch"
    write_patches_mk alpha beta gamma delta

    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"delta.patch"* ]]
    [[ "$output" == *"never applied"* ]]
}

@test "a patched submodule file changing under the stamp is drift" {
    in_sync
    printf 'one\nALPHA\nhand edit\n' > "$SUB/one.txt"

    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"one.txt"* ]]
    [[ "$output" == *"changed since"* ]]
}

@test "a patch-created file disappearing under the stamp is drift" {
    in_sync
    rm -f "$SUB/three.txt"

    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"three.txt"* ]]
}

@test "resetting the submodule under the stamp is drift" {
    in_sync
    git -C "$SUB" checkout -- one.txt two.txt
    rm -f "$SUB/three.txt"

    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"one.txt"* ]]
    [[ "$output" == *"two.txt"* ]]
}

@test "no stamp with a dirty submodule is drift" {
    # Patches applied by a build that predates stamping. Nothing can say which
    # revision of each patch is in there, so it has to be rebuilt from pristine.
    apply_all
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"no-stamp"* ]]
    [[ "$output" == *"make reapply-patches"* ]]
}

@test "no stamp with only a patch-created file present is drift" {
    # git diff sees nothing here - the created file is untracked. A dirty check
    # that only asked git diff would call this a pristine clone.
    git -C "$SUB" apply "$ROOT/patches/gamma.patch"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"no-stamp"* ]]
}

@test "deleting a patch that is still applied is drift" {
    in_sync
    rm -f "$ROOT/patches/beta.patch"
    write_patches_mk alpha gamma

    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"beta.patch"* ]]
    [[ "$output" == *"gone from patches/"* ]]
}

# ---------------------------------------------------------------------------
# --pre-apply: the mode mk/patches.mk runs BEFORE the apply blocks.
# ---------------------------------------------------------------------------

@test "--pre-apply still fails an edited patch" {
    in_sync
    printf 'one\nALPHA\nALPHA_NEW\n' > "$SUB/one.txt"
    git -C "$SUB" diff -- one.txt > "$ROOT/patches/alpha.patch"
    printf 'one\nALPHA\n' > "$SUB/one.txt"

    run_gate --pre-apply
    [ "$status" -eq 1 ]
    [[ "$output" == *"alpha.patch"* ]]
}

@test "--pre-apply allows a brand new patch through" {
    # A new patch's guard tests a marker that is not in the tree, so the apply
    # blocks about to run WILL apply it. An edited patch's guard tests a marker
    # the old revision already left behind, which is why that one still fails.
    in_sync
    printf 'one\nALPHA\nDELTA\n' > "$SUB/one.txt"
    git -C "$SUB" diff -- one.txt > "$ROOT/patches/delta.patch"
    git -C "$SUB" checkout -- one.txt
    git -C "$SUB" apply "$ROOT/patches/alpha.patch"
    write_patches_mk alpha beta gamma delta

    run_gate --pre-apply
    [ "$status" -eq 0 ]

    # ...and the ungated mode still calls it out.
    run_gate
    [ "$status" -eq 1 ]
}

@test "--pre-apply still fails a missing stamp over a dirty submodule" {
    apply_all
    run_gate --pre-apply
    [ "$status" -eq 1 ]
}

# ---------------------------------------------------------------------------
# --list
# ---------------------------------------------------------------------------

@test "--list prints one bare finding per line" {
    in_sync
    printf 'one\nALPHA\nALPHA_NEW\n' > "$SUB/one.txt"
    git -C "$SUB" diff -- one.txt > "$ROOT/patches/alpha.patch"
    printf 'one\nALPHA\n' > "$SUB/one.txt"
    printf 'two\nBETA\nhand edit\n' > "$SUB/two.txt"

    run_gate --list
    [ "$status" -eq 1 ]
    [[ "$output" == *"patch-edited lib/fake alpha.patch"* ]]
    [[ "$output" == *"file-changed lib/fake two.txt"* ]]
    # No decoration, no remedy paragraph.
    [[ "$output" != *"make reapply-patches"* ]]
}

@test "--list is silent and zero when in sync" {
    in_sync
    run_gate --list
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------------------------------------------------------------------------
# The stamp itself
# ---------------------------------------------------------------------------

@test "the stamp lives in the submodule git dir, not its working tree" {
    in_sync
    [ -f "$SUB/.git/helix-patches-applied.json" ]
    # Nothing new shows up in the submodule's own status.
    run git -C "$SUB" status --porcelain --untracked-files=all
    [[ "$output" != *"helix-patches-applied"* ]]
}

@test "the stamp records both patch hashes and submodule file hashes" {
    in_sync
    run python3 -c "
import json,sys
s=json.load(open('$SUB/.git/helix-patches-applied.json'))
assert sorted(s['patches']) == ['alpha.patch','beta.patch','gamma.patch'], s['patches']
assert 'one.txt' in s['files'] and 'three.txt' in s['files'], s['files']
print('ok')
"
    [ "$status" -eq 0 ]
}

@test "--clear-stamp forgets the recorded state" {
    # `make reset-patches` restores the submodule to pristine, which invalidates
    # the stamp; leaving it behind would make the very next check report every
    # file as changed.
    in_sync
    run python3 "$GATE" --repo-root "$ROOT" --clear-stamp
    [ "$status" -eq 0 ]
    [ ! -f "$SUB/.git/helix-patches-applied.json" ]

    # Pristine again + no stamp is the grace case, so reapply-patches can run.
    git -C "$SUB" checkout -- one.txt two.txt
    rm -f "$SUB/three.txt"
    run_gate
    [ "$status" -eq 0 ]
}

@test "--write-stamp is what the real repo's gate reads back" {
    # End to end against the real tree: whatever state lib/ is in, the gate must
    # produce a verdict rather than a traceback.
    run python3 "$GATE"
    [ "$status" -eq 0 ] || [ "$status" -eq 1 ]
    [[ "$output" == *"patch drift"* ]]
}

# A pre-commit hook exports GIT_DIR/GIT_WORK_TREE pointing at the superproject.
# Those leak into `git -C <submodule>`, which then resolves to the superproject
# git dir instead of the submodule's, so the stamp is looked for where one never
# exists and the gate cries [no-stamp] on every commit. It passed standalone and
# failed only under the hook, which reads as flaky rather than broken.
@test "a hook's leaked GIT_DIR does not fake a missing stamp" {
    in_sync
    run env GIT_DIR="$ROOT/.git" GIT_WORK_TREE="$ROOT" \
        python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 0 ]
    [[ "$output" != *"no-stamp"* ]]
}

@test "a hook's leaked GIT_DIR does not mask real drift" {
    in_sync
    printf '\n# edited after stamping\n' >> "$ROOT/patches/alpha.patch"
    run env GIT_DIR="$ROOT/.git" GIT_WORK_TREE="$ROOT" \
        python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 1 ]
    [[ "$output" == *"patch-edited"* ]]
}

# ---------------------------------------------------------------------------
# Reachability. The gate above is only worth what the build actually runs, and
# mk/patches.mk runs it from the $(PATCHES_STAMP) recipe - which is skipped
# whenever that stamp is up to date. The stamp is per-worktree; lib/ and the
# .git/modules that hold the applied-stamp are shared. So when ANOTHER worktree
# re-applies a different branch's patches, nothing in this tree's own patches/
# or submodule HEADs moves, the recipe is skipped, every apply guard greps a
# marker the foreign revision also has, and the tree compiles against a patch
# revision that is not the one in its patches/ - silently.
#
# These two run against the real repo rather than the fixture, because what
# they pin is this repo's wiring. Both are read-only: `make -pn` prints the
# database without running a recipe, and nothing here writes to lib/.

@test "the per-worktree patch stamp depends on the shared applied-stamp" {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    local problems=""

    grep -qE '^\$\(PATCHES_STAMP\):.*\$\(APPLIED_STAMP_ID\)' mk/patches.mk \
        || problems="the stamp rule does not list \$(APPLIED_STAMP_ID); "
    # The id is only a proxy: it has to be hashed FROM the applied-stamps, or it
    # never moves when another worktree re-patches.
    grep -qE '^APPLIED_STAMP_HASH := \$\(shell cat \$\(APPLIED_STAMPS\)' mk/patches.mk \
        || problems="${problems}the id is not derived from \$(APPLIED_STAMPS); "

    local db
    db="$(make -pn 2>/dev/null)"
    # Anchored: a path that merely starts with the name is a different file,
    # and make would be watching something the gate never writes.
    grep -qE 'modules/lvgl/helix-patches-applied\.json( |$)' <<<"$db" \
        || problems="${problems}no lvgl applied-stamp in the make database; "
    grep -qE 'modules/libhv/helix-patches-applied\.json( |$)' <<<"$db" \
        || problems="${problems}no libhv applied-stamp in the make database"

    [ -z "$problems" ] || { echo "$problems"; false; }
}

@test "make and the gate agree on the applied-stamp filename" {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    local name
    name="$(sed -n 's/^STAMP_NAME = "\(.*\)"$/\1/p' "$GATE")"
    [ -n "$name" ] || { echo "STAMP_NAME not found in $GATE"; false; }
    # mk/patches.mk spells the name out; a rename on either side would leave
    # make watching a path the gate never writes, and the recipe would go back
    # to being unreachable without anything failing.
    local esc="${name//./\\.}"
    grep -qE "/${esc}\$" mk/patches.mk \
        || { echo "mk/patches.mk does not end a path with $name"; false; }
}
