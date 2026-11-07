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

load helpers

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
    contains "alpha.patch" "$output"
    contains "edited since" "$output"
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
    contains "delta.patch" "$output"
    [[ "$output" == *"never applied"* ]]
}

@test "a patched submodule file changing under the stamp is drift" {
    in_sync
    printf 'one\nALPHA\nhand edit\n' > "$SUB/one.txt"

    run_gate
    [ "$status" -eq 1 ]
    contains "one.txt" "$output"
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
    contains "one.txt" "$output"
    [[ "$output" == *"two.txt"* ]]
}

@test "no stamp with a dirty submodule is drift" {
    # Patches applied by a build that predates stamping. Nothing can say which
    # revision of each patch is in there, so it has to be rebuilt from pristine.
    apply_all
    run_gate
    [ "$status" -eq 1 ]
    contains "no-stamp" "$output"
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
    contains "beta.patch" "$output"
    [[ "$output" == *"gone from patches/"* ]]
}

init_root_repo() {
    # The stale-stamp discrimination needs a superproject git can ask about:
    # commit the fixture's shelf and wiring so a deleted patch is either
    # ref-held (a real removal) or ref-less (never shipped anywhere).
    git -C "$ROOT" init -q
    git -C "$ROOT" config user.email t@example.invalid
    git -C "$ROOT" config user.name "Fixture"
    printf 'FAKE_DIR := lib/fake\n' > "$ROOT/Makefile"
    git -C "$ROOT" add Makefile mk/patches.mk patches
    git -C "$ROOT" commit -qm fixture
}

@test "a stamp entry no ref ever held is a stale entry, not a removed patch" {
    # An uncommitted ghost patch is applied and stamped, then deleted and
    # unwired. The stamp is the only place it ever existed; the patch-removed
    # wording would claim its hunks linger in the submodule and send someone
    # hunting for a patch that never shipped anywhere.
    init_root_repo
    printf 'ghost\n' > "$SUB/ghost.txt"
    git -C "$SUB" add -N ghost.txt
    git -C "$SUB" diff -- ghost.txt > "$ROOT/patches/ghost.patch"
    git -C "$SUB" reset -q -- ghost.txt
    rm -f "$SUB/ghost.txt"
    write_patches_mk alpha beta gamma ghost
    apply_all
    git -C "$SUB" apply "$ROOT/patches/ghost.patch"
    write_stamp
    rm -f "$ROOT/patches/ghost.patch"
    write_patches_mk alpha beta gamma

    run_gate
    [ "$status" -eq 1 ]
    contains "ghost.patch" "$output"
    contains "stale-stamp-entry" "$output"
    contains "exists in no ref" "$output"
    contains "to resync the stamp" "$output"
    lacks "patch-removed" "$output"
}

