#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# pre-push runs no quality sweep for a work-branch push (patch drift only), and a
# push that reaches main, release/* or a tag with everything. The hook runs against
# a fixture repo whose gates are stand-ins that print their name and the mode.

load helpers

HOOK="$BATS_TEST_DIRNAME/../../.githooks/pre-push"

setup() {
    export HELIX_LAND_GATE_STAMP_DIR="$BATS_TEST_TMPDIR/stamps"
    export HELIX_LAND_GATE_SHARED_STAMP_DIR="$BATS_TEST_TMPDIR/shared-stamps"
    unset HELIX_PREPUSH_FULL HELIX_PREPUSH_IN_PLACE
    ROOT="$BATS_TEST_TMPDIR/repo"
    git init -q -b main "$ROOT"
    git -C "$ROOT" config user.email t@t
    git -C "$ROOT" config user.name t
    mkdir -p "$ROOT/scripts" "$ROOT/lib"
    printf '#!/bin/sh\necho "ran: quality-checks logs=$HELIX_QC_LOG_DIR args=[$*]"\n' \
        >"$ROOT/scripts/quality-checks.sh"
    chmod +x "$ROOT/scripts/quality-checks.sh"
    local gate
    for gate in check_clang_diagnostics check_patch_drift; do
        printf 'print("ran: %s")\n' "$gate" >"$ROOT/scripts/$gate.py"
    done
    touch "$ROOT/lib/.keep"
    echo base >"$ROOT/f"
    git -C "$ROOT" add -A
    git -C "$ROOT" commit -qm base
    BASE=$(git -C "$ROOT" rev-parse HEAD)
    echo tip >"$ROOT/f"
    git -C "$ROOT" commit -qam tip
    TIP=$(git -C "$ROOT" rev-parse HEAD)
}

push() { # <remote ref>[=<sha>]...: one stdin line per ref, each pushing TIP (or <sha>) over BASE
    local lines="" ref sha
    for ref in "$@"; do
        sha=$TIP
        case "$ref" in *=*) sha=${ref#*=} ref=${ref%%=*} ;; esac
        lines="${lines}refs/heads/x $sha $ref $BASE"$'\n'
    done
    run env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_OBJECT_DIRECTORY \
        bash -c 'cd "$1" && printf "%s" "$2" | bash "$3"' _ "$ROOT" "$lines" "$HOOK"
}

@test "a push to main runs every gate, the clang divergence gate included" {
    echo dirty >"$ROOT/f" # the isolated path is where the clang gate runs separately
    push refs/heads/main
    [ "$status" -eq 0 ]
    contains "ran: quality-checks" "$output"
    # No --staged-only: every gate, the whole-tree ones a commit never ran.
    contains "logs=$(cd "$ROOT" && pwd -P)/build/qc-logs args=[]" "$output"
    lacks "skip_push_only" "$output"
    contains "ran: check_clang_diagnostics" "$output"
}

@test "a push to a release branch or a tag runs every gate" {
    push refs/heads/release/1.1
    [ "$status" -eq 0 ]
    contains "args=[]" "$output"
    push refs/tags/v1.2.0
    [ "$status" -eq 0 ]
    contains "args=[]" "$output"
}

@test "a work-branch push runs no quality sweep, and still asks patch drift" {
    echo dirty >"$ROOT/f"
    push refs/heads/fix/thing
    [ "$status" -eq 0 ] || fail "$output"
    lacks "ran: quality-checks" "$output"
    contains "a work-branch push runs no quality sweep" "$output"
    contains "commit hook judged each commit's staged files" "$output"
    contains "make land-gate and CI" "$output"
    contains "run every gate before anything reaches main" "$output"
    lacks "ran: check_clang_diagnostics" "$output"
    contains "ran: check_patch_drift" "$output"
}

@test "a work-branch push checks nothing out and sweeps nowhere" {
    echo dirty >"$ROOT/f"
    push refs/heads/fix/thing
    [ "$status" -eq 0 ] || fail "$output"
    lacks "working tree differs" "$output"
    lacks "Sweeping" "$output"
    [ "$(git -C "$ROOT" worktree list | wc -l)" -eq 1 ] || fail "a throwaway checkout was left behind"
}

@test "a work-branch push still fails on patch drift in the pushing tree" {
    printf 'import sys\nprint("ran: check_patch_drift")\nsys.exit(1)\n' >"$ROOT/scripts/check_patch_drift.py"
    push refs/heads/fix/thing
    [ "$status" -eq 1 ]
    contains "ran: check_patch_drift" "$output"
    contains "Patch drift is in YOUR WORKING TREE" "$output"
    lacks "ran: quality-checks" "$output"
}

@test "a push that updates a work branch and main together runs every gate" {
    push refs/heads/fix/thing refs/heads/main
    push refs/heads/main refs/heads/fix/thing
}

@test "HELIX_PREPUSH_FULL=1 sweeps a work branch fully" {
    HELIX_PREPUSH_FULL=1 push refs/heads/fix/thing
    [ "$status" -eq 0 ]
    contains "args=[]" "$output"
}

@test "a push that would sweep another ref's commit instead of main's is refused" {
    push refs/heads/main=$BASE refs/heads/fix/thing
    [ "$status" -eq 1 ]
    contains "would leave" "$output"
    lacks "ran: quality-checks" "$output"
    push refs/heads/fix/thing=$BASE refs/heads/main
    [ "$status" -eq 0 ]
}

@test "kept gate logs go to the pushing tree, not the throwaway checkout" {
    echo dirty >"$ROOT/f"
    push refs/heads/main
    [ "$status" -eq 0 ]
    contains "logs=$(cd "$ROOT" && pwd -P)/build/qc-logs" "$output"
}