@test "a stamp entry a ref still holds keeps the patch-removed verdict" {
    # beta was committed before it was deleted: its hunks really do linger,
    # and the remedy is the ordinary removal repair, not a stamp resync.
    init_root_repo
    in_sync
    rm -f "$ROOT/patches/beta.patch"
    write_patches_mk alpha gamma

    run_gate
    [ "$status" -eq 1 ]
    contains "beta.patch" "$output"
    contains "patch-removed" "$output"
    lacks "stale-stamp-entry" "$output"
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
    contains "patch-edited lib/fake alpha.patch" "$output"
    contains "file-changed lib/fake two.txt" "$output"
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
    grep -qE '^APPLIED_STAMP_HASH_CMD = cat .*\$\(APPLIED_STAMPS\)' mk/patches.mk \
        || problems="${problems}the id is not derived from \$(APPLIED_STAMPS); "
    grep -qE '^APPLIED_STAMP_HASH := \$\(shell \$\(APPLIED_STAMP_HASH_CMD\)\)' mk/patches.mk \
        || problems="${problems}the id is not the hash of that command; "

    # `help` has no prerequisites: it prints the database without make first
    # deciding whether the default goal can be built, which on a host with no
    # compiled dependencies it cannot. Only APPLIED_STAMPS feeds the id, so a
    # stamp variable defined but left out of it watches nothing.
    local db
    db="$(make_db | grep '^APPLIED_STAMPS := ')"
    # Anchored: a path that merely starts with the name is a different file,
    # and make would be watching something the gate never writes.
    grep -qE 'modules/lvgl/helix-patches-applied\.json( |$)' <<<"$db" \
        || problems="${problems}no lvgl applied-stamp in the make database; "
    grep -qE 'modules/libhv/helix-patches-applied\.json( |$)' <<<"$db" \
        || problems="${problems}no libhv applied-stamp in the make database; "
    grep -qE 'modules/lib/lua/helix-patches-applied\.json( |$)' <<<"$db" \
        || problems="${problems}no lua applied-stamp in the make database"

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

# ---------------------------------------------------------------------------
# Which remedy gets printed. `make reapply-patches` repairs whatever checkout
# the submodule path resolves to, so the advice is only safe when that path is
# this tree's own. A tree sharing another's checkout is also the tree most
# likely to be reported, since patches/ is per-branch.
# ---------------------------------------------------------------------------

# Move the fixture submodule outside the repo and leave a symlink behind: the
# shape of a worktree whose lib/ entry points into another tree's checkout.
share_sub_from_another_tree() {
    SHARED_SUB="${BATS_TEST_TMPDIR:-$(mktemp -d)}/other-tree/lib/fake"
    mkdir -p "$(dirname "$SHARED_SUB")"
    mv "$SUB" "$SHARED_SUB"
    ln -s "$SHARED_SUB" "$SUB"
}

# The drift used by both cases below: an applied patch grows a hunk the
# submodule does not carry.
edit_an_applied_patch() {
    printf 'one\nALPHA\nALPHA_NEW\n' > "$SUB/one.txt"
    git -C "$SUB" diff -- one.txt > "$ROOT/patches/alpha.patch"
    printf 'one\nALPHA\n' > "$SUB/one.txt"
}

@test "drift in a symlinked submodule does not tell you to reapply" {
    in_sync
    share_sub_from_another_tree
    edit_an_applied_patch

    run_gate
    [ "$status" -eq 1 ]
    contains "Do NOT run" "$output"
    # Naming the real target is the point: it is the tree that would be damaged.
    contains "$SHARED_SUB" "$output"
    contains "setup-worktree.sh --setup-only" "$output"
    case "$output" in
        *"Fix: make reapply-patches"*)
            printf 'symlinked tree was told to reapply:\n%s\n' "$output" >&2
            return 1
            ;;
    esac
}

@test "drift in a private submodule still tells you to reapply" {
    in_sync
    edit_an_applied_patch

    run_gate
    [ "$status" -eq 1 ]
    contains "Fix: make reapply-patches" "$output"
    case "$output" in
        *"Do NOT run"*)
            printf 'private checkout was refused the reapply remedy:\n%s\n' "$output" >&2
            return 1
            ;;
    esac
}

@test "a symlinked submodule with no drift is not warned about" {
    in_sync
    share_sub_from_another_tree

    run_gate
    [ "$status" -eq 0 ]
    case "$output" in
        *"Do NOT run"*)
            printf 'clean tree got a remedy it has no use for:\n%s\n' "$output" >&2
            return 1
            ;;
    esac
}

# --- where the pre-push hook asks the question (prestonbrown/helixscreen#1471) ---
#
# The hook's throwaway checkout symlinks lib/ back to the primary tree, so its
# submodules carry the primary branch's applied patches. Running the gate there
# measures one branch's patches/ against another branch's lib/.

@test "qc_patch_drift defers when the caller says lib/ is borrowed" {
    . scripts/qc/patch_drift.sh
    section_time() { :; }
    HELIX_QC_SKIP_PATCH_DRIFT=1 run qc_patch_drift
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "deferred to the tree that owns lib/"
}

@test "qc_patch_drift runs the real gate when nothing defers it" {
    . scripts/qc/patch_drift.sh
    section_time() { :; }
    run qc_patch_drift
    ! echo "$output" | grep -q "deferred to the tree that owns lib/"
    echo "$output" | grep -q "patch drift"
}

@test "pre-push defers drift in the isolated sweep and re-asks in the primary tree" {
    # The sweep invocation spans lines, so assert the flag and the invocation
    # separately rather than pinning one formatting of the command.
    grep -q 'HELIX_QC_SKIP_PATCH_DRIFT=1' .githooks/pre-push
    grep -q './scripts/quality-checks.sh' .githooks/pre-push
    [ "$(grep -c 'prepush_patch_drift "$REPO_ROOT"' .githooks/pre-push)" -eq 2 ]
    grep -q 'cd "$tree" && python3 scripts/check_patch_drift.py' .githooks/lib/push-range.sh
}

@test "pre-push blames the working tree, not the pushed commit, for drift" {
    grep -q 'Patch drift is in YOUR WORKING TREE' .githooks/lib/push-range.sh
}

# --- the clang gate has the same borrowed-tree problem ---
#
# check_clang_diagnostics.py derives its repo root from its own path and needs a
# compile database there. The isolated checkout is never built, so the gate
# declines rather than reporting a vacuous green, and the hook re-asks it in the
# tree that has a database.

@test "qc_clang_divergence_deferred is set by the env flag" {
    . scripts/qc/decl_ui.sh
    HELIX_QC_SKIP_CLANG_DIVERGENCE=1 qc_clang_divergence_deferred
}

@test "qc_clang_divergence_deferred is false without the flag" {
    . scripts/qc/decl_ui.sh
    unset HELIX_QC_SKIP_CLANG_DIVERGENCE
    ! qc_clang_divergence_deferred
}

@test "the clang section consults the predicate before running the gate" {
    # The deferral arm must come first, or an unbuilt isolated tree still fails.
    local sec
    sec=$(awk '/Checking clang\/GCC divergence/,/check_clang_diagnostics.py not found/' scripts/qc/decl_ui.sh)
    contains "if qc_clang_divergence_deferred; then" "$sec"
    contains 'elif [ "$STAGED_ONLY" = false ]' "$sec"
}

@test "pre-push defers the clang gate in the sweep and re-asks in the primary tree" {
    grep -q 'HELIX_QC_SKIP_CLANG_DIVERGENCE=1' .githooks/pre-push
    grep -q 'cd "$REPO_ROOT" && python3 scripts/check_clang_diagnostics.py' .githooks/pre-push
}

@test "pre-push lends build/bin only to a commit that descends from this tree" {
    # Another line's commit validated by this branch's binaries reports this
    # branch's constants as missing from that line's XML.
    grep -q 'build/bin" \] && git merge-base --is-ancestor HEAD "\$TARGET"' .githooks/pre-push
}

@test "a branch whose quality-checks.sh predates the deferral is still pushable" {
    # The hook only exports the flag; a script that has never heard of it must
    # ignore it rather than fail, so an older branch stays pushable.
    run env HELIX_QC_SKIP_CLANG_DIVERGENCE=1 bash -c '
        STAGED_ONLY=false
        section_time() { :; }
        # Stand in for a pre-deferral script: the plain guard, no predicate.
        if [ "$STAGED_ONLY" = false ] && [ -f "scripts/check_clang_diagnostics.py" ]; then
            echo "gate would run"
        fi'
    [ "$status" -eq 0 ]
    [[ "$output" == *"gate would run"* ]]
}

# --- --pending-since: drift the pushed commits explain ---
#
# A landing that adds or edits a patch is pushed from a tree that has not built
# it yet. Drift confined to patches the pushed range changed is that tree being
# behind, not a fault in the commits; everything else still fails.

commit_root() {
    git -C "$ROOT" add Makefile mk/patches.mk patches
    git -C "$ROOT" commit -qm "$1"
}

# delta.patch on the shelf and wired, committed; nothing applies it.
commit_new_delta_patch() {
    printf 'one\nALPHA\nDELTA\n' > "$SUB/one.txt"
    git -C "$SUB" diff -- one.txt > "$ROOT/patches/delta.patch"
    printf 'one\nALPHA\n' > "$SUB/one.txt"
    write_patches_mk alpha beta gamma delta
    commit_root "add delta"
}

# alpha.patch grows a hunk the applied submodule does not carry, committed.
commit_edited_alpha_patch() {
    printf 'one\nALPHA\nALPHA_NEW\n' > "$SUB/one.txt"
    git -C "$SUB" diff -- one.txt > "$ROOT/patches/alpha.patch"
    printf 'one\nALPHA\n' > "$SUB/one.txt"
    commit_root "edit alpha"
}

@test "--pending-since: a new patch the range adds is a note, not drift" {
    init_root_repo
    in_sync
    base=$(git -C "$ROOT" rev-parse HEAD)
    commit_new_delta_patch

    run_gate --pending-since "$base"
    [ "$status" -eq 0 ]
    contains "pushed commits explain" "$output"
    contains "[patch-unapplied] lib/fake delta.patch" "$output"
    contains 'The next `make` here applies the new patches' "$output"
    lacks "❌" "$output"
}

@test "--pending-since: no rev given keeps the plain verdict on the same tree" {
    init_root_repo
    in_sync
    commit_new_delta_patch

    run_gate
    [ "$status" -eq 1 ]
    contains "never applied" "$output"
    lacks "pushed commits explain" "$output"
}

@test "--pending-since: an edited patch the range changes is a note naming reapply" {
    init_root_repo
    in_sync
    base=$(git -C "$ROOT" rev-parse HEAD)
    commit_edited_alpha_patch

    run_gate --pending-since "$base"
    [ "$status" -eq 0 ]
    contains "[patch-edited] lib/fake alpha.patch" "$output"
    contains "make reapply-patches" "$output"
}

@test "--pending-since: a removed patch the range deletes is a note" {
    init_root_repo
    in_sync
    base=$(git -C "$ROOT" rev-parse HEAD)
    git -C "$ROOT" rm -q patches/beta.patch
    write_patches_mk alpha gamma
    commit_root "drop beta"

    run_gate --pending-since "$base"
    [ "$status" -eq 0 ]
    contains "[patch-removed] lib/fake beta.patch" "$output"
}

@test "--pending-since: a patch the range renames explains both names" {
    init_root_repo
    in_sync
    base=$(git -C "$ROOT" rev-parse HEAD)
    git -C "$ROOT" mv patches/beta.patch patches/beta2.patch
    write_patches_mk alpha beta2 gamma
    commit_root "rename beta"

    run_gate --pending-since "$base"
    [ "$status" -eq 0 ]
    contains "[patch-removed] lib/fake beta.patch" "$output"
    contains "[patch-unapplied] lib/fake beta2.patch" "$output"
}

@test "--pending-since: an uncommitted edit to a patch the range changed still fails" {
    # The range explains the committed revision; bytes on disk that differ
    # from the pushed commit are this tree's own edit, not the push.
    init_root_repo
    in_sync
    base=$(git -C "$ROOT" rev-parse HEAD)
    commit_edited_alpha_patch
    printf '# in flight\n' >> "$ROOT/patches/alpha.patch"

    run_gate --pending-since "$base"
    [ "$status" -eq 1 ]
    contains "[patch-edited] lib/fake alpha.patch" "$output"
    lacks "pushed commits explain" "$output"
}

@test "--pending-since: a patch the range removed but still on disk fails" {
    init_root_repo
    in_sync
    base=$(git -C "$ROOT" rev-parse HEAD)
    git -C "$ROOT" rm -q --cached patches/beta.patch
    write_patches_mk alpha gamma
    git -C "$ROOT" add mk/patches.mk
    git -C "$ROOT" commit -qm "drop beta"
    [ -z "$(git -C "$ROOT" ls-files patches/beta.patch)" ]
    # Unwired, so it reads as removed, but its file is still on disk.
    [ -f "$ROOT/patches/beta.patch" ]

    run_gate --pending-since "$base"
    [ "$status" -eq 1 ]
    contains "[patch-removed] lib/fake beta.patch" "$output"
}

@test "--pending-since: a same-named file in a patches/ subdirectory explains nothing" {
    init_root_repo
    in_sync
    commit_edited_alpha_patch
    base=$(git -C "$ROOT" rev-parse HEAD)
    mkdir -p "$ROOT/patches/old"
    printf 'archived\n' > "$ROOT/patches/old/alpha.patch"
    commit_root "archive"

    run_gate --pending-since "$base"
    [ "$status" -eq 1 ]
    contains "[patch-edited] lib/fake alpha.patch" "$output"
}

@test "--pending-since: an edited patch outside the range still fails" {
    init_root_repo
    in_sync
    commit_edited_alpha_patch
    base=$(git -C "$ROOT" rev-parse HEAD)
    printf '# unrelated\n' >> "$ROOT/Makefile"
    commit_root "unrelated"

    run_gate --pending-since "$base"
    [ "$status" -eq 1 ]
    contains "[patch-edited] lib/fake alpha.patch" "$output"
    contains "make reapply-patches" "$output"
    lacks "pushed commits explain" "$output"
}

@test "--pending-since: a changed submodule file fails even with a patch in range" {
    init_root_repo
    in_sync
    base=$(git -C "$ROOT" rev-parse HEAD)
    commit_new_delta_patch
    printf 'two\nBETA\nhand edit\n' > "$SUB/two.txt"

    run_gate --pending-since "$base"
    [ "$status" -eq 1 ]
    contains "[file-changed] lib/fake two.txt" "$output"
    # The explained half is still named, so the pusher sees both.
    contains "[patch-unapplied] lib/fake delta.patch" "$output"
    contains "pushed commits explain" "$output"
}

@test "--pending-since: only patch findings are explained, whatever the name" {
    # A changed submodule file is never explained by the range, even when a
    # patches/ file the range touched happens to share its name.
    init_root_repo
    in_sync
    base=$(git -C "$ROOT" rev-parse HEAD)
    printf 'notes\n' > "$ROOT/patches/two.txt"
    commit_root "shelf note"
    printf 'two\nBETA\nhand edit\n' > "$SUB/two.txt"

    run_gate --pending-since "$base"
    [ "$status" -eq 1 ]
    contains "[file-changed] lib/fake two.txt" "$output"
}

@test "--pending-since: a missing stamp over a dirty submodule still fails" {
    init_root_repo
    apply_all
    base=$(git -C "$ROOT" rev-parse HEAD)
    commit_new_delta_patch

    run_gate --pending-since "$base"
    [ "$status" -eq 1 ]
    contains "no-stamp" "$output"
}

@test "--pending-since: a rev git cannot resolve treats every finding as drift" {
    init_root_repo
    in_sync
    commit_new_delta_patch

    run_gate --pending-since 0123456789abcdef0123456789abcdef01234567
    [ "$status" -eq 1 ]
    contains "could not diff patches/" "$output"
    contains "never applied" "$output"
}

@test "--pending-head bounds the range, not HEAD" {
    init_root_repo
    in_sync
    base=$(git -C "$ROOT" rev-parse HEAD)
    commit_new_delta_patch
    head=$(git -C "$ROOT" rev-parse HEAD)

    run_gate --pending-since "$base" --pending-head "$base"
    [ "$status" -eq 1 ]
    run_gate --pending-since "$base" --pending-head "$head"
    [ "$status" -eq 0 ]
}

# --- the pre-push hook, end to end against the fixture ---
#
# The fixture gets its own copy of the gate and a stand-in quality-checks.sh
# that reports whether the hook deferred drift to its own follow-up.

setup_hook_fixture() {
    init_root_repo
    mkdir -p "$ROOT/scripts"
    cp "$GATE" "$ROOT/scripts/check_patch_drift.py"
    cp "$(dirname "$GATE")/own_git_dir.sh" "$ROOT/scripts/own_git_dir.sh"
    printf '#!/bin/sh\necho "qc-sweep skip-drift=${HELIX_QC_SKIP_PATCH_DRIFT:-unset}"\n' \
        > "$ROOT/scripts/quality-checks.sh"
    chmod +x "$ROOT/scripts/quality-checks.sh"
    # The hook links each lib/ submodule into its throwaway checkout, which
    # needs lib/ to be a tracked directory there, as it is in the real repo.
    touch "$ROOT/lib/.keep"
    git -C "$ROOT" add scripts lib/.keep
    git -C "$ROOT" commit -qm scripts
}

HOOK="$BATS_TEST_DIRNAME/../../.githooks/pre-push"

run_hook() {
    local target remote="$1"
    target=$(git -C "$ROOT" rev-parse HEAD)
    run env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_OBJECT_DIRECTORY \
        bash -c 'cd "$1" && printf "refs/heads/main %s refs/heads/main %s\n" "$2" "$3" | bash "$4"' \
        _ "$ROOT" "$target" "$remote" "$HOOK"
}

@test "pre-push fast path: a pushed new patch is a note and the push proceeds" {
    setup_hook_fixture
    in_sync
    base=$(git -C "$ROOT" rev-parse HEAD)
    commit_new_delta_patch

    run_hook "$base"
    [ "$status" -eq 0 ]
    contains "full quality sweep" "$output"
    contains "qc-sweep skip-drift=1" "$output"
    contains "[patch-unapplied] lib/fake delta.patch" "$output"
    lacks "YOUR WORKING TREE" "$output"
}

@test "pre-push fast path: drift the push does not explain still rejects it" {
    setup_hook_fixture
    in_sync
    commit_edited_alpha_patch
    base=$(git -C "$ROOT" rev-parse HEAD)
    printf '# unrelated\n' >> "$ROOT/Makefile"
    commit_root "unrelated"

    run_hook "$base"
    [ "$status" -eq 1 ]
    contains "[patch-edited] lib/fake alpha.patch" "$output"
    contains "YOUR WORKING TREE" "$output"
}

@test "pre-push isolated path: a pushed edited patch is a note and the push proceeds" {
    setup_hook_fixture
    in_sync
    base=$(git -C "$ROOT" rev-parse HEAD)
    commit_edited_alpha_patch
    printf '# in flight\n' >> "$ROOT/Makefile"

    run_hook "$base"
    [ "$status" -eq 0 ]
    contains "throwaway checkout" "$output"
    contains "qc-sweep skip-drift=1" "$output"
    contains "[patch-edited] lib/fake alpha.patch" "$output"
}

@test "pre-push isolated path: a changed submodule file still rejects the push" {
    setup_hook_fixture
    in_sync
    base=$(git -C "$ROOT" rev-parse HEAD)
    commit_new_delta_patch
    printf 'two\nBETA\nhand edit\n' > "$SUB/two.txt"
    printf '# in flight\n' >> "$ROOT/Makefile"

    run_hook "$base"
    [ "$status" -eq 1 ]
    contains "[file-changed] lib/fake two.txt" "$output"
    contains "YOUR WORKING TREE" "$output"
}

@test "pre-push with no base to measure from asks the plain question" {
    # A new branch in a clone with no origin/main has no range: nothing is
    # explained, so a pushed new patch fails as it does in the commit gate.
    setup_hook_fixture
    in_sync
    commit_new_delta_patch

    run_hook 0000000000000000000000000000000000000000
    [ "$status" -eq 1 ]
    contains "never applied" "$output"
}

# --- push_base: where the pushed range starts ---

PUSH_RANGE_LIB="$BATS_TEST_DIRNAME/../../.githooks/lib/push-range.sh"

# main and release/1.0 fork at m0; feat is cut from release/1.0.
setup_fork_fixture() {
    P="$BATS_TEST_TMPDIR/forks"
    git init -q -b main "$P"
    git -C "$P" config user.email t@example.invalid
    git -C "$P" config user.name Fixture
    git -C "$P" commit -q --allow-empty -m m0
    M0=$(git -C "$P" rev-parse HEAD)
    git -C "$P" checkout -q -b release/1.0
    git -C "$P" commit -q --allow-empty -m r1
    R1=$(git -C "$P" rev-parse HEAD)
    git -C "$P" checkout -q main
    git -C "$P" commit -q --allow-empty -m m1
    git -C "$P" update-ref refs/remotes/origin/main main
    git -C "$P" checkout -q -b feat release/1.0
    git -C "$P" commit -q --allow-empty -m f1
    F1=$(git -C "$P" rev-parse HEAD)
}

run_push_base() { # <remote sha> <target>
    run env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE \
        bash -c 'cd "$1" && . "$2" && push_base "$3" "$4"' _ "$P" "$PUSH_RANGE_LIB" "$1" "$2"
}

@test "push_base takes the remote's sha when this clone has it" {
    setup_fork_fixture
    run_push_base "$R1" "$F1"
    [ "$status" -eq 0 ]
    [ "$output" = "$R1" ]
}

@test "push_base measures a new branch from its nearest fork, not from main" {
    setup_fork_fixture
    mkdir -p "$P/scripts"
    cp scripts/diff_base.py "$P/scripts/"
    run_push_base 0000000000000000000000000000000000000000 "$F1"
    [ "$status" -eq 0 ]
    [ "$output" = "$R1" ]
}

@test "push_base in a tree without diff_base.py falls back to origin/main" {
    setup_fork_fixture
    run_push_base 0000000000000000000000000000000000000000 "$F1"
    [ "$status" -eq 0 ]
    [ "$output" = "$M0" ]
}

@test "diff_base.py --head measures from the named rev, not HEAD" {
    setup_fork_fixture
    git -C "$P" checkout -q main
    run python3 scripts/diff_base.py --root "$P" --head "$F1"
    [ "$status" -eq 0 ]
    [ "$output" = "$R1" ]
}
